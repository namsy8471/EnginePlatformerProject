#pragma once
#include "GraphicsCommon.h"
#include "IGpuResource.h"

class IBuffer;

class ICommandList : public IGpuResource
{
public:
	virtual ~ICommandList() = default;

	// 기본 제어
	virtual void Close() = 0;
	virtual void Reset() = 0;

	// 상태 설정
	virtual void SetViewport(float x, float y, float width, float height) = 0;
	virtual void SetScissorRect(long left, long top, long right, long bottom) = 0;

	// 렌더타겟 설정
	virtual void SetRenderTargets(void* rtvHandle, void* dsvHandle) = 0;

	// 화면 지우기
	virtual void ClearRenderTarget(void* rtvHandle, const float color[4]) = 0;
	virtual void ClearDepthStencil(void* dsvHandle, float depth = 1.0f, uint8_t stencil = 0) = 0;

	// 리소스 바인딩
	virtual void SetVertexBuffer(IBuffer* buffer) = 0;
	virtual void SetIndexBuffer(IBuffer* buffer) = 0;

	// 그리기 명령
	virtual void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) = 0;
	virtual void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, uint32_t baseVertex, uint32_t startInstance) = 0;

	
	// 동기화
	virtual void ResourceBarrier(IGpuResource* resource, ResourceState before, ResourceState after) = 0;
};