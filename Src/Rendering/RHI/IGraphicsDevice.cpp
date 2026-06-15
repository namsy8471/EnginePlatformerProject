#include "IGraphicsDevice.h"
#include "Rendering/Backends/DirectX12/DX12Device.h"
#include "Rendering/Backends/Vulkan/VulkanDevice.h"

std::unique_ptr<IGraphicsDevice> IGraphicsDevice::Create(GraphicsAPI api, void* windowHandle, int width, int height)
{
	switch (api)
	{
	case GraphicsAPI::DirectX12:
		return std::make_unique<DX12Device>(windowHandle, width, height);
	
	case GraphicsAPI::Vulkan:
		return std::make_unique<VulkanDevice>(windowHandle, width, height);

	}

	return {};
}
