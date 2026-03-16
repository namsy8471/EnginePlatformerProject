#pragma once

#include "Scene.h"
#include "Math/Camera.h"

#include <DirectXMath.h>

struct alignas(16) CameraConstants
{
	DirectX::XMFLOAT4X4 WorldViewProjection = {};
	DirectX::XMFLOAT4X4 ViewProjection = {};
	DirectX::XMFLOAT4 CameraPosition = {};
};

namespace RenderSystem
{
	[[nodiscard]] inline Asset::StaticMeshAsset* GetRenderableMesh(Scene& scene, EntityId entityId)
	{
		return scene.GetMeshAsset(entityId);
	}

	[[nodiscard]] inline const Asset::StaticMeshAsset* GetRenderableMesh(const Scene& scene, EntityId entityId)
	{
		return scene.GetMeshAsset(entityId);
	}

	[[nodiscard]] inline EntityId GetPrimaryRenderableEntity(const Scene& scene) noexcept
	{
		return scene.GetPrimaryRenderableEntity();
	}

	[[nodiscard]] inline Asset::StaticMeshAsset* GetPrimaryRenderableMesh(Scene& scene)
	{
		return scene.GetMeshAsset(scene.GetPrimaryRenderableEntity());
	}

	[[nodiscard]] inline const Asset::StaticMeshAsset* GetPrimaryRenderableMesh(const Scene& scene)
	{
		return scene.GetMeshAsset(scene.GetPrimaryRenderableEntity());
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
		const auto position = camera.GetPosition();
		cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
		return true;
	}

	[[nodiscard]] inline bool BuildCameraConstants(const Scene& scene, const Camera& camera, CameraConstants& cameraConstants)
	{
		return BuildCameraConstants(scene, camera, scene.GetPrimaryRenderableEntity(), cameraConstants);
	}
}
