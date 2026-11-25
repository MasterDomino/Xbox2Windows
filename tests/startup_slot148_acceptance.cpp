// Acceptance client for command-list slot 148, the upload allocation that the title's
// acceleration-structure builder requests (Refs #1758). It drives the actual console list and queue
// of the d3d12_xs stand-in and uses whatever slot 148 the build under test installs. It supplies no
// allocator of its own and keeps no reference to any upload page.
//
//   startup_slot148_acceptance <warp|hardware>
//       (X2W_D3D12_PERMISSIVE=1 X2W_D3D12_LIST_NOOP=1 X2W_D3D12_RENDER=1, X2W_D3D12_DEBUG=0|1)
//
// The call shape is the one traced from the builder: rcx = the list, rdx = 0x280 bytes,
// r8 = 0x10 alignment. The result in rax is a pointer the caller writes. A null result is a
// refusal; the builder then reports "Failed to allocate upload buffer" (E_FAIL).
//
// The title would use that pointer as the GPU address too. To read the bytes back on the desktop
// GPU, the client asks the module that implements slot 148 for the desktop GPU address of a live
// range. That fixture export is
//
//   extern "C" unsigned long long x2w_d3d12_upload_gpu_address(void* list, const void* cpu,
//                                                             unsigned long long bytes);
//
// It returns 0 when [cpu, cpu + bytes) is not inside one live allocation of the list.
//
// Steps, each on a new list:
//
//   traced      Reset the list on allocator a and allocate (0x280, 0x10). The CPU range must be
//               committed and writable. The GPU address must be 16-byte aligned and map the last
//               byte to GPU + 0x27f.
//   fresh       Write payload A, Close (S_OK), and expect a null allocation from the closed list.
//               Queue Wait on gate H, submit, Signal D = 1. Reset the list on allocator b, allocate,
//               write payload B, Close, submit, Signal D = 2. While both submissions are held, an
//               observer queue on the same desktop device copies both GPU ranges into a readback
//               buffer. D must still be 0 after that read. Then the CPU signals H and D must reach 2.
//               A's bytes must read A and B's must read B, byte for byte.
//   allocator   The same, but the list is Reset on allocator a again and allocator a is itself Reset
//               while A is held. A's range must not be handed to B, and A must still read A.
//   overwrite   The #2068 control: as fresh, but B is also written into A's live range before the
//               gate opens. A's readback must then read B, which shows the observer reads the live
//               bytes late rather than a copy taken at allocation.
//   reuse       A forced early-reuse control. A is written and the list Closed but never submitted,
//               then the list is Reset on the same idle allocator and B allocated. That is the only
//               client-side sequence after which a correct slot may hand A's range out again. If B
//               overlaps A's GPU range, the read of A's range must show B. If B lies elsewhere the
//               control reports that it was not exercised, because the slot did not reuse.
//   arguments   Zero size, zero alignment, alignment 24 and a size that overflows must be refused.
//               A 256-byte alignment must be honoured on both the CPU pointer and the GPU address.
//
// Finally the desktop device must not be removed and, with the debug layer, must hold no errors.
//
// Exit codes: 0 pass; 77 no physical adapter; 11 slot 148 returned memory but its module has no GPU address
// export; 12 traced request: CPU range, alignment or GPU address; 13 an allocation after a successful
// Close was not refused; 14 invalid arguments not refused or alignment not honoured; 15 payload
// mismatch; 16 overwrite control did not read B; 17 reuse control: B reuses A's range but A's read
// did not show B; 18 ordering (work ran before the gate opened, or a timeout); 19 device removed or
// debug-layer errors; 20 A's range was handed to B while A was held; 2 to 5 usage and setup.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown*, const void*, REFIID, void**);

namespace {
using Word = std::uint64_t;
template <class T> Word arg(T* value) { return reinterpret_cast<Word>(value); }
Word invoke(void* object, unsigned index, Word a = 0, Word b = 0, Word c = 0, Word d = 0, Word e = 0)
{
    using Slot = Word (WINAPI*)(void*, Word, Word, Word, Word, Word);
    return reinterpret_cast<Slot>((*static_cast<void***>(object))[index])(object, a, b, c, d, e);
}
bool ok(Word hr) { return SUCCEEDED(static_cast<HRESULT>(hr)); }

constexpr unsigned kCloseSlot = 9, kResetSlot = 139, kUploadSlot = 148;
constexpr unsigned kAllocatorReset = 8, kFenceCompleted = 8, kFenceSignal = 10;
constexpr unsigned kQueueExecute = 10, kQueueSignal = 14, kQueueWait = 17;
constexpr Word kBytes = 0x280, kAlignment = 0x10;  // the traced request: 16 x 40 bytes, 16-byte aligned
constexpr unsigned char kPayloadA = 0xA1, kPayloadB = 0xB2;  // the #2068 payloads
constexpr DWORD kWaitMs = 10000;
constexpr char kGpuExport[] = "x2w_d3d12_upload_gpu_address";
using GpuAddress = unsigned long long (*)(void* list, const void* cpu, unsigned long long bytes);

enum Exit {
    pass = 0, usage = 2, no_adapter = 3, no_device = 4, setup = 5, no_gpu_export = 11,
    traced_shape = 12, close_refusal = 13, invalid_arguments = 14, payload = 15, overwrite_control = 16,
    reuse_control = 17, ordering = 18, device_state = 19, held_reuse = 20, skip = 77
};

// [cpu, cpu + bytes) is committed and writable.
bool writable(Word cpu, Word bytes)
{
    constexpr DWORD write = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    for (Word at = cpu; at < cpu + bytes;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            !(info.Protect & write) || (info.Protect & PAGE_GUARD))
            return false;
        at = reinterpret_cast<Word>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}
// Fills the range, reporting an access violation instead of crashing (no C++ objects in a __try frame).
bool fill(Word cpu, unsigned char value, Word bytes)
{
    __try {
        std::memset(reinterpret_cast<void*>(cpu), value, static_cast<size_t>(bytes));
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
bool overlaps(Word a, Word b, Word bytes) { return a < b + bytes && b < a + bytes; }

struct Upload {
    Word cpu = 0, gpu = 0;
};

struct Client {
    void* device = nullptr;  // console device
    void* queue = nullptr;   // console queue
    ComPtr<ID3D12Device> desktop;
    GpuAddress gpu_address = nullptr;
    // Observer: a desktop queue on the same device that copies GPU ranges into a readback buffer.
    ComPtr<ID3D12CommandQueue> observer;
    ComPtr<ID3D12CommandAllocator> observer_allocator;
    ComPtr<ID3D12GraphicsCommandList> observer_list;
    ComPtr<ID3D12Fence> observer_fence;
    UINT64 observer_value = 0;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> copy;
    ComPtr<ID3D12Resource> target, readback;  // two 0x280-byte slots

    void* fence()
    {
        void* made = nullptr;
        return ok(invoke(device, 36, 0, 0, arg(&__uuidof(ID3D12Fence)), arg(&made))) ? made : nullptr;
    }
    void* allocator()
    {
        void* made = nullptr;
        return ok(invoke(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, arg(&__uuidof(ID3D12CommandAllocator)), arg(&made)))
                   ? made : nullptr;
    }
    void* list(void* on)
    {
        const unsigned desc[] = {D3D12_COMMAND_LIST_TYPE_DIRECT, 2, 1, 0};
        void* made = nullptr;
        return ok(invoke(device, 73, arg(desc), arg(on), 0, arg(&__uuidof(ID3D12GraphicsCommandList)), arg(&made)))
                   ? made : nullptr;
    }
    static bool reached(void* fence, Word value)
    {
        const auto deadline = GetTickCount64() + kWaitMs;
        while (invoke(fence, kFenceCompleted) < value) {
            if (GetTickCount64() >= deadline) return false;
            Sleep(1);
        }
        return true;
    }
    bool submit(void* list, void* done, Word value)
    {
        void* lists[] = {list};
        invoke(queue, kQueueExecute, 1, arg(lists));
        return ok(invoke(queue, kQueueSignal, arg(done), value));
    }

    // Allocates the traced request and checks the CPU range and its GPU address.
    int traced(void* list, const char* label, Upload& out)
    {
        const Word cpu = invoke(list, kUploadSlot, kBytes, kAlignment);
        if (!cpu) {
            std::printf("%s: CommandList::slot148(0x%llx, 0x%llx) returned 0x0 on an open list\n", label,
                        static_cast<unsigned long long>(kBytes), static_cast<unsigned long long>(kAlignment));
            return traced_shape;
        }
        if (!gpu_address) {
            HMODULE owner = nullptr;
            const void* entry = (*static_cast<void***>(list))[kUploadSlot];
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(entry), &owner))
                gpu_address = reinterpret_cast<GpuAddress>(GetProcAddress(owner, kGpuExport));
            if (!gpu_address) {
                std::printf("%s: slot 148 returned 0x%llx, but the module that implements it exports no %s\n", label,
                            static_cast<unsigned long long>(cpu), kGpuExport);
                return no_gpu_export;
            }
        }
        const Word gpu = gpu_address(list, reinterpret_cast<const void*>(cpu), kBytes);
        const Word last = gpu_address(list, reinterpret_cast<const void*>(cpu + kBytes - 1), 1);
        const bool range = writable(cpu, kBytes);
        std::printf("%s: cpu %% 16 = %llu, writable = %d, gpu %% 16 = %llu, last byte at gpu + 0x%llx\n", label,
                    static_cast<unsigned long long>(cpu % kAlignment), range ? 1 : 0,
                    static_cast<unsigned long long>(gpu % kAlignment),
                    static_cast<unsigned long long>(last && gpu ? last - gpu : 0));
        if (cpu % kAlignment || !range || !gpu || gpu % kAlignment || last != gpu + kBytes - 1) return traced_shape;
        out = {cpu, gpu};
        return pass;
    }
    // A successful Close, then an allocation that must be refused.
    int close(void* list, const char* label)
    {
        const auto closed = static_cast<HRESULT>(invoke(list, kCloseSlot));
        if (FAILED(closed)) {
            std::printf("%s: Close -> 0x%08lx\n", label, static_cast<unsigned long>(closed));
            return setup;
        }
        const Word after = invoke(list, kUploadSlot, kBytes, kAlignment);
        std::printf("%s: allocation after a successful Close -> 0x%llx (expected 0x0)\n", label,
                    static_cast<unsigned long long>(after));
        return after ? close_refusal : pass;
    }

    bool observer_setup()
    {
        static const char source[] =
            "ByteAddressBuffer src : register(t0);\n"
            "RWByteAddressBuffer dst : register(u0);\n"
            "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID)\n"
            "{ if (i < 160) dst.Store(i * 4, src.Load(i * 4)); }\n";
        ComPtr<ID3DBlob> code, errors, serialized;
        if (FAILED(D3DCompile(source, sizeof(source) - 1, "slot148_copy", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code,
                              &errors)))
            return false;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        const D3D12_ROOT_SIGNATURE_DESC root_desc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(desktop->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                IID_PPV_ARGS(&root))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root.Get();
        pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, 2 * kBytes, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        if (FAILED(desktop->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&copy))) ||
            FAILED(desktop->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&observer))) ||
            FAILED(desktop->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&observer_allocator))) ||
            FAILED(desktop->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, observer_allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&observer_list))) ||
            FAILED(observer_list->Close()) ||
            FAILED(desktop->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&observer_fence))) ||
            FAILED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&target))))
            return false;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(desktop->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    }
    // Copies [first, first + 0x280) and [second, second + 0x280) on the GPU and returns both, in order.
    bool read(Word first, Word second, std::vector<unsigned char>& out)
    {
        if (FAILED(observer_allocator->Reset()) || FAILED(observer_list->Reset(observer_allocator.Get(), copy.Get())))
            return false;
        observer_list->SetComputeRootSignature(root.Get());
        const Word sources[] = {first, second};
        for (int i = 0; i < 2; ++i) {
            observer_list->SetComputeRootShaderResourceView(0, sources[i]);
            observer_list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress() + i * kBytes);
            observer_list->Dispatch(3, 1, 1);
        }
        D3D12_RESOURCE_BARRIER barrier{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        barrier.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
        observer_list->ResourceBarrier(1, &barrier);
        observer_list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, 2 * kBytes);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        observer_list->ResourceBarrier(1, &barrier);
        if (FAILED(observer_list->Close())) return false;
        ID3D12CommandList* lists[] = {observer_list.Get()};
        observer->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool finished = event && SUCCEEDED(observer->Signal(observer_fence.Get(), ++observer_value)) &&
                              SUCCEEDED(observer_fence->SetEventOnCompletion(observer_value, event)) &&
                              WaitForSingleObject(event, kWaitMs) == WAIT_OBJECT_0;
        if (event) CloseHandle(event);
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(2 * kBytes)};
        if (!finished || FAILED(readback->Map(0, &range, &mapped))) return false;
        out.assign(static_cast<const unsigned char*>(mapped), static_cast<const unsigned char*>(mapped) + 2 * kBytes);
        const D3D12_RANGE none{};
        readback->Unmap(0, &none);
        return true;
    }

    UINT64 debug_errors()
    {
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(desktop.As(&info))) return 0;
        UINT64 errors = 0;
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info->GetMessage(i, message, &bytes))) continue;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++errors;
                std::printf("debug layer: %.*s\n", static_cast<int>(message->DescriptionByteLength), message->pDescription);
            }
        }
        return errors;
    }
};

// Bytes [from, from + count) of a readback slot all equal value; prints the first mismatch.
bool all(const std::vector<unsigned char>& bytes, Word slot, Word from, Word count, unsigned char value, const char* what)
{
    for (Word i = from; i < from + count; ++i) {
        if (bytes[slot * kBytes + i] != value) {
            std::printf("%s: byte %llu is 0x%02x (expected 0x%02x)\n", what, static_cast<unsigned long long>(i),
                        bytes[slot * kBytes + i], value);
            return false;
        }
    }
    return true;
}

enum class Mode { fresh, allocator, overwrite };

// Opens the gate on every return, so a failed check never leaves the console queue blocked.
struct Gate {
    void* fence;
    ~Gate() { invoke(fence, kFenceSignal, 1); }
};

// Submit A behind a gate, Reset, write and submit B, read both while held, then release the gate.
int held_sequence(Client& c, Mode mode)
{
    const char* label = mode == Mode::fresh ? "fresh" : mode == Mode::allocator ? "allocator" : "overwrite";
    void* a = c.allocator();
    void* b = c.allocator();
    void* list = a ? c.list(a) : nullptr;
    void* gate = c.fence();
    void* done = c.fence();
    if (!b || !list || !gate || !done || !ok(invoke(list, kResetSlot, arg(a)))) return setup;

    Upload first;
    if (const int result = c.traced(list, label, first)) return result;
    if (!fill(first.cpu, kPayloadA, kBytes)) return traced_shape;
    if (const int result = c.close(list, label)) return result;
    const Gate held{gate};
    if (!ok(invoke(c.queue, kQueueWait, arg(gate), 1)) || !c.submit(list, done, 1)) return setup;

    // Reset while A is held: on another allocator, or on A's own allocator after resetting it.
    if (mode == Mode::allocator && !ok(invoke(a, kAllocatorReset))) return setup;
    if (!ok(invoke(list, kResetSlot, arg(mode == Mode::allocator ? a : b)))) return setup;
    Upload second;
    if (const int result = c.traced(list, label, second)) return result;
    if (overlaps(first.cpu, second.cpu, kBytes) || overlaps(first.gpu, second.gpu, kBytes)) {
        std::printf("%s: B (cpu 0x%llx, gpu 0x%llx) reuses A's range (cpu 0x%llx, gpu 0x%llx) while A is held\n", label,
                    static_cast<unsigned long long>(second.cpu), static_cast<unsigned long long>(second.gpu),
                    static_cast<unsigned long long>(first.cpu), static_cast<unsigned long long>(first.gpu));
        return held_reuse;
    }
    if (!fill(second.cpu, kPayloadB, kBytes)) return traced_shape;
    if (mode == Mode::overwrite && !fill(first.cpu, kPayloadB, kBytes)) return traced_shape;
    if (const int result = c.close(list, label)) return result;
    if (!c.submit(list, done, 2)) return setup;

    Sleep(50);
    std::vector<unsigned char> bytes;
    const Word before = invoke(done, kFenceCompleted);
    const bool read = c.read(first.gpu, second.gpu, bytes);
    const Word after = invoke(done, kFenceCompleted);
    std::printf("%s: console submissions completed before the read %llu, after it %llu (expected 0, 0)\n", label,
                static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
    const bool opened = ok(invoke(gate, kFenceSignal, 1));
    if (!read || before || after || !opened || !Client::reached(done, 2)) return ordering;

    if (!all(bytes, 1, 0, kBytes, kPayloadB, "B payload")) return payload;
    const unsigned char wanted = mode == Mode::overwrite ? kPayloadB : kPayloadA;
    if (!all(bytes, 0, 0, kBytes, wanted, mode == Mode::overwrite ? "overwrite control" : "A payload"))
        return mode == Mode::overwrite ? overwrite_control : mode == Mode::allocator ? held_reuse : payload;
    std::printf("%s: A reads 0x%02x and B reads 0x%02x, all %llu bytes\n", label, wanted, kPayloadB,
                static_cast<unsigned long long>(kBytes));
    return pass;
}

// Close without submitting, Reset on the same idle allocator: the slot may now hand A's range out again.
int forced_reuse(Client& c)
{
    void* a = c.allocator();
    void* list = a ? c.list(a) : nullptr;
    if (!list || !ok(invoke(list, kResetSlot, arg(a)))) return setup;
    Upload first, second;
    if (const int result = c.traced(list, "reuse", first)) return result;
    if (!fill(first.cpu, kPayloadA, kBytes)) return traced_shape;
    if (const int result = c.close(list, "reuse")) return result;
    if (!ok(invoke(list, kResetSlot, arg(a)))) return setup;
    if (const int result = c.traced(list, "reuse", second)) return result;
    if (!fill(second.cpu, kPayloadB, kBytes)) return traced_shape;
    if (!overlaps(first.gpu, second.gpu, kBytes)) {
        std::printf("reuse: not exercised, B (gpu 0x%llx) does not overlap the unsubmitted A (gpu 0x%llx)\n",
                    static_cast<unsigned long long>(second.gpu), static_cast<unsigned long long>(first.gpu));
        return c.close(list, "reuse");
    }
    // A's range is live again as part of B, so reading it is defined.
    std::vector<unsigned char> bytes;
    if (!c.read(first.gpu, second.gpu, bytes)) return ordering;
    const Word from = second.gpu > first.gpu ? second.gpu - first.gpu : 0;
    const Word to = (std::min)(kBytes, second.gpu + kBytes - first.gpu);
    if (!all(bytes, 0, from, to - from, kPayloadB, "reuse control")) return reuse_control;
    std::printf("reuse: B overlaps A at bytes [%llu, %llu) and A's read shows B there\n",
                static_cast<unsigned long long>(from), static_cast<unsigned long long>(to));
    return c.close(list, "reuse");
}

int arguments(Client& c)
{
    void* a = c.allocator();
    void* list = a ? c.list(a) : nullptr;
    if (!list || !ok(invoke(list, kResetSlot, arg(a)))) return setup;
    const struct {
        const char* name;
        Word bytes, alignment;
    } refused[] = {{"zero size", 0, kAlignment}, {"zero alignment", kBytes, 0}, {"alignment 24", kBytes, 24},
                   {"size overflow", ~Word{0} - 15, kAlignment}};
    int result = pass;
    for (const auto& request : refused) {
        const Word cpu = invoke(list, kUploadSlot, request.bytes, request.alignment);
        std::printf("arguments: %s -> 0x%llx (expected 0x0)\n", request.name, static_cast<unsigned long long>(cpu));
        if (cpu) result = invalid_arguments;
    }
    const Word cpu = invoke(list, kUploadSlot, 64, 256);
    const Word gpu = cpu && c.gpu_address ? c.gpu_address(list, reinterpret_cast<const void*>(cpu), 64) : 0;
    std::printf("arguments: (64, 256) -> cpu %% 256 = %llu, gpu %% 256 = %llu, gpu %s\n",
                static_cast<unsigned long long>(cpu % 256), static_cast<unsigned long long>(gpu % 256),
                gpu ? "found" : "missing");
    if (!cpu || cpu % 256 || !gpu || gpu % 256) result = invalid_arguments;
    const int closed = c.close(list, "arguments");
    return result ? result : closed;
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "warp") && std::strcmp(argv[1], "hardware"))) return usage;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return no_adapter;
    ComPtr<IDXGIAdapter> adapter;
    if (std::strcmp(argv[1], "hardware") == 0) {
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(n, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) return skip;
            if (FAILED(hr)) return no_adapter;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) return no_adapter;
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                adapter = candidate;
                break;
            }
        }
    } else if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
        return no_adapter;
    }
    DXGI_ADAPTER_DESC adapter_desc{};
    if (FAILED(adapter->GetDesc(&adapter_desc))) return no_adapter;
    char debug[4]{};
    const bool debugging = GetEnvironmentVariableA("X2W_D3D12_DEBUG", debug, sizeof(debug)) && debug[0] == '1';
    std::printf("adapter=%ls debug_layer=%s\n", adapter_desc.Description, debugging ? "requested" : "off");

    Client c;
    std::array<unsigned char, 64> parameters{};
    // The console device first: with X2W_D3D12_DEBUG=1 it enables the debug layer before the desktop device exists.
    if (FAILED(D3D12XboxCreateDevice(adapter.Get(), parameters.data(), __uuidof(ID3D12Device), &c.device))) return no_device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&c.desktop)))) return no_device;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    if (!ok(invoke(c.device, 8, arg(&queue_desc), arg(&__uuidof(ID3D12CommandQueue)), arg(&c.queue))) || !c.observer_setup())
        return setup;

    int result = held_sequence(c, Mode::fresh);
    if (!result) result = held_sequence(c, Mode::allocator);
    if (!result) result = held_sequence(c, Mode::overwrite);
    if (!result) result = forced_reuse(c);
    if (!result) result = arguments(c);
    const HRESULT removed = c.desktop->GetDeviceRemovedReason();
    const UINT64 errors = c.debug_errors();
    std::printf("device removed reason: 0x%08lx, debug layer errors: %llu\n", static_cast<unsigned long>(removed),
                static_cast<unsigned long long>(errors));
    if (!result && (removed != S_OK || errors)) result = device_state;
    if (!result) std::puts("slot 148 acceptance passed");
    else std::printf("slot 148 acceptance failed: exit %d\n", result);
    return result;
}
