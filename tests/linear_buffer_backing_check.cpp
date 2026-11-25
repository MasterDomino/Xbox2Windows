// Production slice of #1914 (Refs #874): one ordinary linear buffer, written by the CPU and read by
// the GPU, placed at a title address through the device wrapper's slot 133 (CreatePlacedResourceX)
// and submitted through the command-queue wrapper's ExecuteCommandLists, in d3d12_xs.dll. The cases
// and sentinels are those of tests/unified_buffer_paths_check.cpp (#1942); here they run against the
// production backing instead of a model of it.
//
//   linear_buffer_backing_check <warp|hardware> <path> [--mutate <name>]
//
// path selects the backing (the process environment is set here, before d3d12_xs.dll reads it):
//   default    X2W_D3D12_LINEAR_BUFFERS unset: slot 133 as before (the old behaviour, asserted)
//   share      X2W_D3D12_LINEAR_BUFFERS=1: real sharing where the adapter, extent and memory allow it
//              (a private range); an XMem view is refused for sharing by the host rule and copied
//   copy       X2W_D3D12_LINEAR_BUFFERS=copy: forced copying at submit
//   refusal    X2W_D3D12_LINEAR_BUFFERS=1 with X2W_TEST_EXISTING_HEAPS_UNSUPPORTED=1: the host refuses
//              sharing (as an adapter reporting D3D12_FEATURE_EXISTING_HEAPS = 0 does)
//   untracked  X2W_D3D12_LINEAR_BUFFERS=copy with X2W_TEST_SUBMIT_FENCE_FAIL=1: no submit fence, so
//              the copy cannot keep its lifetime guarantee and must refuse
//
// Title memory comes from two pools. "xmem": xmem.dll physical pages (64 KiB slots of 256 MiB
// pagefile sections) mapped with X2W_XMEM_SHARED_VIEWS=1, five pages per mapping call (one view):
// three neighbour pages, then the two-page buffer at view offset 0x30000. A previous user's pages are
// freed first, so the first buffer lands on reused slots that must read zero. "private": one
// committed 4 MiB VirtualAlloc range, five pages per allocation in the same layout.
//
// Cases (sentinels A, B, C, D, E, G, P, filler and neighbour differ in every word):
//   a    bind A: slot 133 and a list that reads the buffer are recorded over A. The CPU writes B
//        before ExecuteCommandLists. The GPU must read B.
//   a2   then 16 words change to E: the next submission reads them, and a copy moves only those 64 bytes.
//   b    a GPU-written use (ALLOW_UNORDERED_ACCESS): the GPU writes G ^ key; after the fence the CPU
//        alias holds it and the words around keep theirs. The copy cannot give this: it must refuse,
//        with the sharing HRESULT.
//   c    a submission is held in flight on a gate fence, then two more (other buffers, C and D). Each
//        held submission reads its own sentinel. Sharing: the title releases X while it is held, and
//        after the fence places a new buffer (E) at X's address, on the same cached heap generation.
//        Copying: no staging buffer is handed out again while its submission is held; after the fence
//        the next refresh takes one of them again.
//   d1   missing refresh: after a refreshed submission of A the CPU writes B, and the list is
//        executed on the desktop queue directly, past the wrapper. Copying must expose the stale A
//        (the oracle sees a missing refresh); sharing reads B. Through the wrapper the copy reads B.
//   d2   readback before the fence: the GPU write of b is held on the gate. Before the gate opens the
//        fence has not completed and the alias still holds P in every word (the read is ordered
//        before the write by the queue's Wait); after it, G ^ key. Copying refuses the use (as b).
//   split (share only) a buffer across two XMem views: sharing is refused with E_INVALIDARG, decided
//        before any D3D12 call, and the buffer is copied; case a must still read B.
//   gate (refusal only) no heap was opened: D3D12_FEATURE_EXISTING_HEAPS gates sharing before any open.
//   pin  (share, XMem; measured, not asserted) a heap opened from an XMem view and a buffer placed on
//        it, then XMem's unmap of those pages, while they live and after their release.
//
// The default path asserts what slot 133 did before: a private range is aliased (a and b pass) and
// an XMem view gets a committed buffer that does not alias it (a reads zero, b leaves P), with no
// linear-buffer bookkeeping at all.
//
// --mutate <name> sets X2W_TEST_LINEAR_BUFFER_MUTANT, which removes one guarantee inside d3d12_xs.dll:
//   skip-refresh (copy) case a, reuse-early (copy) case c, copy-gpu-writes (copy) case b,
//   ignore-feature (refusal) case gate.
// The run then exits 1 only if every targeted case failed, else 0 (negative controls, WILL_FAIL).
// X2W_TEST_DEBUG_LAYER=1 enables the D3D12 debug layer; any stored error fails the run.
// Returns 0 on pass, 1 on failure, 77 without the adapter, or on the share path when the adapter
// reports no existing-heap support.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xmem_test_device.h"
#include "../src/startup_stubs/linear_buffer_report.h"

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);
extern "C" __declspec(dllimport) bool x2w_d3d12_linear_buffer_info(void*, X2wLinearBufferInfo*);
extern "C" __declspec(dllimport) void x2w_d3d12_linear_buffer_totals(X2wLinearBufferTotals*);
extern "C" __declspec(dllimport) void* WINAPI XMemVirtualAlloc(void*, SIZE_T, DWORD, ULONGLONG, DWORD);
extern "C" __declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(ULONG_PTR, PULONG_PTR, PULONG_PTR);
extern "C" __declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR, PULONG_PTR);
extern "C" __declspec(dllimport) void* WINAPI XMemMapPhysicalPages(void*, ULONG_PTR, const ULONG_PTR*);

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT64 page = 64 * 1024;
constexpr unsigned alloc_pages = 5;       // three neighbour pages, then the buffer
constexpr UINT64 buffer_offset = 3 * page;
constexpr UINT64 buffer_bytes = 2 * page;
constexpr UINT words = 1024;
constexpr UINT64 region_bytes = words * 4;
constexpr UINT64 r0 = page - region_bytes / 2;  // the region the GPU reads; straddles the buffer's two pages
constexpr UINT64 r1 = page + 0x8000;            // the region the GPU writes
constexpr std::uint32_t key = 0x5a5a5a5au;

enum Tag { neighbour, filler, A, B, C, D, E, G, P, tag_count };
const char* const tag_names[tag_count] = {"neighbour", "filler", "A", "B", "C", "D", "E", "G", "P"};
constexpr std::uint32_t seeds[tag_count] = {0x6e656967u, 0x66696c6cu, 0xa1a1a1a1u, 0xb2b2b2b2u, 0xc3c3c3c3u,
                                            0xd4d4d4d4u, 0xe5e5e5e5u, 0x47474747u, 0x50505050u};
std::uint32_t pattern(Tag tag, UINT64 word) { return seeds[tag] ^ static_cast<std::uint32_t>(word * 0x9e3779b1u); }

std::string hex(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

// `count` words of `tag` at byte `offset` of `cpu`; the pattern index is the word's index from `cpu`.
void write(std::uint8_t* cpu, UINT64 offset, Tag tag, UINT64 count)
{
    auto* w = reinterpret_cast<std::uint32_t*>(cpu);
    for (UINT64 i = 0; i < count; ++i) w[offset / 4 + i] = pattern(tag, offset / 4 + i);
}

struct Tally {
    unsigned count[tag_count]{};
    unsigned zero = 0, other = 0;
    std::string text() const
    {
        std::string out;
        for (int t = 0; t < tag_count; ++t)
            if (count[t]) out += (out.empty() ? "" : " ") + std::string(tag_names[t]) + "=" + std::to_string(count[t]);
        if (zero) out += (out.empty() ? "" : " ") + std::string("zero=") + std::to_string(zero);
        if (other) out += (out.empty() ? "" : " ") + std::string("other=") + std::to_string(other);
        return out + "/" + std::to_string(words);
    }
};
// Attributes each word to the sentinel it holds after removing `mask`; word i has pattern index first + i.
Tally tally(const std::uint32_t* got, UINT64 first, std::uint32_t mask)
{
    Tally t;
    for (UINT i = 0; i < words; ++i) {
        const std::uint32_t value = got[i] ^ mask;
        bool matched = false;
        for (int tag = 0; tag < tag_count && !matched; ++tag)
            if (value == pattern(static_cast<Tag>(tag), first + i)) {
                ++t.count[tag];
                matched = true;
            }
        if (!matched) ++(value == 0 ? t.zero : t.other);
    }
    return t;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

using Word = std::uint64_t;
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0, Word e = 0)
{
    using Slot = Word(WINAPI*)(void*, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e);
}
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }

// The desktop object behind a wrapper: SetPrivateDataInterface unwraps its argument before the call
// reaches the shared desktop device (as in tests/startup_indirect_dispatch.cpp).
template <typename T>
ComPtr<T> underlying(ID3D12Device* desktop, ID3D12Device* guest, IUnknown* object)
{
    constexpr GUID slot{0x6c1f0a3e, 0x2b51, 0x4d0e, {0x9b, 0x31, 0x5e, 0x0c, 0x72, 0x14, 0x88, 0x19}};
    ComPtr<T> result;
    if (FAILED(guest->SetPrivateDataInterface(slot, object))) return result;
    ComPtr<IUnknown> stored;
    UINT bytes = sizeof(IUnknown*);
    const HRESULT read = desktop->GetPrivateData(slot, &bytes, stored.GetAddressOf());
    guest->SetPrivateDataInterface(slot, nullptr);
    if (SUCCEEDED(read)) stored.As(&result);
    return result;
}

constexpr char shader_source[] = R"(
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
cbuffer Key : register(b0) { uint key; };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { dst.Store(id.x * 4, src.Load(id.x * 4) ^ key); }
)";

enum class Path { default_backing, share, copy, refusal, untracked };
const char* path_name(Path p)
{
    switch (p) {
    case Path::default_backing: return "default";
    case Path::share: return "share";
    case Path::copy: return "copy";
    case Path::refusal: return "refusal";
    default: return "untracked";
    }
}

// ---- Title memory ----

struct Memory {
    std::uint8_t* cpu = nullptr;  // allocation start; the buffer is at cpu + buffer_offset
    std::vector<ULONG_PTR> tokens;
    std::uint8_t* buffer() const { return cpu + buffer_offset; }
};

struct Pool {
    bool xmem;
    std::uint8_t* base = nullptr;
    UINT64 cursor = 0, bytes = 0;
    explicit Pool(bool x) : xmem(x) {}
    const char* name() const { return xmem ? "xmem" : "private"; }

    bool init()
    {
        bytes = xmem ? UINT64{16} << 30 : UINT64{4} << 20;  // XMem takes the 16 GiB startup reservation shape
        base = static_cast<std::uint8_t*>(xmem ? XMemVirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), 0x20402000, 0x21, PAGE_READWRITE)
                                               : VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!base) return false;
        if (!xmem) return true;
        // A previous user: its slots are freed, so the next allocation reuses them and must read zero.
        Memory previous;
        if (!map(previous, alloc_pages)) return false;
        write(previous.cpu, 0, D, alloc_pages * page / 4);
        return release(previous);
    }
    bool map(Memory& out, unsigned pages)
    {
        out.cpu = base + cursor;
        if (cursor + pages * page > bytes) return false;
        cursor += pages * page;
        if (!xmem) return true;
        out.tokens.assign(pages, 0);
        ULONG_PTR got = pages;
        return XMemAllocatePhysicalPages(0x20400000, &got, out.tokens.data()) && got == pages &&
               XMemMapPhysicalPages(out.cpu, pages, out.tokens.data()) == out.cpu;
    }
    // One allocation in the case layout: neighbour pages, then the buffer filled with `fill`.
    bool allocate(Memory& out, Tag fill, bool* zero = nullptr)
    {
        if (!map(out, alloc_pages)) return false;
        bool clean = true;
        for (UINT64 i = 0; i < alloc_pages * page; i += 4) clean &= *reinterpret_cast<std::uint32_t*>(out.cpu + i) == 0;
        if (zero) *zero = clean;
        write(out.cpu, 0, neighbour, buffer_offset / 4);
        write(out.buffer(), 0, fill, buffer_bytes / 4);
        return true;
    }
    // Two pages of the buffer in two mapping calls (two views): the buffer crosses a view boundary.
    bool allocate_split(Memory& out, Tag fill)
    {
        Memory first, second;
        if (!xmem || !map(first, alloc_pages - 1) || !map(second, 1)) return false;
        out.cpu = first.cpu;
        out.tokens = first.tokens;
        out.tokens.insert(out.tokens.end(), second.tokens.begin(), second.tokens.end());
        write(out.cpu, 0, neighbour, buffer_offset / 4);
        write(out.buffer(), 0, fill, buffer_bytes / 4);
        return true;
    }
    bool release(Memory& memory)
    {
        if (!xmem) return true;
        const auto count = static_cast<ULONG_PTR>(memory.tokens.size());
        return XMemMapPhysicalPages(memory.cpu, count, nullptr) == memory.cpu &&
               XMemFreePhysicalPages(count, memory.tokens.data());
    }
};

// ---- Device, the wrapper's queue, and recording ----

struct Gpu {
    ComPtr<ID3D12Device> desktop, guest;
    ComPtr<ID3D12CommandQueue> guest_queue, queue;  // the wrapper and the desktop queue behind it
    ComPtr<ID3D12Fence> done, gate;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    // GPU-read results (one slot per list that may be in flight at once), their CPU copy, the G words.
    static constexpr UINT slots = 4;
    ComPtr<ID3D12Resource> output, readback, source;
    ComPtr<ID3D12InfoQueue> info;
    UINT64 submitted = 0, gate_value = 0;
    bool gate_armed = false;

    bool init()
    {
        const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (FAILED(guest->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&guest_queue)))) return false;
        queue = underlying<ID3D12CommandQueue>(desktop.Get(), guest.Get(), guest_queue.Get());
        if (!queue || FAILED(desktop->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done))) ||
            FAILED(desktop->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))))
            return false;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 1};
        const D3D12_ROOT_SIGNATURE_DESC root_desc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, code, errors;
        if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
            FAILED(desktop->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))) ||
            FAILED(D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                              &code, &errors)))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.Get();
        pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(desktop->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)))) return false;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        auto desc = buffer_desc(slots * region_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&output))))
            return false;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback))))
            return false;
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        desc.Width = region_bytes;
        if (FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&source))))
            return false;
        void* mapped = nullptr;
        if (FAILED(source->Map(0, nullptr, &mapped))) return false;
        // The G words are indexed as the buffer's words at r1, so the alias can be tallied in place.
        for (UINT i = 0; i < words; ++i) static_cast<std::uint32_t*>(mapped)[i] = pattern(G, r1 / 4 + i);
        source->Unmap(0, nullptr);
        return true;
    }

    // Later submissions wait on the gate until open_gate().
    void hold()
    {
        if (!gate_armed) ++gate_value;
        gate_armed = true;
    }
    void open_gate()
    {
        if (gate_armed) gate->Signal(gate_value);
        gate_armed = false;
    }
    bool wait(UINT64 value)
    {
        if (done->GetCompletedValue() >= value) return true;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ok = event && SUCCEEDED(done->SetEventOnCompletion(value, event)) &&
                        WaitForSingleObject(event, 20000) == WAIT_OBJECT_0;
        if (event) CloseHandle(event);
        return ok;
    }
    std::vector<std::uint32_t> read_output(UINT slot = 0)
    {
        std::vector<std::uint32_t> got(words);
        void* mapped = nullptr;
        const D3D12_RANGE range{slot * region_bytes, (slot + 1) * region_bytes};
        if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
            std::memcpy(got.data(), static_cast<const std::uint8_t*>(mapped) + slot * region_bytes, region_bytes);
            const D3D12_RANGE none{0, 0};
            readback->Unmap(0, &none);
        }
        return got;
    }
    unsigned debug_errors()
    {
        if (!info) return 0;
        unsigned errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, message, &bytes)) || message->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
            if (++errors <= 8) std::printf("debug layer ERROR id %d: %.300s\n", message->ID, message->pDescription);
        }
        return errors;
    }
};

// One title command list: a wrapper list on its own wrapper allocator, recorded on its desktop list.
struct TitleList {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<IUnknown> list;
    ComPtr<ID3D12GraphicsCommandList> native;
    Gpu* gpu = nullptr;

    bool begin(Gpu& g)
    {
        gpu = &g;
        if (FAILED(g.guest->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
        using CreateList = HRESULT(WINAPI*)(IUnknown*, const void*, IUnknown*, IUnknown*, REFIID, void**);
        const UINT list_desc[]{D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
        auto* create = reinterpret_cast<CreateList>((*reinterpret_cast<void***>(g.guest.Get()))[73]);
        if (FAILED(create(g.guest.Get(), list_desc, allocator.Get(), nullptr, __uuidof(ID3D12GraphicsCommandList),
                          reinterpret_cast<void**>(list.GetAddressOf()))))
            return false;
        if (FAILED(static_cast<HRESULT>(invoke(list.Get(), 139, arg(allocator.Get()))))) return false;
        native = underlying<ID3D12GraphicsCommandList>(g.desktop.Get(), g.guest.Get(), list.Get());
        return native != nullptr;
    }
    void barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        native->ResourceBarrier(1, &b);
    }
    void dispatch(D3D12_GPU_VIRTUAL_ADDRESS src, D3D12_GPU_VIRTUAL_ADDRESS dst)
    {
        native->SetComputeRootSignature(gpu->root.Get());
        native->SetPipelineState(gpu->pipeline.Get());
        native->SetComputeRootShaderResourceView(0, src);
        native->SetComputeRootUnorderedAccessView(1, dst);
        native->SetComputeRoot32BitConstant(2, key, 0);
        native->Dispatch(words / 64, 1, 1);
    }
    // output[i] = buffer[r0 + 4i] ^ key, then output -> readback, in result slot `slot`.
    void read(ID3D12Resource* buffer, UINT slot = 0)
    {
        const UINT64 at = slot * region_bytes;
        barrier(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(gpu->output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(buffer->GetGPUVirtualAddress() + r0, gpu->output->GetGPUVirtualAddress() + at);
        barrier(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(gpu->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        native->CopyBufferRegion(gpu->readback.Get(), at, gpu->output.Get(), at, region_bytes);
        barrier(gpu->output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    // buffer[r1 + 4i] = G[i] ^ key.
    void write_g(ID3D12Resource* buffer)
    {
        barrier(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(gpu->source->GetGPUVirtualAddress(), buffer->GetGPUVirtualAddress() + r1);
        barrier(buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    bool close() { return SUCCEEDED(static_cast<HRESULT>(invoke(list.Get(), 9))); }
    // Through the queue wrapper (slot 10, ExecuteCommandLists), behind the gate if it is held.
    UINT64 submit()
    {
        if (gpu->gate_armed) gpu->queue->Wait(gpu->gate.Get(), gpu->gate_value);
        void* lists[] = {list.Get()};
        invoke(gpu->guest_queue.Get(), 10, 1, arg(lists));
        gpu->queue->Signal(gpu->done.Get(), ++gpu->submitted);
        return gpu->submitted;
    }
    // Past the wrapper: the desktop list on the desktop queue (a submission without the refresh).
    UINT64 submit_bypassing()
    {
        ID3D12CommandList* lists[] = {native.Get()};
        gpu->queue->ExecuteCommandLists(1, lists);
        gpu->queue->Signal(gpu->done.Get(), ++gpu->submitted);
        return gpu->submitted;
    }
};

// ---- Placement through slot 133 ----

struct Placed {
    HRESULT hr = E_FAIL;
    ComPtr<IUnknown> wrapper;
    ComPtr<ID3D12Resource> backing;
    X2wLinearBufferInfo info{};
    bool linear = false;
};

Placed place(Gpu& gpu, std::uint8_t* address, bool gpu_written)
{
    Placed p;
    using Place = HRESULT(WINAPI*)(IUnknown*, void*, const D3D12_RESOURCE_DESC*, UINT, const void*, REFIID, void**);
    const auto desc = buffer_desc(buffer_bytes, gpu_written ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    auto* slot = reinterpret_cast<Place>((*reinterpret_cast<void***>(gpu.guest.Get()))[133]);
    p.hr = slot(gpu.guest.Get(), address, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(p.wrapper.GetAddressOf()));
    if (SUCCEEDED(p.hr) && p.wrapper) {
        p.backing = underlying<ID3D12Resource>(gpu.desktop.Get(), gpu.guest.Get(), p.wrapper.Get());
        p.linear = x2w_d3d12_linear_buffer_info(p.wrapper.Get(), &p.info);
    }
    return p;
}
void refresh_info(Placed& p)
{
    if (p.wrapper) p.linear = x2w_d3d12_linear_buffer_info(p.wrapper.Get(), &p.info);
}

std::string backing_text(const Placed& p)
{
    if (FAILED(p.hr)) return "refused " + hex(p.hr);
    if (!p.linear) return "default";
    char text[160];
    if (p.info.path == 1)
        std::snprintf(text, sizeof(text), "shared (heap generation %llu, feature %s)",
                      static_cast<unsigned long long>(p.info.heap_generation), hex(p.info.feature_hr).c_str());
    else
        std::snprintf(text, sizeof(text), "copied (share %s, feature %s, %llu bytes in %llu refreshes, staging %llu)",
                      hex(p.info.share_hr).c_str(), hex(p.info.feature_hr).c_str(),
                      static_cast<unsigned long long>(p.info.bytes_copied), static_cast<unsigned long long>(p.info.refreshes),
                      static_cast<unsigned long long>(p.info.last_staging));
    return text;
}

X2wLinearBufferTotals totals()
{
    X2wLinearBufferTotals t{};
    x2w_d3d12_linear_buffer_totals(&t);
    return t;
}

// ---- Cases ----

struct Run {
    Gpu& gpu;
    Path path;
    unsigned failures = 0, skips = 0;
    std::vector<std::pair<std::string, bool>> results;  // case name -> passed (for --mutate)

    bool copying() const { return path == Path::copy || path == Path::refusal; }
    // The copy backs this pool's buffers: forced, refused by the host, or an XMem view on the share path.
    bool copied(const Pool& pool) const { return copying() || (path == Path::share && pool.xmem); }
    // The sharing HRESULT a refused sharing attempt reports for a buffer inside one allocation.
    HRESULT share_refusal(const Pool& pool) const
    {
        return path == Path::refusal ? DXGI_ERROR_UNSUPPORTED : path == Path::share && pool.xmem ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : S_FALSE;
    }
    void row(const Pool& pool, const char* name, bool pass, const std::string& text)
    {
        std::printf("%-8s %-8s %-6s %s: %s\n", path_name(path), pool.name(), name, pass ? "PASS" : "FAIL", text.c_str());
        results.emplace_back(name, pass);
        if (!pass) ++failures;
    }
    // The path's expected backing for a GPU-read buffer in this pool.
    bool backing_ok(const Pool& pool, const Placed& p, bool split = false) const
    {
        switch (path) {
        case Path::default_backing: return SUCCEEDED(p.hr) && !p.linear;
        case Path::share:
            if (!pool.xmem) return SUCCEEDED(p.hr) && p.linear && p.info.path == 1 && p.info.share_hr == S_OK;
            return SUCCEEDED(p.hr) && p.linear && p.info.path == 2 && p.info.feature_hr == S_OK &&
                   p.info.share_hr == (split ? E_INVALIDARG : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        case Path::copy: return SUCCEEDED(p.hr) && p.linear && p.info.path == 2 && p.info.share_hr == S_FALSE;
        case Path::refusal: return SUCCEEDED(p.hr) && p.linear && p.info.path == 2 && p.info.share_hr == DXGI_ERROR_UNSUPPORTED &&
                                   p.info.feature_hr == DXGI_ERROR_UNSUPPORTED;
        default: return p.hr == E_NOT_VALID_STATE;
        }
    }
    // What the GPU must read in case a: B, except on the default path over an XMem view (not aliased, zero).
    bool read_ok(const Pool& pool, const Tally& t, Tag want) const
    {
        if (path == Path::default_backing && pool.xmem) return t.zero == words;
        return t.count[want] == words;
    }

    void case_a(Pool& pool, bool split = false)
    {
        const char* name = split ? "split" : "a";
        Memory memory;
        if (!(split ? pool.allocate_split(memory, A) : pool.allocate(memory, A))) return row(pool, name, false, "allocation failed");
        Placed p = place(gpu, memory.buffer(), false);
        if (path == Path::untracked) {
            return row(pool, name, p.hr == E_NOT_VALID_STATE && !p.wrapper,
                       "slot 133 -> " + hex(p.hr) + " (expected E_NOT_VALID_STATE 0x8007139f: no submit fence to guard staging reuse)");
        }
        TitleList list;
        if (FAILED(p.hr) || !p.backing || !list.begin(gpu)) return row(pool, name, false, "slot 133 -> " + hex(p.hr));
        list.read(p.backing.Get());
        if (!list.close()) return row(pool, name, false, "Close failed");
        write(memory.buffer(), r0, B, words);  // after bind and record, before ExecuteCommandLists
        if (!gpu.wait(list.submit())) return row(pool, name, false, "fence timeout");
        const Tally t = tally(gpu.read_output().data(), r0 / 4, key);
        refresh_info(p);
        const bool pass = backing_ok(pool, p, split) && read_ok(pool, t, B) &&
                          (!p.linear || p.info.path != 2 || p.info.bytes_copied == buffer_bytes);
        std::string text = "slot 133 -> " + hex(p.hr) + ", " + backing_text(p) + "; GPU read " + t.text();
        if (path == Path::default_backing) text += pool.xmem ? " (old behaviour: not aliased, no copy)" : " (old behaviour: aliased)";
        row(pool, name, pass, text);
        if (split || !pass || path == Path::default_backing) return;

        // a2: 16 words change; only they are copied.
        TitleList second;
        if (!second.begin(gpu)) return row(pool, "a2", false, "list");
        second.read(p.backing.Get());
        second.close();
        const UINT64 before = p.info.bytes_copied;
        write(memory.buffer(), r0 + 64 * 4, E, 16);
        if (!gpu.wait(second.submit())) return row(pool, "a2", false, "fence timeout");
        const Tally t2 = tally(gpu.read_output().data(), r0 / 4, key);
        refresh_info(p);
        const UINT64 copied = p.info.bytes_copied - before;
        const bool pass2 = t2.count[E] == 16 && t2.count[B] == words - 16 && (p.info.path != 2 || copied == 64);
        row(pool, "a2", pass2, "GPU read " + t2.text() + "; bytes copied for this submission " + std::to_string(copied) +
                                   (p.info.path == 2 ? " (expected 64)" : " (shared: none)"));
    }

    void case_b(Pool& pool, bool hold)
    {
        const char* name = hold ? "d2" : "b";
        Memory memory;
        if (!pool.allocate(memory, filler)) return row(pool, name, false, "allocation failed");
        write(memory.buffer(), r1, P, words);
        Placed p = place(gpu, memory.buffer(), true);
        const auto* alias = reinterpret_cast<const std::uint32_t*>(memory.buffer() + r1);
        const bool copy_refuses = copied(pool) || path == Path::untracked;
        if (copy_refuses && FAILED(p.hr)) {
            // The copy has no point at which a GPU write reaches the title memory: refused, no object. The
            // refusal carries the sharing HRESULT (the feature gate's DXGI_ERROR_UNSUPPORTED, or
            // ERROR_NOT_SUPPORTED for an XMem view), or DXGI_ERROR_UNSUPPORTED when sharing is not
            // attempted (copy, untracked).
            const HRESULT want = share_refusal(pool) == S_FALSE ? DXGI_ERROR_UNSUPPORTED : share_refusal(pool);
            return row(pool, name, p.hr == want && !p.wrapper,
                       "GPU-written use: slot 133 -> " + hex(p.hr) + " (expected refusal " + hex(want) + ")");
        }
        TitleList list;
        if (FAILED(p.hr) || !p.backing || !list.begin(gpu)) return row(pool, name, false, "slot 133 -> " + hex(p.hr));
        list.write_g(p.backing.Get());
        if (!list.close()) return row(pool, name, false, "Close failed");
        if (hold) gpu.hold();
        const UINT64 value = list.submit();
        std::string early;
        bool early_ok = true;
        if (hold) {
            // Ordered before the GPU write by the queue's Wait on the gate: no timing assumption.
            const bool pending = gpu.done->GetCompletedValue() < value;
            const Tally before = tally(alias, r1 / 4, 0);
            early_ok = pending && before.count[P] == words;
            early = std::string("before the fence: ") + (pending ? "in flight" : "COMPLETE") + ", alias " + before.text() + "; ";
            gpu.open_gate();
        }
        if (!gpu.wait(value)) return row(pool, name, false, "fence timeout");
        const Tally t = tally(alias, r1 / 4, key), raw = tally(alias, r1 / 4, 0);
        // The words around the written region keep their filler.
        bool around = true;
        const auto* buffer = reinterpret_cast<const std::uint32_t*>(memory.buffer());
        for (UINT64 i = 0; i < buffer_bytes / 4; ++i)
            if (i < r1 / 4 || i >= r1 / 4 + words) around &= buffer[i] == pattern(filler, i);
        refresh_info(p);
        bool pass;
        std::string text = "slot 133 -> " + hex(p.hr) + ", " + backing_text(p) + "; " + early + "after the fence alias " +
                           (t.count[G] ? t.text() + " (G ^ key)" : raw.text()) + (around ? ", neighbours kept" : ", NEIGHBOURS CHANGED");
        if (path == Path::default_backing && pool.xmem) {
            pass = SUCCEEDED(p.hr) && !p.linear && raw.count[P] == words && around;
            text += " (old behaviour: not aliased, the alias keeps P)";
        } else {
            pass = early_ok && around && t.count[G] == words &&
                   (path == Path::default_backing ? SUCCEEDED(p.hr) && !p.linear : p.linear && p.info.path == 1);
            if (path == Path::default_backing) text += " (old behaviour: aliased)";
        }
        row(pool, name, pass, text);
    }

    // c: held submissions and backing reuse only after completion.
    void case_c(Pool& pool)
    {
        if (path == Path::default_backing || path == Path::untracked) return;
        const bool copying = copied(pool);
        struct Held {
            Memory memory;
            Placed placed;
            TitleList list;
            UINT64 value = 0;
            Tag tag;
        };
        const Tag tags[3] = {B, C, D};
        Held held[3];
        const X2wLinearBufferTotals start = totals();
        gpu.hold();
        for (int i = 0; i < 3; ++i) {
            Held& h = held[i];
            h.tag = tags[i];
            if (!pool.allocate(h.memory, h.tag)) return row(pool, "c", false, "allocation failed");
            h.placed = place(gpu, h.memory.buffer(), false);
            if (FAILED(h.placed.hr) || !h.placed.backing || !h.list.begin(gpu)) return row(pool, "c", false, "slot 133 -> " + hex(h.placed.hr));
            h.list.read(h.placed.backing.Get(), static_cast<UINT>(i + 1));
            h.list.close();
            h.value = h.list.submit();
            refresh_info(h.placed);
            // The title is done with X once it is submitted: on the shared path it releases it now.
            if (i == 0 && !copying) h.placed.wrapper.Reset();
        }
        const X2wLinearBufferTotals during = totals();
        const bool pending = gpu.done->GetCompletedValue() < held[0].value;
        // Each held submission reads its own buffer into its own result slot.
        gpu.open_gate();
        if (!gpu.wait(held[2].value)) return row(pool, "c", false, "fence timeout");
        std::string reads = "held reads";
        bool reads_ok = true;
        for (int i = 0; i < 3; ++i) {
            const Tally t = tally(gpu.read_output(static_cast<UINT>(i + 1)).data(), r0 / 4, key);
            reads_ok &= t.count[held[i].tag] == words;
            reads += std::string(" ") + "XYZ"[i] + "=[" + t.text() + "]";
        }
        std::string text;
        bool pass = pending && reads_ok;
        if (copying) {
            const UINT64 s0 = held[0].placed.info.last_staging, s1 = held[1].placed.info.last_staging,
                         s2 = held[2].placed.info.last_staging;
            const bool distinct = s0 && s1 && s2 && s0 != s1 && s0 != s2 && s1 != s2;
            // After completion the next refresh takes one of those buffers again.
            Memory memory;
            Placed w;
            TitleList list;
            if (!pool.allocate(memory, E)) return row(pool, "c", false, "allocation failed");
            w = place(gpu, memory.buffer(), false);
            if (FAILED(w.hr) || !list.begin(gpu)) return row(pool, "c", false, "slot 133 -> " + hex(w.hr));
            list.read(w.backing.Get());
            list.close();
            if (!gpu.wait(list.submit())) return row(pool, "c", false, "fence timeout");
            const Tally after = tally(gpu.read_output().data(), r0 / 4, key);
            refresh_info(w);
            const X2wLinearBufferTotals end = totals();
            const bool reused_after = w.info.last_staging == s0 || w.info.last_staging == s1 || w.info.last_staging == s2;
            pass &= distinct && reused_after && after.count[E] == words && end.staging_reused > during.staging_reused;
            char line[320];
            std::snprintf(line, sizeof(line),
                          "held: staging %llu/%llu/%llu (%s; %llu created, %llu completed ones reused during the hold); after the fence the next refresh "
                          "took staging %llu (%s) and read %s",
                          static_cast<unsigned long long>(s0), static_cast<unsigned long long>(s1), static_cast<unsigned long long>(s2),
                          distinct ? "distinct" : "SHARED", static_cast<unsigned long long>(during.staging_created - start.staging_created),
                          static_cast<unsigned long long>(during.staging_reused - start.staging_reused),
                          static_cast<unsigned long long>(w.info.last_staging), reused_after ? "reused" : "NOT REUSED", after.text().c_str());
            text = line;
        } else {
            // Shared (a private range): after the fence the title places a new buffer Z at X's address.
            const std::string reuse = "after the fence Z is placed at X's address";
            Memory z = held[0].memory;
            write(z.buffer(), 0, E, buffer_bytes / 4);
            Placed zp = place(gpu, z.buffer(), false);
            TitleList list;
            if (FAILED(zp.hr) || !list.begin(gpu)) return row(pool, "c", false, "slot 133 -> " + hex(zp.hr));
            list.read(zp.backing.Get());
            list.close();
            if (!gpu.wait(list.submit())) return row(pool, "c", false, "fence timeout");
            const Tally after = tally(gpu.read_output().data(), r0 / 4, key);
            const UINT64 gx = held[0].placed.info.heap_generation, gz = zp.info.heap_generation;
            pass &= zp.linear && zp.info.path == 1 && gz == gx && after.count[E] == words;
            text = reuse + "; heap generation " + std::to_string(gx) + " -> " + std::to_string(gz) +
                   " (the range's cached heap); Z read " + after.text();
        }
        row(pool, "c", pass, std::string(pending ? "held in flight" : "NOT HELD") + "; " + reads + "; " + text);
    }

    // Measured, not asserted (share path, XMem): why slot 133 does not share a view. A heap opened from
    // the view, with a buffer placed on it, and XMem's unmap of the view's pages, before and after the
    // heap and buffer are released.
    void probe_view_pin(Pool& pool)
    {
        if (path != Path::share || !pool.xmem) return;
        Memory memory;
        ComPtr<ID3D12Device3> device3;
        if (!pool.allocate(memory, A) || FAILED(gpu.desktop.As(&device3))) return;
        ComPtr<ID3D12Heap> heap;
        ComPtr<ID3D12Resource> buffer;
        HRESULT hr = device3->OpenExistingHeapFromAddress(memory.cpu, IID_PPV_ARGS(&heap));
        const auto desc = buffer_desc(buffer_bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        if (SUCCEEDED(hr)) hr = gpu.desktop->CreatePlacedResource(heap.Get(), buffer_offset, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                                  nullptr, IID_PPV_ARGS(&buffer));
        const auto count = static_cast<ULONG_PTR>(memory.tokens.size());
        const bool held_unmap = XMemMapPhysicalPages(memory.cpu, count, nullptr) == memory.cpu;
        const DWORD held_error = held_unmap ? 0 : GetLastError();
        buffer.Reset();
        heap.Reset();
        const bool unmap = held_unmap || XMemMapPhysicalPages(memory.cpu, count, nullptr) == memory.cpu;
        const DWORD error = unmap ? 0 : GetLastError();
        const bool freed = unmap && XMemFreePhysicalPages(count, memory.tokens.data());
        std::printf("%-8s %-8s %-6s measured: heap from the view and placed buffer %s; XMem unmap while they live %s (error %lu); "
                    "after their release %s (error %lu), pages %s\n", path_name(path), pool.name(), "pin", hex(hr).c_str(),
                    held_unmap ? "succeeded" : "failed", held_error, unmap ? "succeeded" : "failed", error,
                    freed ? "freed" : "not freed");
    }

    void case_d1(Pool& pool)
    {
        if (path == Path::default_backing || path == Path::untracked) return;
        Memory memory;
        if (!pool.allocate(memory, A)) return row(pool, "d1", false, "allocation failed");
        Placed p = place(gpu, memory.buffer(), false);
        TitleList first, bypass, through;
        if (FAILED(p.hr) || !first.begin(gpu) || !bypass.begin(gpu) || !through.begin(gpu))
            return row(pool, "d1", false, "slot 133 -> " + hex(p.hr));
        first.read(p.backing.Get());
        first.close();
        if (!gpu.wait(first.submit())) return row(pool, "d1", false, "fence timeout");
        write(memory.buffer(), r0, B, words);
        bypass.read(p.backing.Get());
        bypass.close();
        if (!gpu.wait(bypass.submit_bypassing())) return row(pool, "d1", false, "fence timeout");
        const Tally missing = tally(gpu.read_output().data(), r0 / 4, key);
        through.read(p.backing.Get());
        through.close();
        if (!gpu.wait(through.submit())) return row(pool, "d1", false, "fence timeout");
        const Tally refreshed = tally(gpu.read_output().data(), r0 / 4, key);
        refresh_info(p);
        const bool shared = !copied(pool);
        const bool pass = (shared ? missing.count[B] == words : missing.count[A] == words) && refreshed.count[B] == words &&
                          p.linear && p.info.path == (shared ? 1u : 2u);
        row(pool, "d1", pass, std::string("past the wrapper (no refresh) ") + missing.text() +
                                  (shared ? " (shared: no refresh needed)" : " (stale A expected: the oracle sees the missing refresh)") +
                                  "; through the wrapper " + refreshed.text());
    }
};
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::puts("usage: linear_buffer_backing_check <warp|hardware> <default|share|copy|refusal|untracked> [--mutate <name>]");
        return 2;
    }
    const bool warp = std::strcmp(argv[1], "warp") == 0;
    if (!warp && std::strcmp(argv[1], "hardware") != 0) return 2;
    Path path;
    const std::string p = argv[2];
    if (p == "default") path = Path::default_backing;
    else if (p == "share") path = Path::share;
    else if (p == "copy") path = Path::copy;
    else if (p == "refusal") path = Path::refusal;
    else if (p == "untracked") path = Path::untracked;
    else return 2;
    std::string mutant;
    if (argc == 5 && std::strcmp(argv[3], "--mutate") == 0) mutant = argv[4];
    else if (argc != 3) return 2;

    // The switches are read once, by the first call that needs them: set them before any.
    SetEnvironmentVariableW(L"X2W_D3D12_PERMISSIVE", L"1");
    SetEnvironmentVariableW(L"X2W_D3D12_LIST_NOOP", L"1");
    SetEnvironmentVariableW(L"X2W_D3D12_RENDER", L"1");
    SetEnvironmentVariableW(L"X2W_XMEM_SHARED_VIEWS", L"1");
    SetEnvironmentVariableW(L"X2W_D3D12_LINEAR_BUFFERS", path == Path::default_backing ? nullptr
                                                         : path == Path::copy || path == Path::untracked ? L"copy" : L"1");
    SetEnvironmentVariableW(L"X2W_TEST_EXISTING_HEAPS_UNSUPPORTED", path == Path::refusal ? L"1" : nullptr);
    SetEnvironmentVariableW(L"X2W_TEST_SUBMIT_FENCE_FAIL", path == Path::untracked ? L"1" : nullptr);
    SetEnvironmentVariableA("X2W_TEST_LINEAR_BUFFER_MUTANT", mutant.empty() ? nullptr : mutant.c_str());

    char debug_env[4]{};
    const bool debug_layer = GetEnvironmentVariableA("X2W_TEST_DEBUG_LAYER", debug_env, sizeof(debug_env)) && debug_env[0] == '1';
    if (debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            std::puts("X2W_TEST_DEBUG_LAYER=1 but the debug layer is not installed");
            return 1;
        }
        debug->EnableDebugLayer();
    }

    Gpu gpu;
    static ComPtr<ID3D12Device>* desktop_out = &gpu.desktop;
    static DXGI_ADAPTER_DESC1 adapter_desc{};
    static LARGE_INTEGER driver{};
    const auto create = [](IUnknown* adapter, REFIID iid, void** out) -> HRESULT {
        ComPtr<IDXGIAdapter1> a;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&a)))) {
            a->GetDesc1(&adapter_desc);
            a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        }
        const HRESULT hr = D3D12XboxCreateDevice(adapter, nullptr, IID_PPV_ARGS(desktop_out->ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        unsigned char parameters[64]{};
        return D3D12XboxCreateDevice(adapter, parameters, iid, out);
    };
    bool unmatched = false;
    gpu.guest = create_device(warp, unmatched, create);
    if (!gpu.guest || !gpu.desktop) {
        std::puts(warp ? "WARP device not created" : "no D3D12 hardware adapter");
        return warp || unmatched ? 1 : 77;
    }
    if (debug_layer) gpu.desktop.As(&gpu.info);
    D3D12_FEATURE_DATA_EXISTING_HEAPS heaps{};
    const HRESULT heaps_hr = gpu.desktop->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps));
    std::printf("adapter %04x:%04x %ls, driver %u.%u.%u.%u, D3D12_FEATURE_EXISTING_HEAPS %s supported=%d, debug layer %s, path %s%s%s\n",
                adapter_desc.VendorId, adapter_desc.DeviceId, adapter_desc.Description, HIWORD(driver.HighPart),
                LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart), hex(heaps_hr).c_str(),
                heaps.Supported ? 1 : 0, debug_layer ? "on" : "off", path_name(path), mutant.empty() ? "" : ", mutant ",
                mutant.c_str());
    if (!gpu.init()) {
        std::puts("FAIL: fixture setup (queue, fences, pipeline)");
        return 1;
    }
    if (path == Path::share && (FAILED(heaps_hr) || !heaps.Supported)) {
        std::puts("SKIP share: the adapter reports no existing-heap support; the refusal path covers it");
        return 77;
    }

    Run run{gpu, path};
    for (const bool xmem : {true, false}) {
        Pool pool(xmem);
        if (!pool.init()) {
            run.row(pool, "pool", false, "title memory could not be set up");
            continue;
        }
        run.case_a(pool);
        if (path == Path::share && xmem) run.case_a(pool, true);
        run.case_b(pool, false);
        run.case_c(pool);
        run.case_d1(pool);
        if (path != Path::default_backing) run.case_b(pool, true);
        run.probe_view_pin(pool);
    }
    const X2wLinearBufferTotals t = totals();
    std::printf("totals: placements %llu (shared %llu, copied %llu, refused %llu), feature queries %llu, heap opens %llu "
                "(failed %llu, cache hits %llu), placements failed %llu, submissions %llu, refreshes %llu, bytes copied %llu, "
                "staging created %llu reused %llu, unreadable %llu, submissions refused %llu, last refusal %s\n",
                static_cast<unsigned long long>(t.placements), static_cast<unsigned long long>(t.shared),
                static_cast<unsigned long long>(t.copied), static_cast<unsigned long long>(t.refused),
                static_cast<unsigned long long>(t.feature_queries), static_cast<unsigned long long>(t.heap_opens),
                static_cast<unsigned long long>(t.heap_open_failures), static_cast<unsigned long long>(t.heap_cache_hits),
                static_cast<unsigned long long>(t.place_failures), static_cast<unsigned long long>(t.submissions),
                static_cast<unsigned long long>(t.refreshes), static_cast<unsigned long long>(t.bytes_copied),
                static_cast<unsigned long long>(t.staging_created), static_cast<unsigned long long>(t.staging_reused),
                static_cast<unsigned long long>(t.unreadable), static_cast<unsigned long long>(t.submissions_refused),
                hex(t.last_refusal_hr).c_str());
    {
        Pool none(false);
        if (path == Path::default_backing) {
            // Opt-in off: no linear-buffer bookkeeping at all.
            const bool untouched = !t.placements && !t.submissions && !t.feature_queries && !t.heap_opens;
            run.row(none, "off", untouched, untouched ? "no linear-buffer code ran (all totals zero)" : "LINEAR-BUFFER CODE RAN");
        } else if (path == Path::refusal) {
            run.row(none, "gate", t.feature_queries == 1 && t.heap_opens == 0,
                    "feature queries " + std::to_string(t.feature_queries) + ", heap opens " + std::to_string(t.heap_opens) +
                        " (expected 1 and 0: no open is attempted when D3D12_FEATURE_EXISTING_HEAPS reports no support)");
        }
    }
    const HRESULT removed = gpu.desktop->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        std::printf("FAIL: device removed %s\n", hex(removed).c_str());
        ++run.failures;
    }
    const unsigned errors = gpu.debug_errors();
    if (errors) {
        std::printf("FAIL: %u debug-layer errors\n", errors);
        run.failures += errors;
    }

    if (!mutant.empty()) {
        const char* target = mutant == "skip-refresh" ? "a" : mutant == "reuse-early" ? "c" : mutant == "copy-gpu-writes" ? "b"
                           : mutant == "ignore-feature" ? "gate" : nullptr;
        if (!target) return 2;
        unsigned targeted = 0, failed = 0;
        for (const auto& [name, pass] : run.results)
            if (name == target) {
                ++targeted;
                failed += !pass;
            }
        std::printf("mutant %s: %u of %u targeted '%s' rows failed\n", mutant.c_str(), failed, targeted, target);
        return targeted && failed == targeted ? 1 : 0;
    }
    std::printf("linear_buffer_backing %s %s: %s (%u failed)\n", argv[1], path_name(path), run.failures ? "FAIL" : "PASS", run.failures);
    return run.failures ? 1 : 0;
}
