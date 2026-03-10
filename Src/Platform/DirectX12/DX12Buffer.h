#pragma once

#include "RHI/IGraphicsDevice.h"
#include "RHI/IBuffer.h"
#include <d3d12.h>
#include <wrl.h>

class DX12Device;

class DX12Buffer : public IBuffer
{
public:
	DX12Buffer(DX12Device* device, const BufferDesc& desc);
	virtual ~DX12Buffer();

	void* GetNativeResource() const override;
	uint64_t GetSize() const noexcept override;
	uint32_t GetStride() const noexcept override;
	void Map(void** ppData) override;
	void Unmap() override;

	D3D12_VERTEX_BUFFER_VIEW GetVertexBufferView() const;
	D3D12_INDEX_BUFFER_VIEW GetIndexBufferView() const;

private:
	DX12Device* m_device = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
	uint64_t m_size = 0;
	uint32_t m_stride = 0;
	HeapType m_heapType = HeapType::Default;
	void* m_mappedData = nullptr;
};
