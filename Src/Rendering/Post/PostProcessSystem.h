#pragma once

#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"

#include <string_view>

namespace Rendering
{
	struct PostProcessSettings
	{
		bool ToneMappingEnabled = true;
		float Exposure = 1.0f;
	};

	struct PostProcessStats
	{
		bool ToneMappingEnabled = false;
		bool UsesHdrTarget = false;
		bool ToneMapPassScheduled = false;
		float Exposure = 1.0f;
		std::string_view Backend = "None";
		std::string_view ToneMapper = "ACES";
	};

	class PostProcessSystem
	{
	public:
		[[nodiscard]] static PostProcessStats BuildStats(
			const PostProcessSettings& settings,
			GraphicsAPI api,
			RenderMode renderMode,
			bool hdrTargetAvailable) noexcept;
	};
}
