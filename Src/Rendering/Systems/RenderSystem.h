#pragma once

#include "Scene/Scene.h"
#include "Math/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

struct alignas(16) CameraConstants
{
	DirectX::XMFLOAT4X4 WorldViewProjection = {};
	DirectX::XMFLOAT4X4 ViewProjection = {};
	DirectX::XMFLOAT4X4 World = {};
	DirectX::XMFLOAT4 CameraPosition = {};
	DirectX::XMFLOAT4 BenchmarkParams = {};
	DirectX::XMFLOAT4 LightDirection = { -0.45f, 0.75f, -0.45f, 1.0f };
	DirectX::XMFLOAT4 LightColorIntensity = { 1.0f, 0.95f, 0.82f, 2.5f };
	DirectX::XMFLOAT4 AmbientSpecular = { 0.18f, 0.35f, 32.0f, 1.0f };
};

namespace RenderSystem
{
	[[nodiscard]] inline DirectX::XMFLOAT3 NormalizeOrDefault(const DirectX::XMFLOAT3& value, const DirectX::XMFLOAT3& fallback) noexcept
	{
		DirectX::XMVECTOR vector = DirectX::XMLoadFloat3(&value);
		const float lengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(vector));
		if (!std::isfinite(lengthSquared) || lengthSquared <= 0.000001f)
		{
			return fallback;
		}

		vector = DirectX::XMVector3Normalize(vector);
		DirectX::XMFLOAT3 result = {};
		DirectX::XMStoreFloat3(&result, vector);
		return result;
	}

	[[nodiscard]] inline DirectX::XMFLOAT3 GetDirectionalLightVectorToLight(const TransformComponent* transform) noexcept
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

	inline void ApplyLightConstants(const Scene& scene, CameraConstants& cameraConstants)
	{
		const LightComponent* selectedLight = nullptr;
		const TransformComponent* selectedLightTransform = nullptr;
		for (const SceneEntity& entity : scene.GetEntities())
		{
			const LightComponent* light = scene.GetLightComponent(entity.Id);
			if (!light || !scene.IsLightEnabled(entity.Id) || !light->Enabled)
			{
				continue;
			}

			selectedLight = light;
			selectedLightTransform = scene.GetTransformComponent(entity.Id);
			break;
		}

		if (!selectedLight)
		{
			const DirectX::XMFLOAT3 fallbackDirection = NormalizeOrDefault({ -0.45f, 0.75f, -0.45f }, { 0.0f, 1.0f, 0.0f });
			cameraConstants.LightDirection = { fallbackDirection.x, fallbackDirection.y, fallbackDirection.z, 1.0f };
			cameraConstants.LightColorIntensity = { 1.0f, 0.95f, 0.82f, 1.75f };
			cameraConstants.AmbientSpecular = { 0.18f, 0.35f, 32.0f, 1.0f };
			return;
		}

		const DirectX::XMFLOAT3 directionToLight = GetDirectionalLightVectorToLight(selectedLightTransform);
		cameraConstants.LightDirection = { directionToLight.x, directionToLight.y, directionToLight.z, 1.0f };
		cameraConstants.LightColorIntensity = {
			std::clamp(selectedLight->Color.x, 0.0f, 10.0f),
			std::clamp(selectedLight->Color.y, 0.0f, 10.0f),
			std::clamp(selectedLight->Color.z, 0.0f, 10.0f),
			(std::max)(selectedLight->Intensity, 0.0f)
		};
		cameraConstants.AmbientSpecular = { 0.18f, 0.35f, 32.0f, 1.0f };
	}

	[[nodiscard]] inline Asset::StaticMeshAsset* GetRenderableMesh(Scene& scene, EntityId entityId)
	{
		if (!scene.IsMeshEnabled(entityId))
		{
			return nullptr;
		}
		return scene.GetMeshAsset(entityId);
	}

	[[nodiscard]] inline const Asset::StaticMeshAsset* GetRenderableMesh(const Scene& scene, EntityId entityId)
	{
		if (!scene.IsMeshEnabled(entityId))
		{
			return nullptr;
		}
		return scene.GetMeshAsset(entityId);
	}

	[[nodiscard]] inline EntityId GetPrimaryRenderableEntity(const Scene& scene) noexcept
	{
		return scene.GetPrimaryRenderableEntity();
	}

	[[nodiscard]] inline Asset::StaticMeshAsset* GetPrimaryRenderableMesh(Scene& scene)
	{
		return GetRenderableMesh(scene, scene.GetPrimaryRenderableEntity());
	}

	[[nodiscard]] inline const Asset::StaticMeshAsset* GetPrimaryRenderableMesh(const Scene& scene)
	{
		return GetRenderableMesh(scene, scene.GetPrimaryRenderableEntity());
	}

	[[nodiscard]] inline TransformComponent* GetPrimaryRenderableTransform(Scene& scene)
	{
		return scene.GetTransformComponent(scene.GetPrimaryRenderableEntity());
	}

	[[nodiscard]] inline const TransformComponent* GetPrimaryRenderableTransform(const Scene& scene)
	{
		return scene.GetTransformComponent(scene.GetPrimaryRenderableEntity());
	}

	[[nodiscard]] inline bool BuildCameraConstants(const Scene& scene, const Camera& camera, EntityId entityId, CameraConstants& cameraConstants)
	{
		const TransformComponent* transform = scene.GetTransformComponent(entityId);
		if (!transform)
		{
			return false;
		}

		Math::Store(cameraConstants.WorldViewProjection, transform->GetWorldXmMatrix() * camera.GetViewProjectionMatrix());
		DirectX::XMStoreFloat4x4(&cameraConstants.ViewProjection, camera.GetViewProjectionMatrix());
		Math::Store(cameraConstants.World, transform->GetWorldXmMatrix());
		const auto position = camera.GetPosition();
		cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
		ApplyLightConstants(scene, cameraConstants);
		return true;
	}

	[[nodiscard]] inline bool BuildCameraConstants(const Scene& scene, const Camera& camera, CameraConstants& cameraConstants)
	{
		return BuildCameraConstants(scene, camera, scene.GetPrimaryRenderableEntity(), cameraConstants);
	}
}
