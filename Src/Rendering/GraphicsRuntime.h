#pragma once

#include "Rendering/RHI/GraphicsCommon.h"

class ICommandList;
class IGraphicsDevice;

namespace Rendering
{
	struct GraphicsRuntime
	{
		IGraphicsDevice* Device = nullptr;
		ICommandList* CommandList = nullptr;
		GraphicsAPI CurrentApi = GraphicsAPI::Vulkan;
	};
}
