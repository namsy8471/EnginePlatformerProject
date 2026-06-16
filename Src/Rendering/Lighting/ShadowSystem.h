#pragma once

#include "Math/Camera.h"
#include "Scene/Scene.h"

#include <DirectXMath.h>

#include <cstdint>

namespace Rendering
{
	struct ShadowSettings
	{
		bool Enabled = true;
		uint32_t MapSize = 2048;
		float Distance = 80.0f;
		float OrthographicSize = 80.0f;
	};

	struct ShadowFrameData
	{
		bool Enabled = false;
		bool HasDirectionalCaster = false;
		EntityId LightEntity = InvalidEntityId;
		DirectX::XMFLOAT4X4 LightViewProjection = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		DirectX::XMFLOAT4 Params = { 0.0f, 0.0015f, 0.02f, 0.75f };
		DirectX::XMFLOAT4 DirectionToLight = { -0.45f, 0.75f, -0.45f, 0.0f };
		uint32_t MapSize = 2048;
	};

	struct ShadowStats
	{
		bool Enabled = false;
		bool HasDirectionalCaster = false;
		EntityId LightEntity = InvalidEntityId;
		uint32_t MapSize = 0;
		float Distance = 0.0f;
		float OrthographicSize = 0.0f;
		float Bias = 0.0f;
		float NormalBias = 0.0f;
		float Strength = 0.0f;
	};

	class ShadowSystem
	{
	public:
		[[nodiscard]] static ShadowFrameData BuildDirectionalShadowFrameData(
			const Scene& scene,
			EntityId keyLightEntity,
			const Camera& camera,
			const ShadowSettings& settings);

		[[nodiscard]] static ShadowStats BuildStats(const ShadowFrameData& frameData, const ShadowSettings& settings);
	};
}
