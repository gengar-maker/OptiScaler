#define NOMINMAX
#include "D3D12CommandSplice.h"
#include <windows.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <detours/detours.h>
#include <array>
#include <atomic>
#include <unordered_set>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace D3D12CommandSplice
{
namespace
{
using Microsoft::WRL::ComPtr;
thread_local unsigned internal = 0;
Log logger;
std::mutex registryMutex;
std::mutex installMutex;
bool installed = false;
std::atomic<unsigned> hookGeneration { 1 };
std::array<std::vector<void*>, 86> knownTargets;
std::unordered_set<void**> observedTables;
unsigned nextImplementation = 1;
using HookTarget = std::pair<unsigned, void*>;
bool KnownTarget(unsigned slot, void* address)
{
    const auto& targets = knownTargets[slot];
    return std::find(targets.begin(), targets.end(), address) != targets.end();
}
void RememberTargets(const std::vector<HookTarget>& targets)
{
    for (const auto& [slot, address] : targets)
        knownTargets[slot].push_back(address);
}
using Replay = std::function<void(ID3D12GraphicsCommandList*)>;
struct Pending
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> tail;
    Work work;
    bool closed = false;
};
// RTV/DSV descriptors are consumed when OMSetRenderTargets is recorded, not
// when the GPU executes. The application may overwrite/free its CPU heap
// immediately afterwards. Own descriptor CONTENTS, never just their handles.
// Only the last OM binding matters: state replay contains no draw commands.
struct RenderTargetState
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12DescriptorHeap> renderTargets;
    ComPtr<ID3D12DescriptorHeap> depthStencil;
    UINT count = 0;
    bool hasDepth = false;
    bool bound = false;

    bool Capture(ID3D12GraphicsCommandList* list, UINT newCount, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                 BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
    {
        if (newCount > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (newCount && !handles))
            return false;
        if (!device && FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;
        if (newCount && !renderTargets)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
            if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&renderTargets))))
                return false;
        }
        if (depth && !depthStencil)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            desc.NumDescriptors = 1;
            if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&depthStencil))))
                return false;
        }
        if (newCount)
        {
            const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            auto destination = renderTargets->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < newCount; ++i)
            {
                auto source =
                    contiguous ? D3D12_CPU_DESCRIPTOR_HANDLE { handles[0].ptr + SIZE_T(i) * stride } : handles[i];
                device->CopyDescriptorsSimple(1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                destination.ptr += stride;
            }
        }
        if (depth)
            device->CopyDescriptorsSimple(1, depthStencil->GetCPUDescriptorHandleForHeapStart(), *depth,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        count = newCount;
        hasDepth = depth != nullptr;
        bound = true;
        return true;
    }

    void Replay(ID3D12GraphicsCommandList* list) const
    {
        if (!bound)
            return;
        const auto colour =
            count ? renderTargets->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE {};
        const auto depth =
            hasDepth ? depthStencil->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE {};
        list->OMSetRenderTargets(count, count ? &colour : nullptr, TRUE, hasDepth ? &depth : nullptr);
    }
};
struct Recording
{
    bool resetObserved = false;
    unsigned generation = 0;
    unsigned queries = 0;
    bool renderPass = false;
    bool splitBarrier = false;
    std::string unsupported;
    std::vector<Replay> state;
    RenderTargetState targets;
    std::shared_ptr<Pending> pending;
};
std::unordered_map<ID3D12GraphicsCommandList*, std::shared_ptr<Recording>> recordings;
std::shared_ptr<Recording> Get(ID3D12GraphicsCommandList* list)
{
    std::lock_guard lock(registryMutex);
    auto& p = recordings[list];
    if (!p)
        p = std::make_shared<Recording>();
    return p;
}
void Report(const std::string& text)
{
    if (logger)
        logger(text);
}
template <class T> auto CopyArray(const T* p, size_t count)
{
    return p && count ? std::vector<T>(p, p + count) : std::vector<T> {};
}

// State setters need owned copies of their pointer arguments. The arrays are
// replayed immediately at Begin. RTV/DSV contents are separately snapshotted
// at bind time because application CPU descriptor slots need not stay valid.
template <int Slot, class C, class Fn, class... A> Replay StateCall(Fn fn, A... args)
{
    auto t = std::tuple(args...);
    if constexpr (Slot == 21 || Slot == 22 || Slot == 28)
    {
        auto values = CopyArray(std::get<1>(t), std::get<0>(t));
        return [fn, values](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), static_cast<UINT>(values.size()), values.data()); };
    }
    else if constexpr (Slot == 23)
    {
        auto values = CopyArray(std::get<0>(t), 4);
        return [fn, values](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), values.empty() ? nullptr : values.data()); };
    }
    else if constexpr (Slot == 35 || Slot == 36)
    {
        auto values = CopyArray(static_cast<const UINT*>(std::get<2>(t)), std::get<1>(t));
        return [fn, values, root = std::get<0>(t), offset = std::get<3>(t)](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), root, static_cast<UINT>(values.size()), values.data(), offset); };
    }
    else if constexpr (Slot == 43)
    {
        bool present = std::get<0>(t) != nullptr;
        D3D12_INDEX_BUFFER_VIEW view = present ? *std::get<0>(t) : D3D12_INDEX_BUFFER_VIEW {};
        return [fn, present, view](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), present ? &view : nullptr); };
    }
    else if constexpr (Slot == 44 || Slot == 45)
    {
        const bool present = std::get<2>(t) != nullptr;
        auto values = CopyArray(std::get<2>(t), std::get<1>(t));
        return [fn, values, present, start = std::get<0>(t), count = std::get<1>(t)](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), start, count, present ? values.data() : nullptr); };
    }
    else if constexpr (Slot == 63)
    {
        auto values = CopyArray(std::get<2>(t), size_t(std::get<0>(t)) * std::get<1>(t));
        return [fn, values, samples = std::get<0>(t), pixels = std::get<1>(t)](ID3D12GraphicsCommandList* list) mutable
        { fn(static_cast<C*>(list), samples, pixels, values.empty() ? nullptr : values.data()); };
    }
    else if constexpr (Slot == 77)
    {
        const bool present = std::get<1>(t) != nullptr;
        auto values = CopyArray(std::get<1>(t), 2);
        return [fn, values, present, rate = std::get<0>(t)](ID3D12GraphicsCommandList* list)
        { fn(static_cast<C*>(list), rate, present ? values.data() : nullptr); };
    }
    else
    {
        return [fn, t](ID3D12GraphicsCommandList* list)
        { std::apply([&](auto... a) { fn(static_cast<C*>(list), a...); }, t); };
    }
}
template <int Slot, class Signature, unsigned Implementation = 0> struct Hook;
template <int Slot, class C, class R, unsigned Implementation, class... A>
struct Hook<Slot, R (STDMETHODCALLTYPE C::*)(A...), Implementation>
{
    using Fn = R(STDMETHODCALLTYPE*)(C*, A...);
    inline static Fn original = nullptr;
    static R Forward(C* target, A... args)
    {
        // A trampoline belongs to its source implementation, not every COM
        // object. Dispatch through the owned tail's actual implementation.
        InternalScope scope;
        auto** table = *reinterpret_cast<void***>(target);
        return reinterpret_cast<Fn>(table[Slot])(target, args...);
    }
    static R STDMETHODCALLTYPE Call(C* self, A... args)
    {
        if (internal)
            return original(self, args...);
        auto* list = static_cast<ID3D12GraphicsCommandList*>(self);
        if constexpr (Slot == 10)
        {
            const auto result = original(self, args...);
            if (SUCCEEDED(result))
            {
                auto r = std::make_shared<Recording>();
                r->resetObserved = true;
                r->generation = hookGeneration.load();
                auto t = std::tuple(args...);
                if (auto* pso = std::get<1>(t))
                    r->state.emplace_back([pso](auto* l) { l->SetPipelineState(pso); });
                std::lock_guard lock(registryMutex);
                recordings[list] = r;
            }
            return result;
        }
        else if constexpr (Slot == 9)
        {
            auto r = Get(list);
            auto result = original(self, args...);
            if (r->pending)
            {
                InternalScope scope;
                const auto tailResult = r->pending->tail->Close();
                r->pending->closed = SUCCEEDED(result) && SUCCEEDED(tailResult);
                if (SUCCEEDED(result))
                    result = tailResult;
            }
            // No deferred CPU replay remains after Close. Avoid retaining
            // application resources on command-list identities indefinitely.
            r->state.clear();
            r->targets = {};
            return result;
        }
        else
        {
            auto r = Get(list);
            if (r->pending)
                return Forward(static_cast<C*>(r->pending->tail.Get()), args...);
            auto t = std::tuple(args...);
            if constexpr (Slot == 11)
            {
                r->state.clear();
                r->targets.bound = false;
            }
            if constexpr (Slot == 46)
            {
                if (!r->targets.Capture(list, args...))
                    r->unsupported = "render-target descriptor snapshot failed";
            }
            if constexpr (Slot == 11 || (Slot >= 20 && Slot <= 25) || (Slot >= 28 && Slot <= 45) || Slot == 55 ||
                          Slot == 62 || Slot == 63 || Slot == 65 || Slot == 75 || Slot == 77 || Slot == 78 ||
                          Slot == 81 || Slot == 82 || Slot == 83)
            {
                if (r->state.size() < 131072)
                    r->state.push_back(StateCall<Slot, C>(Forward, args...));
                else
                    r->unsupported = "state replay limit exceeded";
            }
            if constexpr (Slot == 26)
            {
                for (UINT i = 0; i < std::get<0>(t); ++i)
                {
                    const auto& b = std::get<1>(t)[i];
                    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                        continue;
                    if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
                        r->splitBarrier = true;
                }
            }
            if constexpr (Slot == 52)
                ++r->queries;
            if constexpr (Slot == 53)
            {
                if (std::get<1>(t) != D3D12_QUERY_TYPE_TIMESTAMP && r->queries)
                    --r->queries;
            }
            if constexpr (Slot == 68)
                r->renderPass = true;
            if constexpr (Slot == 69)
                r->renderPass = false;
            if constexpr (Slot == 27 || Slot == 59 || Slot == 67 || Slot == 80 || Slot == 84)
                r->unsupported = "prefix uses a state mutation not yet supported by splice";
            return original(self, args...);
        }
    }
};

// Enroll other threads only after all COM discovery and DetourAttach calls.
// No graphics API/registry locks are taken while those threads are suspended.
struct DetourThreads
{
    std::vector<HANDLE> handles;
    LONG Enroll()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return GetLastError();
        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        LONG error = NO_ERROR;
        if (Thread32First(snapshot, &entry))
            do
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                    continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                               THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                                           FALSE, entry.th32ThreadID);
                if (thread)
                    handles.push_back(thread);
                else if (GetLastError() != ERROR_INVALID_PARAMETER)
                {
                    error = GetLastError();
                    break;
                }
            } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
        if (error != NO_ERROR)
            return error;
        for (auto thread : handles)
        {
            const auto result = DetourUpdateThread(thread);
            if (result != NO_ERROR && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0)
                return result;
        }
        return NO_ERROR;
    }
    ~DetourThreads()
    {
        for (auto handle : handles)
            CloseHandle(handle);
    }
};

template <unsigned Implementation>
void AttachImplementation(ID3D12GraphicsCommandList* list, LONG& result, std::vector<HookTarget>& staged)
{
    // Every implementation gets its own typed Detours trampolines. The game
    // may use different entry points from CreateCommandList on our probe.
#include "D3D12CommandSplice_Hooks.inc"
}
bool EnsureImplementation(ID3D12GraphicsCommandList* list, std::string& error)
{
    std::lock_guard lock(installMutex);
    auto** table = *reinterpret_cast<void***>(list);
    if (observedTables.contains(table))
        return true;
    if (nextImplementation >= 8)
    {
        error = "command-list implementation limit reached";
        return false;
    }
    InternalScope scope;
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR)
        result = DetourUpdateThread(GetCurrentThread());
    std::vector<HookTarget> staged;
    switch (nextImplementation)
    {
    case 1:
        AttachImplementation<1>(list, result, staged);
        break;
    case 2:
        AttachImplementation<2>(list, result, staged);
        break;
    case 3:
        AttachImplementation<3>(list, result, staged);
        break;
    case 4:
        AttachImplementation<4>(list, result, staged);
        break;
    case 5:
        AttachImplementation<5>(list, result, staged);
        break;
    case 6:
        AttachImplementation<6>(list, result, staged);
        break;
    case 7:
        AttachImplementation<7>(list, result, staged);
        break;
    }
    DetourThreads threads;
    if (result == NO_ERROR && !staged.empty())
        result = threads.Enroll();
    if (result == NO_ERROR)
        result = DetourTransactionCommit();
    else
        DetourTransactionAbort();
    if (result != NO_ERROR)
    {
        error = "actual command-list hook installation failed: " + std::to_string(result);
        return false;
    }
    RememberTargets(staged);
    observedTables.insert(table);
    if (!staged.empty())
    {
        // Prefixes recorded before complete hook coverage cannot be replayed.
        // A generation avoids racing other threads' per-list state vectors.
        ++hookGeneration;
        std::ostringstream message;
        message << "splice: added actual command-list implementation " << nextImplementation
                << "; newly tracked slots=";
        for (const auto& [slot, address] : staged)
            message << slot << ',';
        message << "; waiting for fresh Reset";
        Report(message.str());
        ++nextImplementation;
    }
    return true;
}

using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn executeOriginal = nullptr;
std::mutex submissionMutex;
// Retain allocations on a failed fence: their GPU use may still be pending.
std::vector<std::shared_ptr<Pending>> failedSubmissions;
bool Wait(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    bool result = SUCCEEDED(queue->Signal(fence.Get(), 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                  WaitForSingleObject(event, 30000) == WAIT_OBJECT_0;
    if (result)
        CloseHandle(event);
    else
    {
        // A fence may still signal this event after timeout. Keep both alive.
        static auto* abandoned = new std::vector<std::pair<ComPtr<ID3D12Fence>, HANDLE>>;
        abandoned->emplace_back(fence, event);
    }
    return result;
}
void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (internal)
    {
        executeOriginal(queue, count, lists);
        return;
    }
    std::vector<std::shared_ptr<Pending>> pending(count);
    bool any = false;
    {
        std::lock_guard lock(registryMutex);
        for (UINT i = 0; i < count; ++i)
        {
            auto it = recordings.find(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (it != recordings.end() && it->second->pending)
            {
                pending[i] = it->second->pending;
                any = true;
            }
        }
    }
    if (!any)
    {
        executeOriginal(queue, count, lists);
        return;
    }
    std::lock_guard lock(submissionMutex);
    InternalScope scope;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        Report("splice: invalid submission queue; continuation was not submitted");
        return;
    }
    UINT begin = 0;
    for (UINT i = 0; i < count; ++i)
    {
        auto p = pending[i];
        if (!p)
            continue;
        executeOriginal(queue, i - begin + 1, lists + begin);
        if (!p->closed || !Wait(device.Get(), queue))
        {
            failedSubmissions.push_back(p);
            Report("splice: producer fence/Close failed");
            return;
        }
        try
        {
            if (p->work)
                p->work(device.Get(), queue);
        }
        catch (const std::exception& e)
        {
            failedSubmissions.push_back(p);
            Report(std::string("splice: callback failed; retaining GPU resources: ") + e.what());
            return;
        }
        catch (...)
        {
            failedSubmissions.push_back(p);
            Report("splice: callback failed with unknown exception");
            return;
        }
        ID3D12CommandList* tail[] = { p->tail.Get() };
        executeOriginal(queue, 1, tail);
        if (!Wait(device.Get(), queue))
        {
            failedSubmissions.push_back(p);
            Report("splice: continuation fence failed");
            return;
        }
        begin = i + 1;
    }
    if (begin < count)
        executeOriginal(queue, count - begin, lists + begin);
}
} // namespace

InternalScope::InternalScope() { ++internal; }
InternalScope::~InternalScope() { --internal; }
bool Begin(ID3D12GraphicsCommandList* list, Work work, std::string& error)
{
    if (!installed || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        error = "splice not initialized for a direct list";
        return false;
    }
    if (!EnsureImplementation(list, error))
        return false;
    auto r = Get(list);
    if (!r->resetObserved || r->generation != hookGeneration.load() || r->pending || r->queries || r->renderPass ||
        r->splitBarrier || !r->unsupported.empty())
    {
        error = !r->unsupported.empty() ? r->unsupported : "list lacks a clean reset boundary or has an open scope";
        return false;
    }
    InternalScope scope;
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
    {
        error = "GetDevice failed";
        return false;
    }
    auto p = std::make_shared<Pending>();
    p->work = std::move(work);
    auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p->allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&p->tail));
    if (FAILED(hr))
    {
        error = "continuation allocation failed";
        return false;
    }
    // Do not manufacture COMMON -> previous-state barriers. D3D12 permits
    // existing promotable BeforeState values for decayed resources; first
    // access promotes them again. Non-decaying resources retain their state.
    // The callback must restore the input states of every resource it touches.
    // See Microsoft "Using Resource Barriers", Performance implications.
    for (const auto& replay : r->state)
        replay(p->tail.Get());
    r->targets.Replay(p->tail.Get());
    r->pending = p;
    error.clear();
    return true;
}
ID3D12GraphicsCommandList* Continuation(ID3D12GraphicsCommandList* list)
{
    auto r = Get(list);
    return r->pending ? r->pending->tail.Get() : list;
}

bool Install(ID3D12Device* device, Log log, std::string& error)
{
    std::lock_guard lock(installMutex);
    if (installed)
        return true;
    InternalScope scope;
    logger = std::move(log);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12CommandQueue> queue;
    auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr =
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    D3D12_COMMAND_QUEUE_DESC q {};
    if (SUCCEEDED(hr))
        hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
    {
        error = "splice hook discovery failed";
        return false;
    }
    void** table = *reinterpret_cast<void***>(list.Get());
    void** queueTable = *reinterpret_cast<void***>(queue.Get());
    LONG result = DetourTransactionBegin();
    if (result == NO_ERROR)
        result = DetourUpdateThread(GetCurrentThread());
    std::vector<HookTarget> staged;
    AttachImplementation<0>(list.Get(), result, staged);
    executeOriginal = reinterpret_cast<ExecuteFn>(queueTable[10]);
    if (result == NO_ERROR)
        result = DetourAttach(reinterpret_cast<PVOID*>(&executeOriginal), ExecuteHook);
    DetourThreads threads;
    if (result == NO_ERROR)
        result = threads.Enroll();
    if (result == NO_ERROR)
        result = DetourTransactionCommit();
    else
        DetourTransactionAbort();
    if (result != NO_ERROR)
    {
        error = "splice detour installation failed: " + std::to_string(result);
        return false;
    }
    RememberTargets(staged);
    observedTables.insert(table);
    installed = true;
    error.clear();
    return true;
}
} // namespace D3D12CommandSplice
