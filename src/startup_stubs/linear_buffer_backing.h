#pragma once

// Opt-in backing for one ordinary linear buffer kind placed at a title address (#1914, #874):
// device slot 133 (CreatePlacedResourceX) with a buffer description, row-major or unknown layout,
// and initial state COMMON. The title writes it with the CPU and the GPU reads it (vertex, index,
// constant, shader-resource or indirect-argument data). X2W_D3D12_LINEAR_BUFFERS selects it:
//
//   unset or 0  the default: slot 133 keeps its backing (an existing heap cached by allocation
//               base for a private range, otherwise a committed default buffer that does not alias
//               the title memory and holds zeros). Nothing in this file runs.
//   1           share when the adapter, the extent and the use allow it, else copy, else keep the
//               default backing and record why.
//   copy        never share; copy, else keep the default backing and record why.
//
// XMem owns the title memory (identity, mapping, offsets, lifetime); this file owns the desktop
// backing and the fences that say when the GPU is done with it.
//
// Share. Only when D3D12_FEATURE_EXISTING_HEAPS reports support (queried once per device, before
// any open), the whole buffer lies in one committed private allocation (MEM_PRIVATE) at a 64 KiB-
// aligned offset, and the opened heap covers it. The heap is cached by allocation base, as the
// default backing caches it: while it is open the range cannot be released or decommitted
// (VirtualFree fails with error 87, measured in #1942), so a later allocation cannot appear at that
// base under it. Each opened heap gets a generation. The GPU reads and writes the title memory
// itself; nothing is copied.
//
// An XMem view (MEM_MAPPED) is not shared: a heap opened from a view keeps it mapped, and XMem's
// unmap of those pages then fails with error 87 (tests/linear_buffer_backing_check.cpp measures it).
// Desktop resources live for the process, so the title could never unmap or free those pages again.
// Sharing reports HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) for a view and the buffer is copied. A
// buffer across several allocations, or at an unaligned offset, reports E_INVALIDARG. Both are
// decided without a D3D12 call.
//
// Copy. A committed default-heap buffer whose bytes are refreshed from the title memory at each
// submission through the command-queue wrapper, before the title's lists execute on that queue,
// never at bind or record. Only the bytes that differ from the last upload are copied, through an
// upload staging buffer. A staging buffer is reused only after the queue fence of the submission
// that read it has completed. The default buffer itself is rewritten in queue order, after every
// earlier submission on that queue; a title write to bytes that an incomplete submission still reads
// is an unsynchronized concurrent write, outside the contract.
//
// Where neither path can keep its guarantees, slot 133 keeps the default backing and returns what it
// returns without this switch. A title on the console does not see CreatePlacedResourceX fail, so a
// failed call would be a new failure (a title run that took the null object faulted at 1.76 s). The
// refusal is recorded instead: in the slot 133 log line ("kept default backing: linear refusal
// 0x..."), in x2w_d3d12_linear_buffer_info (path 3 and refusal_hr) and in the totals (kept_default,
// last_refusal_hr). The refusals:
//   - a GPU-written use (ALLOW_UNORDERED_ACCESS) that cannot be shared: the copy has no point at which
//     GPU writes reach the title memory. refusal_hr is the sharing HRESULT, or DXGI_ERROR_UNSUPPORTED
//     when sharing was not attempted (X2W_D3D12_LINEAR_BUFFERS=copy).
//   - no submit fence (X2W_TEST_SUBMIT_FENCE_FAIL=1, or the fence cannot be created): staging reuse
//     cannot wait for completion. E_NOT_VALID_STATE.
//   - the copy's default buffer cannot be created: that HRESULT.
//
// At submit nothing is refused: holding back a submission can leave a title waiting on its fences.
// When a copied buffer's refresh cannot be staged (a staging buffer or refresh list cannot be
// created, or the pool stays full of buffers in flight past a 5 s wait: DXGI_ERROR_WAS_STILL_DRAWING),
// the title's lists run with the backing's last upload. The buffer is marked stale until a later
// refresh succeeds; the HRESULT is logged (the first few times) and kept in the buffer's info and the
// totals (refresh_failures, last_refresh_hr).
//
// Fixtures only:
//   X2W_TEST_EXISTING_HEAPS_UNSUPPORTED=1  the feature query reports no support (an adapter such as
//                                          Intel Gen9 reports EXISTING_HEAPS = 0).
//   X2W_TEST_LINEAR_BUFFER_MUTANT=<name>   removes one guarantee, for negative controls:
//     skip-refresh     the copy is taken at placement (bind) and never refreshed at submit
//     reuse-early      a staging buffer is reused without waiting for its fence
//     copy-gpu-writes  the copy path accepts a GPU-written use
//     ignore-feature   sharing is attempted although the feature query reports no support
//     refusal-silent   a kept default backing is not reported (no log text, info or totals)
//     hold-lock        the wait for a full staging pool keeps mutex_ held (other callers block)
//   X2W_TEST_LINEAR_STAGING_LIMIT=<n>      the staging pool holds n buffers instead of 256
//   X2W_TEST_LINEAR_REFRESH_FAIL=<n>       staging requests 2, 2 + n, 2 + 2n, ... fail with E_OUTOFMEMORY

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "startup_stubs/linear_buffer_report.h"
#include "startup_stubs/local_log.h"
#include "startup_stubs/placed_buffer_range.h"

namespace x2w_startup {

enum class LinearBufferMode { off, automatic, copy };

inline LinearBufferMode linear_buffer_mode()
{
    static const LinearBufferMode mode = [] {
        const DWORD saved = GetLastError();
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_D3D12_LINEAR_BUFFERS", value, sizeof(value));
        SetLastError(saved);
        if (!n || n >= sizeof(value)) return LinearBufferMode::off;
        if (std::strcmp(value, "1") == 0) return LinearBufferMode::automatic;
        if (_stricmp(value, "copy") == 0) return LinearBufferMode::copy;
        return LinearBufferMode::off;
    }();
    return mode;
}

namespace linear_detail {
inline bool mutant(const char* name)
{
    static char value[32] = {};
    static const bool read = [] {
        const DWORD saved = GetLastError();
        const DWORD n = GetEnvironmentVariableA("X2W_TEST_LINEAR_BUFFER_MUTANT", value, sizeof(value));
        if (n >= sizeof(value)) value[0] = 0;
        SetLastError(saved);
        return true;
    }();
    (void)read;
    return value[0] && std::strcmp(value, name) == 0;
}
inline bool existing_heaps_forced_off()
{
    static const bool on = x2w_local::env_on("X2W_TEST_EXISTING_HEAPS_UNSUPPORTED");
    return on;
}
inline unsigned refresh_fault_period()
{
    static const unsigned period = [] {
        const DWORD saved = GetLastError();
        char value[16]{};
        const DWORD n = GetEnvironmentVariableA("X2W_TEST_LINEAR_REFRESH_FAIL", value, sizeof(value));
        SetLastError(saved);
        return n && n < sizeof(value) ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return period;
}
inline bool submit_fence_forced_off()
{
    static const bool on = GetEnvironmentVariableW(L"X2W_TEST_SUBMIT_FENCE_FAIL", nullptr, 0) != 0;
    return on;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 width, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}
}  // namespace linear_detail

class LinearBuffers {
    struct Staging;
    struct RefreshList;

public:
    enum Path : std::uint32_t { shared_path = 1, copied_path = 2, kept_default_path = 3 };
    enum Use : std::uint32_t { gpu_read = 1, gpu_written = 2 };

    // The kind this file handles. Anything else keeps the default backing.
    static bool eligible(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state)
    {
        return linear_buffer_mode() != LinearBufferMode::off && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
               (desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN) &&
               desc.Width && initial_state == D3D12_RESOURCE_STATE_COMMON;
    }

    // False under the refusal-silent mutant: a kept default backing is then not reported anywhere.
    static bool report_refusals() { return !linear_detail::mutant("refusal-silent"); }

    // Slot 133 for an eligible buffer. range is measure_placed_buffer's answer for (address, width) and
    // uav says whether the desktop flags keep ALLOW_UNORDERED_ACCESS. On success *out holds the
    // desktop resource and *shared whether it aliases the title memory. A failure is a refusal: the
    // caller keeps the default backing and passes *report (path kept_default_path) to kept_default().
    HRESULT place(ID3D12Device* device, void* address, UINT64 width, bool uav, const PlacedBufferRange& range,
                  ID3D12Resource** out, bool* shared, X2wLinearBufferInfo* report)
    {
        *out = nullptr;
        *shared = false;
        std::lock_guard lock(mutex_);
        ++totals_.placements;
        Record record{};
        record.info.use = uav ? gpu_written : gpu_read;
        record.info.feature_hr = S_FALSE;
        record.info.share_hr = S_FALSE;
        const UINT64 offset = static_cast<UINT64>(static_cast<char*>(address) - static_cast<char*>(range.base));
        ID3D12Resource* resource = nullptr;
        if (linear_buffer_mode() == LinearBufferMode::automatic) {
            record.info.feature_hr = feature(device);
            const bool allowed = SUCCEEDED(record.info.feature_hr) || linear_detail::mutant("ignore-feature");
            if (!allowed)
                record.info.share_hr = record.info.feature_hr;
            else if (!range.covered || (offset & 0xFFFF))
                record.info.share_hr = E_INVALIDARG;  // decided without D3D12: each failed open throws in D3D12Core
            else if (range.type == MEM_MAPPED)
                record.info.share_hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);  // an open heap would pin the view
            else if (range.type != MEM_PRIVATE)
                record.info.share_hr = E_INVALIDARG;
            else
                record.info.share_hr = share(device, range, offset, width, uav, &resource, &record.info.heap_generation);
        }
        HRESULT hr = S_OK;
        if (SUCCEEDED(record.info.share_hr) && record.info.share_hr != S_FALSE) {
            record.info.path = shared_path;
            ++totals_.shared;
        } else if (uav && !linear_detail::mutant("copy-gpu-writes")) {
            hr = record.info.share_hr == S_FALSE ? DXGI_ERROR_UNSUPPORTED : record.info.share_hr;
        } else if (linear_detail::submit_fence_forced_off() || FAILED(fence_check(device))) {
            hr = E_NOT_VALID_STATE;
        } else {
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            const auto desc = linear_detail::buffer(width, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                               : D3D12_RESOURCE_FLAG_NONE);
            hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&resource));
            if (SUCCEEDED(hr)) {
                try {
                    record.copy = std::make_shared<Copied>();
                    record.copy->title = static_cast<const unsigned char*>(address);
                    record.copy->bytes = width;
                    if (linear_detail::mutant("skip-refresh")) {
                        // Mutant: the bytes at placement are what every submission gets.
                        const std::size_t readable = x2w_local::probe_readable(reinterpret_cast<std::uintptr_t>(address),
                                                                               static_cast<std::size_t>(width));
                        record.copy->bound.assign(record.copy->title, record.copy->title + readable);
                        record.copy->bound.resize(static_cast<std::size_t>(width));
                    }
                } catch (const std::bad_alloc&) {
                    resource->Release();
                    resource = nullptr;
                    hr = E_OUTOFMEMORY;
                }
            }
            if (SUCCEEDED(hr)) {
                record.info.path = copied_path;
                ++totals_.copied;
            }
        }
        if (SUCCEEDED(hr)) {
            record.info.backing = resource;
            try {
                records_.emplace(resource, std::move(record));
            } catch (const std::bad_alloc&) {
                hr = E_OUTOFMEMORY;
                --(record.info.path == shared_path ? totals_.shared : totals_.copied);
            }
        }
        if (FAILED(hr)) {
            if (resource) resource->Release();
            record.info.path = kept_default_path;
            record.info.refusal_hr = hr;
            record.info.backing = nullptr;
            if (report) *report = record.info;
            return hr;
        }
        const auto& stored = records_.at(resource).info;
        if (report) *report = stored;
        const bool is_shared = stored.path == shared_path;
        *out = resource;
        *shared = is_shared;
        return S_OK;
    }

    // Slot 133 kept the default backing for a refused placement: record why, for this resource.
    void kept_default(ID3D12Resource* resource, const X2wLinearBufferInfo& report)
    {
        if (!report_refusals()) return;
        std::lock_guard lock(mutex_);
        ++totals_.kept_default;
        totals_.last_refusal_hr = report.refusal_hr;
        try {
            Record record{};
            record.info = report;
            record.info.path = kept_default_path;
            record.info.backing = resource;
            records_.insert_or_assign(resource, std::move(record));
        } catch (const std::bad_alloc&) {
        }
    }

    // The resource never reached the title and is about to be released.
    void discard(ID3D12Resource* resource)
    {
        std::lock_guard lock(mutex_);
        records_.erase(resource);
    }

    // The title released the resource: its memory is not read again. The desktop backing stays (as
    // every wrapper's does); a shared one keeps its heap.
    void retire(ID3D12Resource* resource)
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(resource);
        if (found == records_.end() || !found->second.copy) return;
        found->second.copy->live = false;
        std::vector<unsigned char>().swap(found->second.copy->uploaded);
    }

    // What one submission used: released by the queue fence value that after_submit signals.
    struct Submission {
        std::vector<Staging*> staging;
        RefreshList* list = nullptr;
    };

    // Before the title's lists are executed on queue (the contract's copy point): records and
    // executes the refresh of every live copied buffer whose title bytes changed. Nothing here holds
    // the submission back: a buffer whose refresh fails keeps its last upload and is marked stale.
    //
    // mutex_ is held only to read and publish state: the work is snapshotted under it, then staging
    // is taken (and, with the pool full, its fence waited for), filled, recorded and executed without
    // it, and the results are published under it again. A release() or another queue's submission
    // therefore never waits behind a fence wait or ExecuteCommandLists here.
    void before_submit(ID3D12CommandQueue* queue, Submission& submission)
    {
        struct Work {
            ID3D12Resource* dest;
            std::shared_ptr<Copied> copy;
            const unsigned char* source;
            UINT64 offset, bytes;
            Staging* staging = nullptr;
            HRESULT hr = S_OK;
        };
        std::vector<Work> work;
        QueueState* state = nullptr;
        HRESULT state_hr = S_OK;
        {
            std::lock_guard lock(mutex_);
            ++totals_.submissions;
            bool any = false;
            for (auto& [resource, record] : records_) any |= record.copy && record.copy->live;
            if (!any) return;
            state_hr = queue_state(queue, &state);
            for (auto& [resource, record] : records_) {
                const std::shared_ptr<Copied>& copy = record.copy;
                if (!copy || !copy->live) continue;
                const std::size_t bytes = static_cast<std::size_t>(copy->bytes);
                const unsigned char* source = copy->bound.empty() ? copy->title : copy->bound.data();
                if (copy->bound.empty() && x2w_local::probe_readable(reinterpret_cast<std::uintptr_t>(source), bytes) < bytes) {
                    if (x2w_local::first_calls(unreadable_logged_, 4))
                        x2w_local::logf("x2w-d3d12 linear buffer %p: title memory %p is no longer readable; its backing "
                                        "keeps the last upload", static_cast<void*>(resource), static_cast<const void*>(source));
                    ++totals_.unreadable;
                    continue;
                }
                std::size_t first = 0, last = bytes;
                if (copy->uploaded.size() == bytes) {
                    if (std::memcmp(copy->uploaded.data(), source, bytes) == 0) {
                        copy->stale = false;
                        continue;
                    }
                    while (first < bytes && copy->uploaded[first] == source[first]) ++first;
                    while (last > first && copy->uploaded[last - 1] == source[last - 1]) --last;
                    first &= ~std::size_t{3};
                    last = (std::min)(bytes, (last + 3) & ~std::size_t{3});
                }
                try {
                    work.push_back({resource, copy, source, first, last - first});
                } catch (const std::bad_alloc&) {
                    refresh_failed(resource, *copy, E_OUTOFMEMORY);
                }
            }
        }
        if (work.empty()) return;

        // Without the lock: staging (each one ours alone once taken), the copy into it, the list, the queue.
        std::vector<Staging*> taken;
        bool any_staged = false;
        for (Work& w : work) {
            w.hr = state_hr;
            if (SUCCEEDED(w.hr)) w.hr = acquire_staging(queue, w.bytes, taken, &w.staging);
            if (FAILED(w.hr)) continue;
            taken.push_back(w.staging);
            std::memcpy(w.staging->mapped, w.source + w.offset, static_cast<std::size_t>(w.bytes));
            any_staged = true;
        }
        RefreshList* list = nullptr;
        HRESULT list_hr = S_OK;
        if (any_staged) {
            list_hr = acquire_list(queue, *state, &list);
            if (SUCCEEDED(list_hr)) {
                // Buffers rest in COMMON: each copy promotes its destination to COPY_DEST (on a direct,
                // compute or copy queue alike), and it decays to COMMON again when this
                // ExecuteCommandLists completes, before the title's lists use it.
                for (const Work& w : work)
                    if (SUCCEEDED(w.hr)) list->list->CopyBufferRegion(w.dest, w.offset, w.staging->buffer, 0, w.bytes);
                list_hr = list->list->Close();
            }
            if (SUCCEEDED(list_hr)) {
                ID3D12CommandList* lists[] = {list->list};
                queue->ExecuteCommandLists(1, lists);
            }
        }

        // Publish under the lock. Two submissions that snapshot the same buffer concurrently can both
        // copy the same changed span and publish `uploaded` in either order, so `uploaded` may briefly
        // lag what the GPU already holds. The next submission then compares the title against an older
        // upload and copies a span that is redundant or larger than needed, never one that misses a
        // change, so the cost is an extra refresh, not stale data.
        std::lock_guard lock(mutex_);
        if (list && FAILED(list_hr)) list->value = 0;
        for (Work& w : work) {
            const HRESULT hr = FAILED(w.hr) ? w.hr : list_hr;
            if (FAILED(hr)) {
                if (w.staging) w.staging->value = 0;  // never reached the GPU
                refresh_failed(w.dest, *w.copy, hr);
                continue;
            }
            submission.staging.push_back(w.staging);
            Copied& copy = *w.copy;
            if (copy.live) {
                // The backing now holds these bytes (the staging buffer keeps them until its fence).
                try {
                    if (copy.uploaded.size() != copy.bytes) copy.uploaded.assign(static_cast<std::size_t>(copy.bytes), 0);
                    std::memcpy(copy.uploaded.data() + w.offset, w.staging->mapped, static_cast<std::size_t>(w.bytes));
                } catch (const std::bad_alloc&) {
                    copy.uploaded.clear();  // the next submission copies the whole buffer again
                }
            }
            copy.stale = false;
            ++copy.refreshes;
            copy.bytes_copied += w.bytes;
            copy.last_staging = w.staging->id;
            ++totals_.refreshes;
            totals_.bytes_copied += w.bytes;
        }
        if (SUCCEEDED(list_hr) && list) submission.list = list;
    }

    // After the title's lists were executed on queue: the fence value that completes them also releases
    // the staging buffers and refresh list used for this submission.
    void after_submit(ID3D12CommandQueue* queue, Submission& submission)
    {
        std::lock_guard lock(mutex_);
        const auto found = queues_.find(queue);
        if (found == queues_.end()) return;
        QueueState& state = found->second;
        UINT64 value = 0;
        if (SUCCEEDED(queue->Signal(state.fence, state.value + 1))) value = ++state.value;
        // Without a signalled value nothing used here can be proven complete: it stays in use.
        for (Staging* staging : submission.staging) {
            staging->fence = state.fence;
            staging->value = value ? value : ~UINT64{0};
        }
        if (submission.list) submission.list->value = value ? value : ~UINT64{0};
        submission = {};
        if (!value) {
            if (x2w_local::first_calls(signal_logged_, 2))
                x2w_local::logf("x2w-d3d12 linear buffers: the submit fence could not be signalled; its staging stays in use");
            return;
        }
        for (auto& [resource, record] : records_)
            if (record.copy && record.copy->live) record.copy->last_fence = value;
    }

    bool info(ID3D12Resource* resource, X2wLinearBufferInfo* out)
    {
        std::lock_guard lock(mutex_);
        const auto found = records_.find(resource);
        if (found == records_.end()) return false;
        *out = found->second.info;
        if (const Copied* copy = found->second.copy.get()) {
            out->refreshes = copy->refreshes;
            out->bytes_copied = copy->bytes_copied;
            out->last_staging = copy->last_staging;
            out->last_fence = copy->last_fence;
            out->stale = copy->stale ? 1 : 0;
            out->refresh_failures = copy->refresh_failures;
            out->last_refresh_hr = copy->last_refresh_hr;
        }
        return true;
    }

    void totals(X2wLinearBufferTotals* out)
    {
        std::lock_guard lock(mutex_);
        *out = totals_;
    }

private:
    struct Copied {
        const unsigned char* title = nullptr;
        UINT64 bytes = 0;
        bool live = true;
        std::vector<unsigned char> uploaded;  // what the backing holds (empty before the first refresh)
        std::vector<unsigned char> bound;     // skip-refresh mutant: the bytes at placement
        UINT64 refreshes = 0, bytes_copied = 0, last_staging = 0, last_fence = 0, refresh_failures = 0;
        bool stale = false;
        HRESULT last_refresh_hr = S_OK;
    };
    struct Record {
        X2wLinearBufferInfo info{};
        std::shared_ptr<Copied> copy;  // shared: a submission's snapshot outlives a discard
    };
    struct Staging {
        ID3D12Resource* buffer = nullptr;
        unsigned char* mapped = nullptr;
        UINT64 bytes = 0, id = 0;
        ID3D12Fence* fence = nullptr;  // the queue fence whose value completes its last use
        UINT64 value = 0;              // 0: free; ~0: in use with no signalled value
    };
    struct RefreshList {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        UINT64 value = 0;
    };
    struct QueueState {
        ID3D12Fence* fence = nullptr;
        UINT64 value = 0;
        std::vector<std::unique_ptr<RefreshList>> lists;
    };
    struct Heap {
        void* base;
        ID3D12Heap* heap;
        UINT64 generation, bytes;
    };
    // 256 staging buffers; X2W_TEST_LINEAR_STAGING_LIMIT=<n> (fixtures) lowers it, to make the pool fill.
    static std::size_t staging_limit()
    {
        static const std::size_t limit = [] {
            const DWORD saved = GetLastError();
            char value[16]{};
            const DWORD n = GetEnvironmentVariableA("X2W_TEST_LINEAR_STAGING_LIMIT", value, sizeof(value));
            SetLastError(saved);
            const unsigned long parsed = n && n < sizeof(value) ? std::strtoul(value, nullptr, 10) : 0;
            return parsed ? static_cast<std::size_t>(parsed) : std::size_t{256};
        }();
        return limit;
    }

    HRESULT feature(ID3D12Device* device)
    {
        if (feature_device_ != device) {
            feature_device_ = device;
            ++totals_.feature_queries;
            D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
            const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps));
            feature_hr_ = FAILED(hr) ? hr
                        : heaps.Supported && !linear_detail::existing_heaps_forced_off() ? S_OK : DXGI_ERROR_UNSUPPORTED;
            x2w_local::logf("x2w-d3d12 linear buffers (X2W_D3D12_LINEAR_BUFFERS=%s): D3D12_FEATURE_EXISTING_HEAPS -> 0x%08lx "
                            "supported=%d%s", linear_buffer_mode() == LinearBufferMode::copy ? "copy" : "1",
                            static_cast<unsigned long>(hr), heaps.Supported ? 1 : 0,
                            linear_detail::existing_heaps_forced_off() ? " (reported as unsupported: X2W_TEST_EXISTING_HEAPS_UNSUPPORTED)" : "");
        }
        return feature_hr_;
    }

    // A private range only (place() decides the rest). Its heap is opened once per allocation base and
    // cached: the open heap keeps the range from being released, so the base cannot name other memory.
    HRESULT share(ID3D12Device* device, const PlacedBufferRange& range, UINT64 offset, UINT64 width, bool uav,
                  ID3D12Resource** out, UINT64* generation)
    {
        const Heap* entry = nullptr;
        for (const Heap& known : heaps_)
            if (known.base == range.base) entry = &known;
        if (entry) {
            ++totals_.heap_cache_hits;
        } else {
            ID3D12Device3* device3 = nullptr;
            ID3D12Heap* heap = nullptr;
            HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device3));
            if (SUCCEEDED(hr)) {
                ++totals_.heap_opens;
                hr = device3->OpenExistingHeapFromAddress(range.base, IID_PPV_ARGS(&heap));
                device3->Release();
                if (FAILED(hr)) ++totals_.heap_open_failures;
            }
            if (FAILED(hr)) return hr;
            try {
                heaps_.push_back({range.base, heap, ++generation_, heap->GetDesc().SizeInBytes});
            } catch (const std::bad_alloc&) {
                heap->Release();
                return E_OUTOFMEMORY;
            }
            entry = &heaps_.back();
        }
        *generation = entry->generation;
        if (entry->bytes < offset || width > entry->bytes - offset) return E_INVALIDARG;
        const auto desc = linear_detail::buffer(width, static_cast<D3D12_RESOURCE_FLAGS>(
            (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER));
        const HRESULT hr = device->CreatePlacedResource(entry->heap, offset, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(out));
        if (FAILED(hr)) ++totals_.place_failures;
        return hr;
    }

    // Whether a fence can be created for this device at all (the copy path's lifetime guarantee needs one).
    HRESULT fence_check(ID3D12Device* device)
    {
        if (fence_check_device_ != device) {
            fence_check_device_ = device;
            ID3D12Fence* fence = nullptr;
            fence_check_hr_ = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            if (fence) fence->Release();
        }
        return fence_check_hr_;
    }

    HRESULT queue_state(ID3D12CommandQueue* queue, QueueState** out)
    {
        auto found = queues_.find(queue);
        if (found == queues_.end()) {
            if (linear_detail::submit_fence_forced_off()) return E_NOT_VALID_STATE;
            ID3D12Device* device = nullptr;
            HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
            ID3D12Fence* fence = nullptr;
            if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            if (device) device->Release();
            if (FAILED(hr)) return hr;
            try {
                found = queues_.emplace(queue, QueueState{}).first;
            } catch (const std::bad_alloc&) {
                fence->Release();
                return E_OUTOFMEMORY;
            }
            found->second.fence = fence;
        }
        *out = &found->second;
        return S_OK;
    }

    static bool complete(const Staging& staging)
    {
        return staging.value == 0 ||
               (staging.value != ~UINT64{0} && staging.fence && staging.fence->GetCompletedValue() >= staging.value);
    }

    // Takes a staging buffer of at least `bytes` that no submission still reads, for this submission
    // alone (`taken` are the ones it already holds). Called without mutex_: the lock is taken only to
    // choose or publish a buffer; creating one and waiting for a fence happen outside it.
    HRESULT acquire_staging(ID3D12CommandQueue* queue, UINT64 bytes, const std::vector<Staging*>& taken, Staging** out)
    {
        const bool early = linear_detail::mutant("reuse-early");
        {
            std::lock_guard lock(mutex_);
            if (const unsigned period = linear_detail::refresh_fault_period();
                period && ++staging_requests_ % period == 2 % period)
                return E_OUTOFMEMORY;  // fixture fault (X2W_TEST_LINEAR_REFRESH_FAIL)
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool create = false;
            ID3D12Fence* fence = nullptr;
            UINT64 value = 0;
            {
                std::lock_guard lock(mutex_);
                Staging* best = nullptr;
                for (const auto& staging : staging_) {
                    if (staging->bytes < bytes || (!early && !complete(*staging))) continue;
                    if (std::find(taken.begin(), taken.end(), staging.get()) != taken.end()) continue;
                    // The reuse-early mutant takes the most recently used buffer, in flight or not.
                    if (!best || (early ? staging->id > best->id : staging->bytes < best->bytes)) best = staging.get();
                }
                if (best) {
                    ++totals_.staging_reused;
                    best->value = ~UINT64{0};
                    *out = best;
                    return S_OK;
                }
                if (staging_.size() + creating_ < staging_limit()) {
                    ++creating_;
                    create = true;
                } else {
                    // The pool is full of buffers in flight: the oldest with a signalled value is waited for.
                    const Staging* oldest = nullptr;
                    for (const auto& staging : staging_)
                        if (staging->value != ~UINT64{0} && staging->fence && (!oldest || staging->value < oldest->value))
                            oldest = staging.get();
                    if (oldest) {
                        fence = oldest->fence;
                        value = oldest->value;
                    }
                }
            }
            if (create) return create_staging(queue, bytes, out);
            if (!fence || attempt) break;
            // The hold-lock mutant waits with mutex_ held, as before the review: other callers block.
            std::unique_lock<std::mutex> held;
            if (linear_detail::mutant("hold-lock")) held = std::unique_lock<std::mutex>(mutex_);
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const bool reached = event && SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                                 WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
            if (event) CloseHandle(event);
            if (!reached) return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    // Called without mutex_ after acquire_staging reserved a place in the pool (creating_).
    HRESULT create_staging(ID3D12CommandQueue* queue, UINT64 bytes, Staging** out)
    {
        auto staging = std::make_unique<Staging>();
        ID3D12Device* device = nullptr;
        HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) {
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
            const UINT64 rounded = (bytes + 0xFFFF) & ~UINT64{0xFFFF};
            const UINT64 size = rounded ? rounded : 0x10000;
            const auto desc = linear_detail::buffer(size, D3D12_RESOURCE_FLAG_NONE);
            hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&staging->buffer));
            device->Release();
            staging->bytes = size;
        }
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        if (SUCCEEDED(hr)) hr = staging->buffer->Map(0, &none, &mapped);
        std::lock_guard lock(mutex_);
        --creating_;
        if (FAILED(hr)) {
            if (staging->buffer) staging->buffer->Release();
            return hr;
        }
        staging->mapped = static_cast<unsigned char*>(mapped);
        staging->id = ++staging_ids_;
        staging->value = ~UINT64{0};
        try {
            staging_.push_back(std::move(staging));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;  // the unique_ptr releases nothing: the buffer is leaked, not reused
        }
        ++totals_.staging_created;
        *out = staging_.back().get();
        return S_OK;
    }

    // A refresh list for queue whose last use has completed, or a new one, reset and ours alone.
    // Called without mutex_; the lock covers only the choice and the pool.
    HRESULT acquire_list(ID3D12CommandQueue* queue, QueueState& state, RefreshList** out)
    {
        RefreshList* found = nullptr;
        {
            std::lock_guard lock(mutex_);
            const UINT64 completed = state.fence->GetCompletedValue();
            for (const auto& list : state.lists) {
                if (list->value == ~UINT64{0} || list->value > completed) continue;
                list->value = ~UINT64{0};
                found = list.get();
                break;
            }
        }
        if (found) {
            HRESULT hr = found->allocator->Reset();
            if (SUCCEEDED(hr)) hr = found->list->Reset(found->allocator, nullptr);
            *out = found;  // on failure the caller frees it again under the lock
            return hr;
        }
        ID3D12Device* device = nullptr;
        HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr)) return hr;
        const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
        auto list = std::make_unique<RefreshList>();
        hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&list->allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, type, list->allocator, nullptr, IID_PPV_ARGS(&list->list));
        device->Release();
        if (FAILED(hr)) {
            if (list->list) list->list->Release();
            if (list->allocator) list->allocator->Release();
            return hr;
        }
        list->value = ~UINT64{0};
        std::lock_guard lock(mutex_);
        try {
            state.lists.push_back(std::move(list));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        *out = state.lists.back().get();
        return S_OK;
    }

    // The submission runs without this buffer's refresh: its backing keeps the last upload.
    void refresh_failed(ID3D12Resource* resource, Copied& copy, HRESULT hr)
    {
        copy.stale = true;
        ++copy.refresh_failures;
        copy.last_refresh_hr = hr;
        ++totals_.refresh_failures;
        totals_.last_refresh_hr = hr;
        if (x2w_local::first_calls(refresh_logged_, 8))
            x2w_local::logf("x2w-d3d12 linear buffers: refresh of %p failed 0x%08lx; the submission runs with its last "
                            "upload (stale)", static_cast<void*>(resource), static_cast<unsigned long>(hr));
    }

    std::mutex mutex_;
    std::unordered_map<ID3D12Resource*, Record> records_;
    std::unordered_map<ID3D12CommandQueue*, QueueState> queues_;
    std::vector<std::unique_ptr<Staging>> staging_;
    std::vector<Heap> heaps_;
    ID3D12Device* feature_device_ = nullptr;
    HRESULT feature_hr_ = S_FALSE;
    ID3D12Device* fence_check_device_ = nullptr;
    HRESULT fence_check_hr_ = S_OK;
    UINT64 generation_ = 0, staging_ids_ = 0, staging_requests_ = 0;
    std::size_t creating_ = 0;  // staging buffers being created outside the lock
    X2wLinearBufferTotals totals_{};
    volatile LONG unreadable_logged_ = 0, signal_logged_ = 0, refresh_logged_ = 0;
};

}  // namespace x2w_startup
