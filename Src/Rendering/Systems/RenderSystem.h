#pragma once

#include "Rendering/Lighting/ShadowSystem.h"
#include "Scene/Scene.h"
#include "Math/Camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Forward and Forward+ keep a fixed uniform array for predictable shader cost.
// Deferred lighting uses a growable light buffer; zero means no scene-side cap.
inline constexpr uint32_t kMaxForwardGpuLights = 8;
inline constexpr size_t kUnlimitedDeferredGpuLights = 0;
inline constexpr uint32_t kInitialDeferredLightBufferCapacity = 64;
inline constexpr uint32_t kDeferredLightTileSize = 32;

struct alignas(16) LightGpuData
{
	DirectX::XMFLOAT4 PositionType = {};
	DirectX::XMFLOAT4 DirectionRange = {};
	DirectX::XMFLOAT4 ColorIntensity = {};
	DirectX::XMFLOAT4 SpotAnglesEnabled = {};
};

struct alignas(16) DeferredTileLightRange
{
	uint32_t Offset = 0;
	uint32_t Count = 0;
	uint32_t Padding0 = 0;
	uint32_t Padding1 = 0;
};

struct alignas(16) CameraConstants
{
	DirectX::XMFLOAT4X4 WorldViewProjection = {};
	DirectX::XMFLOAT4X4 ViewProjection = {};
	DirectX::XMFLOAT4X4 World = {};
	DirectX::XMFLOAT4X4 WorldInverseTranspose = {};
	DirectX::XMFLOAT4 CameraPosition = {};
	DirectX::XMFLOAT4 BenchmarkParams = {};
	DirectX::XMFLOAT4 LightDirection = { -0.45f, 0.75f, -0.45f, 1.0f };
	DirectX::XMFLOAT4 LightColorIntensity = { 1.0f, 0.95f, 0.82f, 3.25f };
	DirectX::XMFLOAT4 AmbientSpecular = { 0.35f, 0.35f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 MaterialBaseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 MaterialSpecularShininess = { 1.0f, 1.0f, 1.0f, 32.0f };
	DirectX::XMFLOAT4 MaterialEmissiveMetallic = { 0.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 MaterialRoughnessFlags = { 0.5f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 MaterialTextureFlags = { 0.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 MaterialTextureFlags2 = { 0.0f, 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT4 AmbientColorIntensity = { 0.62f, 0.68f, 0.78f, 0.35f };
	DirectX::XMFLOAT4 ExposureDebug = { 1.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 LightCountParams = { 1.0f, 0.0f, 0.0f, 0.0f };
	std::array<DirectX::XMFLOAT4, kMaxForwardGpuLights> LightPositionType = {};
	std::array<DirectX::XMFLOAT4, kMaxForwardGpuLights> LightDirectionRange = {};
	std::array<DirectX::XMFLOAT4, kMaxForwardGpuLights> LightColorIntensityData = {};
	std::array<DirectX::XMFLOAT4, kMaxForwardGpuLights> LightSpotAnglesEnabled = {};
	DirectX::XMFLOAT4X4 ShadowViewProjection = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT4 ShadowParams = { 0.0f, 0.0015f, 0.02f, 0.75f };
	DirectX::XMFLOAT4 ShadowDirection = { -0.45f, 0.75f, -0.45f, 0.0f };
};

struct alignas(16) DeferredLightingConstants
{
	DirectX::XMFLOAT4 CameraPosition = {};
	DirectX::XMFLOAT4 AmbientColorIntensity = { 0.62f, 0.68f, 0.78f, 0.35f };
	DirectX::XMFLOAT4 ExposureDebug = { 1.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 LightCountParams = {};
	DirectX::XMFLOAT4 ScreenSize = {};
	DirectX::XMFLOAT4X4 ShadowViewProjection = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT4 ShadowParams = { 0.0f, 0.0015f, 0.02f, 0.75f };
	DirectX::XMFLOAT4 ShadowDirection = { -0.45f, 0.75f, -0.45f, 0.0f };
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

	[[nodiscard]] inline DirectX::XMFLOAT3 GetLightForwardDirection(const TransformComponent* transform) noexcept
	{
		if (!transform)
		{
			return NormalizeOrDefault({ 0.45f, -0.75f, 0.45f }, { 0.0f, -1.0f, 0.0f });
		}

		const DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&transform->WorldTransform.Rotation);
		const DirectX::XMVECTOR forward = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation);
		DirectX::XMFLOAT3 direction = {};
		DirectX::XMStoreFloat3(&direction, forward);
		return NormalizeOrDefault(direction, { 0.0f, -1.0f, 0.0f });
	}

	[[nodiscard]] inline float LightTypeToGpuValue(LightType type) noexcept
	{
		switch (type)
		{
		case LightType::Point:
			return 1.0f;
		case LightType::Spot:
			return 2.0f;
		case LightType::Directional:
		default:
			return 0.0f;
		}
	}

	[[nodiscard]] inline LightGpuData BuildLightGpuData(
		const LightComponent& light,
		const TransformComponent* transform)
	{
		DirectX::XMFLOAT3 position = {};
		if (transform)
		{
			position = transform->WorldTransform.Translation;
		}

		const DirectX::XMFLOAT3 directionToLight = GetDirectionalLightVectorToLight(transform);
		const DirectX::XMFLOAT3 lightForward = GetLightForwardDirection(transform);
		const float type = LightTypeToGpuValue(light.Type);
		const float range = (std::max)(light.Range, 0.001f);
		const float halfSpotAngle = std::clamp(light.SpotAngle * 0.5f, 0.001f, DirectX::XM_PIDIV2 - 0.001f);
		const float outerCos = std::cos(halfSpotAngle);
		const float innerCos = std::cos(halfSpotAngle * 0.72f);

		return {
			.PositionType = { position.x, position.y, position.z, type },
			.DirectionRange = {
				light.Type == LightType::Directional ? directionToLight.x : lightForward.x,
				light.Type == LightType::Directional ? directionToLight.y : lightForward.y,
				light.Type == LightType::Directional ? directionToLight.z : lightForward.z,
				range
			},
			.ColorIntensity = {
				std::clamp(light.Color.x, 0.0f, 10.0f),
				std::clamp(light.Color.y, 0.0f, 10.0f),
				std::clamp(light.Color.z, 0.0f, 10.0f),
				(std::max)(light.Intensity, 0.0f)
			},
			.SpotAnglesEnabled = { innerCos, outerCos, light.Enabled ? 1.0f : 0.0f, 0.0f }
		};
	}

	inline void WriteLightConstants(
		CameraConstants& cameraConstants,
		uint32_t lightIndex,
		const LightGpuData& light)
	{
		if (lightIndex >= kMaxForwardGpuLights)
		{
			return;
		}

		cameraConstants.LightPositionType[lightIndex] = light.PositionType;
		cameraConstants.LightDirectionRange[lightIndex] = light.DirectionRange;
		cameraConstants.LightColorIntensityData[lightIndex] = light.ColorIntensity;
		cameraConstants.LightSpotAnglesEnabled[lightIndex] = light.SpotAnglesEnabled;
	}

	[[nodiscard]] inline bool IsRenderableLight(const Scene& scene, EntityId entityId) noexcept
	{
		const LightComponent* light = scene.GetLightComponent(entityId);
		return light && scene.IsLightEnabled(entityId) && light->Enabled;
	}

	[[nodiscard]] inline uint32_t CountSceneRenderableLights(const Scene& scene) noexcept
	{
		uint32_t lightCount = 0;
		for (const SceneEntity& entity : scene.GetEntities())
		{
			if (IsRenderableLight(scene, entity.Id))
			{
				++lightCount;
			}
		}
		return lightCount;
	}

	[[nodiscard]] inline std::vector<LightGpuData> CollectSceneLights(
		const Scene& scene,
		EntityId keyLightEntity,
		size_t maxLightCount)
	{
		std::vector<LightGpuData> lights;
		const size_t entityCount = scene.GetEntities().size();
		lights.reserve(maxLightCount > 0 ? (std::min)(maxLightCount, entityCount) : entityCount);

		auto tryAppendLight = [&](EntityId entityId)
		{
			if (entityId == InvalidEntityId || (maxLightCount > 0 && lights.size() >= maxLightCount))
			{
				return;
			}

			const LightComponent* light = scene.GetLightComponent(entityId);
			if (!light || !scene.IsLightEnabled(entityId) || !light->Enabled)
			{
				return;
			}

			lights.push_back(BuildLightGpuData(*light, scene.GetTransformComponent(entityId)));
		};

		tryAppendLight(keyLightEntity);
		for (const SceneEntity& entity : scene.GetEntities())
		{
			if (entity.Id == keyLightEntity)
			{
				continue;
			}
			tryAppendLight(entity.Id);
		}

		if (lights.empty())
		{
			LightComponent fallbackLight;
			fallbackLight.Type = LightType::Directional;
			fallbackLight.Color = { 1.0f, 0.95f, 0.82f };
			fallbackLight.Intensity = 2.75f;
			lights.push_back(BuildLightGpuData(fallbackLight, nullptr));
		}

		return lights;
	}

	inline void ApplyLightConstants(
		const Scene& scene,
		CameraConstants& cameraConstants,
		const DirectX::XMFLOAT3& ambientColor,
		float ambientIntensity,
		float exposure,
		MaterialDebugView debugView,
		EntityId keyLightEntity,
		bool useDeferredLightList,
		uint32_t deferredLightCount)
	{
		const float clampedAmbientIntensity = std::clamp(ambientIntensity, 0.0f, 2.0f);
		const float clampedExposure = std::clamp(exposure, 0.05f, 8.0f);
		cameraConstants.AmbientColorIntensity = {
			std::clamp(ambientColor.x, 0.0f, 4.0f),
			std::clamp(ambientColor.y, 0.0f, 4.0f),
			std::clamp(ambientColor.z, 0.0f, 4.0f),
			clampedAmbientIntensity
		};
		cameraConstants.ExposureDebug = {
			clampedExposure,
			static_cast<float>(static_cast<uint32_t>(debugView)),
			0.0f,
			0.0f
		};

		const std::vector<LightGpuData> forwardLights = CollectSceneLights(scene, keyLightEntity, kMaxForwardGpuLights);
		const uint32_t forwardLightCount = static_cast<uint32_t>((std::min)(forwardLights.size(), static_cast<size_t>(kMaxForwardGpuLights)));
		for (uint32_t lightIndex = 0; lightIndex < forwardLightCount; ++lightIndex)
		{
			WriteLightConstants(cameraConstants, lightIndex, forwardLights[lightIndex]);
		}

		const DirectX::XMFLOAT4& firstDirection = cameraConstants.LightDirectionRange[0];
		const DirectX::XMFLOAT4& firstColor = cameraConstants.LightColorIntensityData[0];
		cameraConstants.LightDirection = { firstDirection.x, firstDirection.y, firstDirection.z, 1.0f };
		cameraConstants.LightColorIntensity = firstColor;
		cameraConstants.AmbientSpecular = { clampedAmbientIntensity, 0.35f, clampedExposure, 1.0f };
		cameraConstants.LightCountParams = {
			static_cast<float>(forwardLightCount),
			0.0f,
			static_cast<float>(deferredLightCount),
			useDeferredLightList ? 1.0f : 0.0f
		};
	}

	[[nodiscard]] inline float TextureSourceFlag(const Asset::StaticMeshMaterial& material, Asset::MaterialTextureSlot slot) noexcept
	{
		return Asset::GetMaterialTextureBinding(material, slot).HasSource() || !Asset::GetMaterialTexturePath(material, slot).empty()
			? 1.0f
			: 0.0f;
	}

	inline void ApplyMaterialConstants(const Scene& scene, EntityId entityId, size_t materialIndex, CameraConstants& cameraConstants)
	{
		const Asset::StaticMeshAsset* mesh = scene.GetMeshAsset(entityId);
		if (!mesh || mesh->Materials.empty() || materialIndex >= mesh->Materials.size())
		{
			return;
		}

		const Asset::StaticMeshMaterial& material = mesh->Materials[materialIndex];
		cameraConstants.MaterialBaseColor = material.DiffuseColor;
		cameraConstants.MaterialSpecularShininess = {
			std::clamp(material.SpecularColor.x, 0.0f, 16.0f),
			std::clamp(material.SpecularColor.y, 0.0f, 16.0f),
			std::clamp(material.SpecularColor.z, 0.0f, 16.0f),
			std::clamp(material.Shininess, 1.0f, 1024.0f)
		};
		cameraConstants.MaterialEmissiveMetallic = {
			std::clamp(material.EmissiveColor.x, 0.0f, 16.0f),
			std::clamp(material.EmissiveColor.y, 0.0f, 16.0f),
			std::clamp(material.EmissiveColor.z, 0.0f, 16.0f),
			std::clamp(material.MetallicFactor, 0.0f, 1.0f)
		};
		cameraConstants.MaterialRoughnessFlags = {
			std::clamp(material.RoughnessFactor, 0.02f, 1.0f),
			static_cast<float>(material.ShadingModel == Asset::MaterialShadingModel::PBR ? 1 : material.ShadingModel == Asset::MaterialShadingModel::Unlit ? 2 : 0),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::MetallicRoughness),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Metallic)
		};
		cameraConstants.MaterialTextureFlags = {
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Roughness),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Normal),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::AO),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Emissive)
		};
		cameraConstants.MaterialTextureFlags2 = {
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Opacity),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Specular),
			TextureSourceFlag(material, Asset::MaterialTextureSlot::Shininess),
			material.UseVertexColor ? 1.0f : 0.0f
		};
		cameraConstants.ExposureDebug.z = material.NormalYFlip ? 1.0f : 0.0f;
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

	[[nodiscard]] inline bool BuildCameraConstants(
		const Scene& scene,
		const Camera& camera,
		EntityId entityId,
		size_t materialIndex,
		CameraConstants& cameraConstants,
		DirectX::XMFLOAT3 ambientColor = { 0.62f, 0.68f, 0.78f },
		float ambientIntensity = 0.35f,
		float exposure = 1.0f,
		MaterialDebugView debugView = MaterialDebugView::Lit,
		EntityId keyLightEntity = InvalidEntityId,
		bool useDeferredLightList = false,
		uint32_t deferredLightCount = 0,
		const Rendering::ShadowSettings& shadowSettings = {})
	{
		const TransformComponent* transform = scene.GetTransformComponent(entityId);
		if (!transform)
		{
			return false;
		}

		const DirectX::XMMATRIX worldMatrix = transform->GetWorldXmMatrix();
		Math::Store(cameraConstants.WorldViewProjection, worldMatrix * camera.GetViewProjectionMatrix());
		DirectX::XMStoreFloat4x4(&cameraConstants.ViewProjection, camera.GetViewProjectionMatrix());
		Math::Store(cameraConstants.World, worldMatrix);
		Math::Store(cameraConstants.WorldInverseTranspose, DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, worldMatrix)));
		const auto position = camera.GetPosition();
		cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
		ApplyLightConstants(scene, cameraConstants, ambientColor, ambientIntensity, exposure, debugView, keyLightEntity, useDeferredLightList, deferredLightCount);
		ApplyMaterialConstants(scene, entityId, materialIndex, cameraConstants);
		const Rendering::ShadowFrameData shadowData = Rendering::ShadowSystem::BuildDirectionalShadowFrameData(scene, keyLightEntity, camera, shadowSettings);
		cameraConstants.ShadowViewProjection = shadowData.LightViewProjection;
		cameraConstants.ShadowParams = shadowData.Params;
		cameraConstants.ShadowDirection = shadowData.DirectionToLight;
		return true;
	}

	[[nodiscard]] inline bool BuildCameraConstants(const Scene& scene, const Camera& camera, EntityId entityId, CameraConstants& cameraConstants)
	{
		return BuildCameraConstants(scene, camera, entityId, 0, cameraConstants);
	}

	[[nodiscard]] inline bool BuildCameraConstants(const Scene& scene, const Camera& camera, CameraConstants& cameraConstants)
	{
		return BuildCameraConstants(scene, camera, scene.GetPrimaryRenderableEntity(), cameraConstants);
	}
}
