#pragma once

#include "Scene/Scene.h"

#include <functional>
#include <string_view>

struct SceneRenderState;

namespace Rendering
{
	class MaterialTextureSystem
	{
	public:
		using LogCallback = std::function<void(std::string_view)>;

		[[nodiscard]] static bool LoadCpuMaterialTextures(
			Scene& scene,
			SceneRenderState& renderState,
			EntityId entityId,
			const LogCallback& logCallback);

		[[nodiscard]] static bool HasTransparency(const CpuMaterialTexture& materialTexture);
	};
}
