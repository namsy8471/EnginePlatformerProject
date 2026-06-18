#pragma once

#include "Math/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace Rendering
{
	struct SkyboxSettings
	{
		bool Enabled = true;
		DirectX::XMFLOAT3 ZenithColor = { 0.32f, 0.55f, 0.95f };
		DirectX::XMFLOAT3 HorizonColor = { 0.78f, 0.88f, 1.0f };
		DirectX::XMFLOAT3 GroundColor = { 0.34f, 0.39f, 0.46f };
		DirectX::XMFLOAT3 SunColor = { 1.0f, 0.86f, 0.58f };
		DirectX::XMFLOAT3 SunDirection = { -0.35f, 0.78f, -0.42f };
		float Intensity = 1.0f;
		float HorizonHeight = -0.04f;
		float HorizonBlend = 1.35f;
		float SunSize = 0.035f;
		float SunIntensity = 1.15f;
	};

	struct alignas(16) SkyboxGpuConstants
	{
		DirectX::XMFLOAT4 CameraRightTanX = {};
		DirectX::XMFLOAT4 CameraUpTanY = {};
		DirectX::XMFLOAT4 CameraForwardEnabled = {};
		DirectX::XMFLOAT4 ZenithColorIntensity = {};
		DirectX::XMFLOAT4 HorizonColorBlend = {};
		DirectX::XMFLOAT4 GroundColorHorizon = {};
		DirectX::XMFLOAT4 SunDirectionSize = {};
		DirectX::XMFLOAT4 SunColorIntensity = {};
	};

	[[nodiscard]] inline DirectX::XMFLOAT3 ClampSkyboxColor(const DirectX::XMFLOAT3& color) noexcept
	{
		return {
			std::clamp(color.x, 0.0f, 8.0f),
			std::clamp(color.y, 0.0f, 8.0f),
			std::clamp(color.z, 0.0f, 8.0f)
		};
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 NormalizeSkyboxDirection(const DirectX::XMFLOAT3& value) noexcept
	{
		DirectX::XMVECTOR direction = DirectX::XMLoadFloat3(&value);
		const float lengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(direction));
		if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
		{
			return { -0.35f, 0.78f, -0.42f };
		}

		direction = DirectX::XMVector3Normalize(direction);
		DirectX::XMFLOAT3 result = {};
		DirectX::XMStoreFloat3(&result, direction);
		return result;
	}

	[[nodiscard]] inline SkyboxSettings ClampSkyboxSettings(SkyboxSettings settings) noexcept
	{
		settings.ZenithColor = ClampSkyboxColor(settings.ZenithColor);
		settings.HorizonColor = ClampSkyboxColor(settings.HorizonColor);
		settings.GroundColor = ClampSkyboxColor(settings.GroundColor);
		settings.SunColor = ClampSkyboxColor(settings.SunColor);
		settings.SunDirection = NormalizeSkyboxDirection(settings.SunDirection);
		settings.Intensity = std::clamp(settings.Intensity, 0.0f, 8.0f);
		settings.HorizonHeight = std::clamp(settings.HorizonHeight, -0.95f, 0.95f);
		settings.HorizonBlend = std::clamp(settings.HorizonBlend, 0.05f, 8.0f);
		settings.SunSize = std::clamp(settings.SunSize, 0.001f, 0.35f);
		settings.SunIntensity = std::clamp(settings.SunIntensity, 0.0f, 16.0f);
		return settings;
	}

	[[nodiscard]] inline SkyboxGpuConstants BuildSkyboxGpuConstants(const SkyboxSettings& sourceSettings, const Camera& camera) noexcept
	{
		const SkyboxSettings settings = ClampSkyboxSettings(sourceSettings);
		const DirectX::XMFLOAT3 right = camera.GetRight();
		const DirectX::XMFLOAT3 up = camera.GetUp();
		const DirectX::XMFLOAT3 forward = camera.GetForward();
		const float tanY = std::tan(camera.GetFovY() * 0.5f);
		const float tanX = tanY * camera.GetAspect();

		return {
			.CameraRightTanX = { right.x, right.y, right.z, tanX },
			.CameraUpTanY = { up.x, up.y, up.z, tanY },
			.CameraForwardEnabled = { forward.x, forward.y, forward.z, settings.Enabled ? 1.0f : 0.0f },
			.ZenithColorIntensity = { settings.ZenithColor.x, settings.ZenithColor.y, settings.ZenithColor.z, settings.Intensity },
			.HorizonColorBlend = { settings.HorizonColor.x, settings.HorizonColor.y, settings.HorizonColor.z, settings.HorizonBlend },
			.GroundColorHorizon = { settings.GroundColor.x, settings.GroundColor.y, settings.GroundColor.z, settings.HorizonHeight },
			.SunDirectionSize = { settings.SunDirection.x, settings.SunDirection.y, settings.SunDirection.z, settings.SunSize },
			.SunColorIntensity = { settings.SunColor.x, settings.SunColor.y, settings.SunColor.z, settings.SunIntensity }
		};
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 EstimateSkyboxClearColor(const SkyboxSettings& sourceSettings) noexcept
	{
		const SkyboxSettings settings = ClampSkyboxSettings(sourceSettings);
		return {
			(settings.HorizonColor.x * 0.62f + settings.ZenithColor.x * 0.38f) * settings.Intensity,
			(settings.HorizonColor.y * 0.62f + settings.ZenithColor.y * 0.38f) * settings.Intensity,
			(settings.HorizonColor.z * 0.62f + settings.ZenithColor.z * 0.38f) * settings.Intensity
		};
	}
}
