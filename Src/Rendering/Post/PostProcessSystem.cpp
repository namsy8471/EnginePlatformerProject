#include "Rendering/Post/PostProcessSystem.h"

#include <algorithm>

namespace Rendering
{
	PostProcessStats PostProcessSystem::BuildStats(
		const PostProcessSettings& settings,
		GraphicsAPI api,
		RenderMode renderMode,
		bool hdrTargetAvailable) noexcept
	{
		const bool deferredHdr = renderMode == RenderMode::Deferred && hdrTargetAvailable;
		const std::string_view backend = deferredHdr
			? (api == GraphicsAPI::DirectX12 ? "DX12 Deferred HDR" : "Vulkan Deferred HDR")
			: "Shader Inline";

		return {
			.ToneMappingEnabled = settings.ToneMappingEnabled,
			.UsesHdrTarget = deferredHdr,
			.ToneMapPassScheduled = settings.ToneMappingEnabled && deferredHdr,
			.Exposure = std::clamp(settings.Exposure, 0.05f, 8.0f),
			.Backend = backend,
			.ToneMapper = "ACES"
		};
	}
}
