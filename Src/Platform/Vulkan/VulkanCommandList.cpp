#include "VulkanCommandList.h"

#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanResource.h"

#include <stdexcept>

// VulkanCommandList는 현재 프레임의 VkCommandBuffer를 기록하는 역할을 맡습니다.
VulkanCommandList::VulkanCommandList(VulkanDevice* device)
	: m_device(device)
{
}

VulkanCommandList::~VulkanCommandList()
{
}

void* VulkanCommandList::GetNativeResource() const
{
	// VkCommandBuffer는 dispatchable handle이므로 void*로 전달할 수 있습니다.
	return m_commandBuffer;
}

void VulkanCommandList::Close()
{
	// 렌더패스가 열려 있다면 먼저 종료해야 커맨드 버퍼를 정상적으로 닫을 수 있습니다.
	if (m_renderPassBegun)
	{
		vkCmdEndRenderPass(m_commandBuffer);
		m_renderPassBegun = false;
	}

	if (m_isRecording)
	{
		m_device->EndFrameCommandRecording();
		m_isRecording = false;
	}
}

void VulkanCommandList::Reset()
{
	// 현재 프레임에 사용할 커맨드 버퍼를 가져와 기록 상태를 초기화합니다.
	m_device->BeginFrameCommandRecording();
	m_commandBuffer = m_device->m_commandBuffer;
	m_isRecording = true;
	m_renderTargetsSet = false;
	m_renderPassBegun = false;
	m_hasClearColor = false;
	m_hasClearDepth = false;
	m_clearValues[0] = {};
	m_clearValues[1] = {};
}

void VulkanCommandList::SetViewport(float x, float y, float width, float height)
{
	// Vulkan은 viewport를 명령 버퍼에 기록해 현재 프레임 렌더 상태로 적용합니다.
	VkViewport viewport = {};
	viewport.x = x;
	viewport.y = y;
	viewport.width = width;
	viewport.height = height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);
}

void VulkanCommandList::SetScissorRect(long left, long top, long right, long bottom)
{
	// Vulkan은 scissor도 명령 버퍼에 기록합니다.
	VkRect2D scissor = {};
	scissor.offset.x = left;
	scissor.offset.y = top;
	scissor.extent.width = static_cast<uint32_t>(right - left);
	scissor.extent.height = static_cast<uint32_t>(bottom - top);
	vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);
}

void VulkanCommandList::SetRenderTargets(void* rtvHandle, void* dsvHandle)
{
	// Vulkan은 DX12식 RTV/DSV 핸들을 직접 바인딩하지 않고 framebuffer + render pass 조합으로 처리합니다.
	// 여기서는 이후 Clear 호출 시 렌더패스를 열 수 있도록 상태만 표시합니다.
	m_renderTargetsSet = true;
}

void VulkanCommandList::ClearRenderTarget(void* rtvHandle, const float color[4])
{
	// 현재 프레임의 컬러 clear 값을 저장합니다.
	m_clearValues[0].color.float32[0] = color[0];
	m_clearValues[0].color.float32[1] = color[1];
	m_clearValues[0].color.float32[2] = color[2];
	m_clearValues[0].color.float32[3] = color[3];
	m_hasClearColor = true;
}

void VulkanCommandList::ClearDepthStencil(void* dsvHandle, float depth, uint8_t stencil)
{
	// 현재 프레임의 depth/stencil clear 값을 저장합니다.
	m_clearValues[1].depthStencil.depth = depth;
	m_clearValues[1].depthStencil.stencil = stencil;
	m_hasClearDepth = true;

	// main.cpp의 현재 호출 순서는 ClearRenderTarget -> ClearDepthStencil이므로,
	// 두 clear 값이 모두 모인 시점에 render pass를 시작합니다.
	BeginRenderPassIfNeeded();
}

void VulkanCommandList::SetVertexBuffer(IBuffer* buffer)
{
	auto vkBuffer = dynamic_cast<VulkanBuffer*>(buffer);
	if (!vkBuffer)
	{
		return;
	}

	const VkBuffer vertexBuffer = vkBuffer->GetVkBuffer();
	const VkDeviceSize offsets[] = { 0 };
	vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, &vertexBuffer, offsets);
}

void VulkanCommandList::SetIndexBuffer(IBuffer* buffer)
{
	// vkCmdBindIndexBuffer는 실제 Vulkan 버퍼 구현 후 연결합니다.
}

void VulkanCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance)
{
	vkCmdDraw(m_commandBuffer, vertexCount, instanceCount, startVertex, startInstance);
}

void VulkanCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, uint32_t baseVertex, uint32_t startInstance)
{
	// 파이프라인 상태가 아직 없으므로 실제 DrawIndexed도 연결하지 않습니다.
}

void VulkanCommandList::ResourceBarrier(IGpuResource* resource, ResourceState before, ResourceState after)
{
	// Vulkan의 리소스 상태 전환은 image layout 전환과 pipeline barrier로 처리합니다.
	// 현재 단계에서는 스왑체인 이미지의 Present <-> RenderTarget 전환만 다룹니다.
	auto vkResource = dynamic_cast<VulkanImageResource*>(resource);
	if (!vkResource)
	{
		return;
	}

	const VkImageLayout newLayout = TranslateResourceState(after);
	if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED)
	{
		return;
	}

	// Vulkan에서는 color attachment로 사용 중인 이미지를 Present로 전환하기 전에 render pass를 끝내야 합니다.
	if (m_renderPassBegun && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		vkCmdEndRenderPass(m_commandBuffer);
		m_renderPassBegun = false;
	}

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = m_device->m_swapchainImageLayouts[m_device->m_currentBackBufferIndex];
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vkResource->GetImage();
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	if (barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
	{
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = 0;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	}
	else
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	}

	vkCmdPipelineBarrier(
		m_commandBuffer,
		srcStage,
		dstStage,
		0,
		0,
		nullptr,
		0,
		nullptr,
		1,
		&barrier);

	m_device->m_swapchainImageLayouts[m_device->m_currentBackBufferIndex] = newLayout;
}

void VulkanCommandList::BeginRenderPassIfNeeded()
{
	// RenderPass는 framebuffer와 clear 값이 모두 준비된 뒤 한 번만 시작해야 합니다.
	if (!m_isRecording || m_renderPassBegun || !m_renderTargetsSet || !m_hasClearColor || !m_hasClearDepth)
	{
		return;
	}

	VkRenderPassBeginInfo renderPassBeginInfo = {};
	renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBeginInfo.renderPass = m_device->m_renderPass;
	renderPassBeginInfo.framebuffer = m_device->m_framebuffers[m_device->m_currentBackBufferIndex];
	renderPassBeginInfo.renderArea.offset = { 0, 0 };
	renderPassBeginInfo.renderArea.extent = m_device->m_swapchainExtent;
	renderPassBeginInfo.clearValueCount = 2;
	renderPassBeginInfo.pClearValues = m_clearValues;

	vkCmdBeginRenderPass(m_commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_renderPassBegun = true;
}

VkImageLayout VulkanCommandList::TranslateResourceState(ResourceState state) const
{
	switch (state)
	{
	case ResourceState::RenderTarget:
		return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	case ResourceState::Present:
		return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	default:
		return VK_IMAGE_LAYOUT_UNDEFINED;
	}
}
