#include "Rendering/Lighting/ShadowSystem.h"

#include "Math/MathHelpers.h"

#include <algorithm>
#include <cmath>

namespace Rendering
{
	namespace
	{
		[[nodiscard]] DirectX::XMFLOAT3 NormalizeOrDefault(const DirectX::XMFLOAT3& value, const DirectX::XMFLOAT3& fallback) noexcept
		{
			const DirectX::XMVECTOR vector = DirectX::XMLoadFloat3(&value);
			const float lengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(vector));
			if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
			{
				return fallback;
			}

			DirectX::XMFLOAT3 result = {};
			DirectX::XMStoreFloat3(&result, DirectX::XMVector3Normalize(vector));
			return result;
		}

		[[nodiscard]] DirectX::XMFLOAT3 GetDirectionalLightVectorToLight(const TransformComponent* transform) noexcept
		{
			if (!transform)
			{
				return NormalizeOrDefault({ -0.45f, 0.75f, -0.45f }, { 0.0f, 1.0f, 0.0f });
			}

			const DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&transform->WorldTransform.Rotation);
			const DirectX::XMVECTOR forward = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation);
			DirectX::XMFLOAT3 direction = {};
			DirectX::XMStoreFloat3(&direction, DirectX::XMVectorNegate(forward));
			return NormalizeOrDefault(direction, { 0.0f, 1.0f, 0.0f });
		}

		[[nodiscard]] bool IsShadowCastingDirectionalLight(const Scene& scene, EntityId entityId) noexcept
		{
			const LightComponent* light = scene.GetLightComponent(entityId);
			return light
				&& scene.IsLightEnabled(entityId)
				&& light->Enabled
				&& light->CastShadows
				&& light->Type == LightType::Directional;
		}

		[[nodiscard]] EntityId ResolveShadowLight(const Scene& scene, EntityId keyLightEntity) noexcept
		{
			if (IsShadowCastingDirectionalLight(scene, keyLightEntity))
			{
				return keyLightEntity;
			}

			for (const SceneEntity& entity : scene.GetEntities())
			{
				if (IsShadowCastingDirectionalLight(scene, entity.Id))
				{
					return entity.Id;
				}
			}
			return InvalidEntityId;
		}
	}

	ShadowFrameData ShadowSystem::BuildDirectionalShadowFrameData(
		const Scene& scene,
		EntityId keyLightEntity,
		const Camera& camera,
		const ShadowSettings& settings)
	{
		ShadowFrameData frameData;
		frameData.MapSize = (std::max)(settings.MapSize, 256u);
		if (!settings.Enabled)
		{
			return frameData;
		}

		const EntityId lightEntity = ResolveShadowLight(scene, keyLightEntity);
		if (lightEntity == InvalidEntityId)
		{
			return frameData;
		}

		const LightComponent* light = scene.GetLightComponent(lightEntity);
		const TransformComponent* lightTransform = scene.GetTransformComponent(lightEntity);
		const DirectX::XMFLOAT3 directionToLight = GetDirectionalLightVectorToLight(lightTransform);
		const float distance = std::clamp(settings.Distance, 1.0f, 10000.0f);
		const float orthoSize = std::clamp(settings.OrthographicSize, 1.0f, 10000.0f);

		const DirectX::XMFLOAT3 cameraPositionValue = camera.GetPosition();
		const DirectX::XMFLOAT3 cameraForwardValue = camera.GetForward();
		const DirectX::XMVECTOR cameraPosition = DirectX::XMLoadFloat3(&cameraPositionValue);
		const DirectX::XMVECTOR cameraForward = DirectX::XMLoadFloat3(&cameraForwardValue);
		const DirectX::XMVECTOR sceneCenter = DirectX::XMVectorAdd(cameraPosition, DirectX::XMVectorScale(cameraForward, distance * 0.5f));
		const DirectX::XMVECTOR lightDirection = DirectX::XMLoadFloat3(&directionToLight);
		const DirectX::XMVECTOR lightPosition = DirectX::XMVectorAdd(sceneCenter, DirectX::XMVectorScale(lightDirection, distance));

		const float upDot = std::abs(DirectX::XMVectorGetX(DirectX::XMVector3Dot(lightDirection, DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f))));
		const DirectX::XMVECTOR up = upDot > 0.95f
			? DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
			: DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
		const DirectX::XMMATRIX lightView = DirectX::XMMatrixLookAtLH(lightPosition, sceneCenter, up);
		const DirectX::XMMATRIX lightProjection = DirectX::XMMatrixOrthographicLH(orthoSize, orthoSize, 0.1f, distance * 2.0f);

		Math::Store(frameData.LightViewProjection, lightView * lightProjection);
		frameData.Enabled = true;
		frameData.HasDirectionalCaster = true;
		frameData.LightEntity = lightEntity;
		frameData.Params = {
			1.0f,
			std::clamp(light ? light->ShadowBias : 0.0015f, 0.0f, 0.1f),
			std::clamp(light ? light->ShadowNormalBias : 0.02f, 0.0f, 1.0f),
			std::clamp(light ? light->ShadowStrength : 0.75f, 0.0f, 1.0f)
		};
		frameData.DirectionToLight = { directionToLight.x, directionToLight.y, directionToLight.z, 0.0f };
		return frameData;
	}

	ShadowStats ShadowSystem::BuildStats(const ShadowFrameData& frameData, const ShadowSettings& settings)
	{
		return ShadowStats{
			.Enabled = frameData.Enabled,
			.HasDirectionalCaster = frameData.HasDirectionalCaster,
			.LightEntity = frameData.LightEntity,
			.MapSize = frameData.MapSize,
			.Distance = settings.Distance,
			.OrthographicSize = settings.OrthographicSize,
			.Bias = frameData.Params.y,
			.NormalBias = frameData.Params.z,
			.Strength = frameData.Params.w
		};
	}
}
