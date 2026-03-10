#pragma once

#include "RHI/ICommandList.h"
#include <d3d12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

class DX12Device;

class DX12CommandList : public ICommandList
{
public:
	DX12CommandList(DX12Device* device);
	virtual ~DX12CommandList();

	// ICommandList 구현
	void* GetNativeResource() const override;

	// 기본 제어
	void Reset() override;
	void Close() override;

	// 상태 설정
	void SetViewport(float x, float y, float width, float height) override;
	void SetScissorRect(long left, long top, long right, long bottom) override;

	// 렌더타겟 설정
	void SetRenderTargets(void* rtvHandle, void* dsvHandle) override;
	
	// 화면 클리어
	void ClearRenderTarget(void* rtvHandle, const float color[4]) override;
	void ClearDepthStencil(void* dsvHandle, float depth, uint8_t stencil) override;
	
	// 리소스 바인딩
	void SetVertexBuffer(IBuffer* buffer) override;
	void SetIndexBuffer(IBuffer* buffer) override;
	
	// 그리기 호출
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, uint32_t baseVertex, uint32_t startInstance) override;
	
	// 동기화
	void ResourceBarrier(IGpuResource* resource, ResourceState before, ResourceState after) override;

private:
	D3D12_RESOURCE_STATES TranslateResourceState(ResourceState state);

private:
	// ����̽� ���� (Alloc �� ��Ÿ �۾��� �ʿ�)
	DX12Device* m_device;

	// Ŀ�ǵ� ����Ʈ
	ComPtr<ID3D12GraphicsCommandList> m_commandList;

	// Ŀ�ǵ� �Ҵ���
	ComPtr<ID3D12CommandAllocator> m_commandAllocator[2];
	UINT m_currentCommandListIndex = 0;

};

