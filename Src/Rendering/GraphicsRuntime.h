#pragma once

#include "Rendering/RHI/GraphicsCommon.h"

#include <memory>

class ICommandList;
class IGraphicsDevice;

namespace Rendering
{
	struct GraphicsRuntime
	{
		std::unique_ptr<IGraphicsDevice> Device;
		std::unique_ptr<ICommandList> CommandList;
		GraphicsAPI CurrentApi = GraphicsAPI::Vulkan;
	};
}
