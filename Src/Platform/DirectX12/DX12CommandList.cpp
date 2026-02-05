#include "DX12CommandList.h"
#include "Platform/DirectX12/DX12Device.h"
#include "Utils/Utils.h"

#include <stdexcept>
#include <string>

DX12CommandList::DX12CommandList(DX12Device* device) : m_device(device)
{
	ID3D12Device* d3dDevice = m_device->GetD3DDevice();

	// 커맨드 할당자 생성
	// 매 프레임 리셋해야 하므로 가장 중요함
	ThrowIfFailed(d3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(&m_commandAllocator)
	));

	// 커맨드 리스트 생성
	ThrowIfFailed(d3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_commandAllocator.Get(),
		nullptr,
		IID_PPV_ARGS(&m_commandList)
	));

	// 커맨드 리스트는 생성 직후에 열려 있으므로 닫아줌
	ThrowIfFailed(m_commandList->Close());
}

DX12CommandList::~DX12CommandList()
{
	// ComPtr는 자동으로 해제됨
}

void* DX12CommandList::GetNativeResource() const
{
	return m_commandList.Get();
}

void DX12CommandList::Reset()
{
	// 커맨드 할당자 및 리스트 리셋
	ThrowIfFailed(m_commandAllocator->Reset());
	ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));
}

void DX12CommandList::Close()
{
	// 커맨드 리스트 닫기
	ThrowIfFailed(m_commandList->Close());
}

void DX12CommandList::SetViewport(float x, float y, float width, float height)
{
	D3D12_VIEWPORT viewport = {};
	viewport.TopLeftX = x;
	viewport.TopLeftY = y;
	viewport.Width = width;
	viewport.Height = height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	m_commandList->RSSetViewports(1, &viewport);
}

void DX12CommandList::SetScissorRect(long left, long top, long right, long bottom)
{
	D3D12_RECT scissorRect = {};
	scissorRect.left = left;
	scissorRect.top = top;
	scissorRect.right = right;
	scissorRect.bottom = bottom;
	m_commandList->RSSetScissorRects(1, &scissorRect);
}

void DX12CommandList::SetRenderTargets(void* rtvHandle, void* dsvHandle)
{
	D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle = {};
	D3D12_CPU_DESCRIPTOR_HANDLE dsvCpuHandle = {};

	if(rtvHandle) {
		rtvCpuHandle.ptr = reinterpret_cast<SIZE_T>(rtvHandle);
	}
	if(dsvHandle) {
		dsvCpuHandle.ptr = reinterpret_cast<SIZE_T>(dsvHandle);
	}

	m_commandList->OMSetRenderTargets(
		1,
		&rtvCpuHandle,
		FALSE,
		dsvHandle ? &dsvCpuHandle : nullptr
	);
}

void DX12CommandList::ClearRenderTarget(void* rtvHandle, const float color[4])
{
	// RTV 핸들 변환
	D3D12_CPU_DESCRIPTOR_HANDLE rtvCpuHandle = {};
	rtvCpuHandle.ptr = reinterpret_cast<SIZE_T>(rtvHandle);

	// 렌더 타겟 클리어
	m_commandList->ClearRenderTargetView(rtvCpuHandle, color, 0, nullptr);
}

void DX12CommandList::ClearDepthStencil(void* dsvHandle, float depth, uint8_t stencil)
{
	D3D12_CPU_DESCRIPTOR_HANDLE dsvCpuHandle = {};
	dsvCpuHandle.ptr = reinterpret_cast<SIZE_T>(dsvHandle);

	m_commandList->ClearDepthStencilView(
		dsvCpuHandle,
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
		depth,
		stencil,
		0,
		nullptr
	);
}

void DX12CommandList::SetVertexBuffer(IBuffer* buffer)
{
	//TODO: 구현 필요
}

void DX12CommandList::SetIndexBuffer(IBuffer* buffer)
{
	//TODO: 구현 필요
}

void DX12CommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
{
	m_commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
}

void DX12CommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, uint32_t baseVertex, uint32_t startInstance)
{
	m_commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
}

void DX12CommandList::ResourceBarrier(IGpuResource* resource, ResourceState before, ResourceState after)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = static_cast<ID3D12Resource*>(resource->GetNativeResource());
	barrier.Transition.StateBefore = TranslateResourceState(before);
	barrier.Transition.StateAfter = TranslateResourceState(after);
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	m_commandList->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_STATES DX12CommandList::TranslateResourceState(ResourceState state)
{
	switch(state)
	{
		case ResourceState::Common: return D3D12_RESOURCE_STATE_COMMON;
		case ResourceState::VertexAndConstantBuffer: return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		case ResourceState::IndexBuffer: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
		case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case ResourceState::DepthStencil: return D3D12_RESOURCE_STATE_DEPTH_WRITE; // 보통 Write로 씀
		case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case ResourceState::PixelShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		case ResourceState::NonPixelShaderResource: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
		case ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
		case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
		case ResourceState::GenericRead: return D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	return D3D12_RESOURCE_STATE_COMMON;
}
