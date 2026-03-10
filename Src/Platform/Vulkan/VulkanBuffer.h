#pragma once

#include "VulkanCommon.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/IBuffer.h"

class VulkanDevice;

// VulkanBuffer는 RHI의 IBuffer를 VkBuffer + VkDeviceMemory 조합으로 구현합니다.
class VulkanBuffer : public IBuffer
{
public:
	VulkanBuffer(VulkanDevice* device, const BufferDesc& desc);
	virtual ~VulkanBuffer();

	void* GetNativeResource() const override;
	uint64_t GetSize() const override;
	uint32_t GetStride() const override;
	void Map(void** ppData) override;
	void Unmap() override;

	VkBuffer GetVkBuffer() const;

private:
	VulkanDevice* m_device = nullptr;
	uint64_t m_size = 0;
	uint32_t m_stride = 0;
	HeapType m_heapType = HeapType::Default;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	void* m_mappedData = nullptr;
};
