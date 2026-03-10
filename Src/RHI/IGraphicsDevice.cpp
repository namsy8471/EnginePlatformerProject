#include "IGraphicsDevice.h"
#include "Platform/DirectX12/DX12Device.h"
#include "Platform/Vulkan/VulkanDevice.h"

IGraphicsDevice* IGraphicsDevice::Create(GraphicsAPI api, void* windowHandle, int width, int height)
{
	switch (api)
	{
	case GraphicsAPI::DirectX12:
		return new DX12Device(windowHandle, width, height);
	
	case GraphicsAPI::Vulkan:
		return new VulkanDevice(windowHandle, width, height);

	}

	return nullptr;
}
