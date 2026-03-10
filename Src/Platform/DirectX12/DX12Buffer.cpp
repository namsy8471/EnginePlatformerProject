#include "DX12Buffer.h"

#include "DX12Device.h"
#include "Platform/DirectX12/d3dx12.h"
#include "Utils/Utils.h"

#include <stdexcept>

DX12Buffer::DX12Buffer(DX12Device* device, const BufferDesc& desc)
	: m_device(device), m_size(desc.Size), m_stride(desc.Stride), m_heapType(desc.Heap)
{
	D3D12_HEAP_PROPERTIES heapProperties = {};
	D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;

	if (desc.Heap == HeapType::Upload)
	{
		heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	else if (desc.Heap == HeapType::Readback)
	{
		heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
		initialState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	else
	{
		heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		initialState = D3D12_RESOURCE_STATE_COMMON;
	}

	const D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.Size);
	ThrowIfFailed(m_device->GetD3DDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		initialState,
		nullptr,
		IID_PPV_ARGS(&m_resource)));
}

DX12Buffer::~DX12Buffer()
{
	if (m_mappedData)
	{
		m_resource->Unmap(0, nullptr);
		m_mappedData = nullptr;
	}
}

void* DX12Buffer::GetNativeResource() const
{
	return m_resource.Get();
}

uint64_t DX12Buffer::GetSize() const
{
	return m_size;
}

uint32_t DX12Buffer::GetStride() const
{
	return m_stride;
}

void DX12Buffer::Map(void** ppData)
{
	if (m_heapType == HeapType::Default)
	{
		throw std::runtime_error("Cannot map a DX12 default heap buffer.");
	}

	if (!m_mappedData)
	{
		ThrowIfFailed(m_resource->Map(0, nullptr, &m_mappedData));
	}

	*ppData = m_mappedData;
}

void DX12Buffer::Unmap()
{
	if (m_mappedData)
	{
		m_resource->Unmap(0, nullptr);
		m_mappedData = nullptr;
	}
}

D3D12_VERTEX_BUFFER_VIEW DX12Buffer::GetVertexBufferView() const
{
	D3D12_VERTEX_BUFFER_VIEW view = {};
	view.BufferLocation = m_resource->GetGPUVirtualAddress();
	view.SizeInBytes = static_cast<UINT>(m_size);
	view.StrideInBytes = m_stride;
	return view;
}
