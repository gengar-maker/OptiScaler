#pragma once
#include <array>
#include <d3d12.h>
#include <wrl/client.h>

// GPU execution intervals only. Read after the caller's existing completion
// fence; instrumentation never adds a submission or a wait.
class NrGpuTimer
{
  public:
    enum Stage
    {
        Fallback,
        Encode,
        InputCopy,
        OutputCopy,
        Resolve,
        Count
    };
    void BeginFrame(ID3D12Device* device, ID3D12CommandQueue* queue)
    {
        mask_ = 0;
        if (device_.Get() == device)
            return;
        device_ = device;
        queries_.Reset();
        readback_.Reset();
        frequency_ = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency_)) || !frequency_)
            return;
        D3D12_QUERY_HEAP_DESC q {};
        q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        q.Count = Count * 2;
        if (FAILED(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries_))))
            return;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = Count * 2 * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback_))))
            queries_.Reset();
    }
    void Start(ID3D12GraphicsCommandList* list, Stage stage)
    {
        if (queries_ && readback_)
            list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stage * 2);
    }
    void Stop(ID3D12GraphicsCommandList* list, Stage stage)
    {
        if (!queries_ || !readback_)
            return;
        list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stage * 2 + 1);
        list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stage * 2, 2, readback_.Get(),
                               stage * 2 * sizeof(UINT64));
        mask_ |= 1u << stage;
    }
    std::array<double, Count> ReadCompleted()
    {
        std::array<double, Count> result {};
        result.fill(-1);
        if (!readback_ || !frequency_ || !mask_)
            return result;
        D3D12_RANGE range { 0, Count * 2 * sizeof(UINT64) };
        void* mapped = nullptr;
        if (FAILED(readback_->Map(0, &range, &mapped)))
            return result;
        const auto* ticks = static_cast<const UINT64*>(mapped);
        for (unsigned i = 0; i < Count; ++i)
            if ((mask_ & (1u << i)) && ticks[i * 2 + 1] >= ticks[i * 2])
                result[i] = double(ticks[i * 2 + 1] - ticks[i * 2]) * 1000.0 / double(frequency_);
        D3D12_RANGE written { 0, 0 };
        readback_->Unmap(0, &written);
        return result;
    }

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    UINT64 frequency_ = 0;
    unsigned mask_ = 0;
};
