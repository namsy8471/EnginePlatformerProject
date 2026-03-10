#include "VulkanBuffer.h"

#include "VulkanDevice.h"

#include <stdexcept>

// VulkanBuffer는 VkBuffer와 그에 바인딩된 VkDeviceMemory를 함께 관리합니다.
VulkanBuffer::VulkanBuffer(VulkanDevice* device, const BufferDesc& desc)
	: m_device(device), m_size(desc.Size), m_stride(desc.Stride), m_heapType(desc.Heap)
{
	// 현재 예제는 버텍스/인덱스/업로드 전송에 모두 사용할 수 있도록 범용 usage 플래그를 켭니다.
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = desc.Size;
	bufferCreateInfo.usage =
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if (vkCreateBuffer(m_device->m_device, &bufferCreateInfo, nullptr, &m_buffer) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to create Vulkan buffer.");
	}

	VkMemoryRequirements memoryRequirements = {};
	vkGetBufferMemoryRequirements(m_device->m_device, m_buffer, &memoryRequirements);

	VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	// Upload/Readback 힙은 CPU에서 접근할 수 있어야 하므로 host-visible 메모리를 사용합니다.
	if (desc.Heap == HeapType::Upload || desc.Heap == HeapType::Readback)
	{
		memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	}

	VkMemoryAllocateInfo allocateInfo = {};
	allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocateInfo.allocationSize = memoryRequirements.size;
	allocateInfo.memoryTypeIndex = m_device->FindMemoryType(memoryRequirements.memoryTypeBits, memoryProperties);

	if (vkAllocateMemory(m_device->m_device, &allocateInfo, nullptr, &m_memory) != VK_SUCCESS)
	{
		throw std::runtime_error("Failed to allocate Vulkan buffer memory.");
	}

	vkBindBufferMemory(m_device->m_device, m_buffer, m_memory, 0);
}

VulkanBuffer::~VulkanBuffer()
{
	// 매핑된 메모리가 남아 있다면 먼저 해제합니다.
	if (m_mappedData)
	{
		vkUnmapMemory(m_device->m_device, m_memory);
		m_mappedData = nullptr;
	}

	if (m_buffer != VK_NULL_HANDLE)
	{
		vkDestroyBuffer(m_device->m_device, m_buffer, nullptr);
		m_buffer = VK_NULL_HANDLE;
	}

	if (m_memory != VK_NULL_HANDLE)
	{
		vkFreeMemory(m_device->m_device, m_memory, nullptr);
		m_memory = VK_NULL_HANDLE;
	}
}

void* VulkanBuffer::GetNativeResource() const
{
	// VkBuffer는 이후 Vulkan 전용 바인딩 코드에서 직접 사용하는 편이 안전하므로 여기서는 nullptr를 반환합니다.
	return nullptr;
}

uint64_t VulkanBuffer::GetSize() const noexcept
{
	return m_size;
}

uint32_t VulkanBuffer::GetStride() const noexcept
{
	return m_stride;
}

void VulkanBuffer::Map(void** ppData)
{
	// Device-local 메모리는 CPU가 직접 접근할 수 없으므로 Upload/Readback 힙에서만 매핑을 허용합니다.
	if (m_heapType == HeapType::Default)
	{
		throw std::runtime_error("Cannot map a Vulkan buffer allocated in device-local memory.");
	}

	if (!m_mappedData)
	{
		if (vkMapMemory(m_device->m_device, m_memory, 0, m_size, 0, &m_mappedData) != VK_SUCCESS)
		{
			throw std::runtime_error("Failed to map Vulkan buffer memory.");
		}
	}

	*ppData = m_mappedData;
}

void VulkanBuffer::Unmap()
{
	if (m_mappedData)
	{
		vkUnmapMemory(m_device->m_device, m_memory);
		m_mappedData = nullptr;
	}
}

VkBuffer VulkanBuffer::GetVkBuffer() const
{
	return m_buffer;
}
