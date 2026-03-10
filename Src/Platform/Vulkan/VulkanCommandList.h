#pragma once

#include "VulkanCommon.h"
#include "RHI/ICommandList.h"

class VulkanDevice;

class VulkanCommandList : public ICommandList
{
public:
	VulkanCommandList(VulkanDevice* device);
	virtual ~VulkanCommandList();

	void* GetNativeResource() const override;

	void Close() override;
	void Reset() override;
	void SetViewport(float x, float y, float width, float height) override;
	void SetScissorRect(long left, long top, long right, long bottom) override;
	void SetRenderTargets(void* rtvHandle, void* dsvHandle) override;
	void ClearRenderTarget(void* rtvHandle, const float color[4]) override;
	void ClearDepthStencil(void* dsvHandle, float depth, uint8_t stencil) override;
	void SetVertexBuffer(IBuffer* buffer) override;
	void SetIndexBuffer(IBuffer* buffer) override;
	void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) override;
	void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, uint32_t baseVertex, uint32_t startInstance) override;
	void ResourceBarrier(IGpuResource* resource, ResourceState before, ResourceState after) override;

private:
	void BeginRenderPassIfNeeded();
	VkImageLayout TranslateResourceState(ResourceState state) const;

private:
	// 디바이스 참조를 통해 현재 프레임의 framebuffer, render pass, command buffer에 접근합니다.
	VulkanDevice* m_device = nullptr;

	// 현재 프레임에 기록 중인 실제 Vulkan 커맨드 버퍼입니다.
	VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;

	// RenderPass를 언제 열고 닫을지 추적하기 위한 상태값들입니다.
	bool m_isRecording = false;
	bool m_renderTargetsSet = false;
	bool m_renderPassBegun = false;
	bool m_hasClearColor = false;
	bool m_hasClearDepth = false;

	// 현재 프레임의 clear color / clear depth-stencil 값을 보관합니다.
	VkClearValue m_clearValues[2] = {};
};
