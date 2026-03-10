#pragma once

#include "VulkanCommon.h"
#include "RHI/IGpuResource.h"

// Vulkan 백버퍼가 아직 준비되지 않았을 때 사용할 임시 리소스 래퍼입니다.
class VulkanNullResource : public IGpuResource
{
public:
	virtual ~VulkanNullResource() = default;

	void* GetNativeResource() const override
	{
		return nullptr;
	}
};

// Vulkan 스왑체인 이미지와 이미지 뷰를 함께 보관하는 간단한 리소스 래퍼입니다.
// 스왑체인 이미지는 swapchain이 소유하므로 이 클래스는 참조만 유지합니다.
class VulkanImageResource : public IGpuResource
{
public:
	VulkanImageResource(VkImage image, VkImageView imageView)
		: m_image(image), m_imageView(imageView)
	{
	}
	virtual ~VulkanImageResource() = default;

	void* GetNativeResource() const override
	{
		return nullptr;
	}

	VkImage GetImage() const
	{
		return m_image;
	}

	VkImageView GetImageView() const
	{
		return m_imageView;
	}

private:
	VkImage m_image = VK_NULL_HANDLE;
	VkImageView m_imageView = VK_NULL_HANDLE;
};
