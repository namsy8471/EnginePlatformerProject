#pragma once

#include "Assets/StaticMesh.h"
#include "Scene/TransformComponent.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct CpuMaterialTexture
{
	std::filesystem::path Path;
	std::vector<unsigned char> Pixels = { 255, 255, 255, 255 };
	int Width = 1;
	int Height = 1;
};

using EntityId = uint32_t;
constexpr EntityId InvalidEntityId = 0;

struct SceneEntity
{
	EntityId Id = InvalidEntityId;
	std::string Name;
};

struct MeshComponent
{
	std::unique_ptr<Asset::StaticMeshAsset> Asset;
	std::vector<CpuMaterialTexture> MaterialTextures;
};

struct BoundsComponent
{
	DirectX::XMFLOAT3 LocalMin = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT3 LocalMax = { 0.0f, 0.0f, 0.0f };
};

struct CameraComponent
{
	float FovY = DirectX::XM_PIDIV4;
	float NearZ = 0.1f;
	float FarZ = 1000.0f;
	bool IsGameCamera = false;
};

enum class LightType : uint8_t
{
	Directional,
	Point,
	Spot
};

struct LightComponent
{
	LightType Type = LightType::Directional;
	DirectX::XMFLOAT3 Color = { 1.0f, 0.96f, 0.86f };
	float Intensity = 2.5f;
	float Range = 200.0f;
	float SpotAngle = DirectX::XM_PIDIV4;
	bool Enabled = true;
};

class Scene
{
public:
	[[nodiscard]] EntityId CreateEntity(std::string_view name)
	{
		const EntityId entityId = m_NextEntityId++;
		m_Entities.push_back({ entityId, std::string(name) });
		return entityId;
	}

	[[nodiscard]] TransformComponent* GetTransformComponent(EntityId entityId)
	{
		auto transformIt = m_Transforms.find(entityId);
		return transformIt != m_Transforms.end() ? &transformIt->second : nullptr;
	}

	[[nodiscard]] const TransformComponent* GetTransformComponent(EntityId entityId) const
	{
		auto transformIt = m_Transforms.find(entityId);
		return transformIt != m_Transforms.end() ? &transformIt->second : nullptr;
	}

	[[nodiscard]] MeshComponent* GetMeshComponent(EntityId entityId)
	{
		auto meshIt = m_Meshes.find(entityId);
		return meshIt != m_Meshes.end() ? &meshIt->second : nullptr;
	}

	[[nodiscard]] TransformComponent& EnsureTransformComponent(EntityId entityId)
	{
		return m_Transforms[entityId];
	}

	[[nodiscard]] MeshComponent& EnsureMeshComponent(EntityId entityId)
	{
		return m_Meshes[entityId];
	}

	[[nodiscard]] BoundsComponent& EnsureBoundsComponent(EntityId entityId)
	{
		return m_Bounds[entityId];
	}

	[[nodiscard]] CameraComponent& EnsureCameraComponent(EntityId entityId)
	{
		return m_Cameras[entityId];
	}

	[[nodiscard]] LightComponent& EnsureLightComponent(EntityId entityId)
	{
		return m_Lights[entityId];
	}

	[[nodiscard]] const MeshComponent* GetMeshComponent(EntityId entityId) const
	{
		auto meshIt = m_Meshes.find(entityId);
		return meshIt != m_Meshes.end() ? &meshIt->second : nullptr;
	}

	[[nodiscard]] Asset::StaticMeshAsset* GetMeshAsset(EntityId entityId)
	{
		auto* meshComponent = GetMeshComponent(entityId);
		return meshComponent ? meshComponent->Asset.get() : nullptr;
	}

	[[nodiscard]] const Asset::StaticMeshAsset* GetMeshAsset(EntityId entityId) const
	{
		auto* meshComponent = GetMeshComponent(entityId);
		return meshComponent ? meshComponent->Asset.get() : nullptr;
	}

	[[nodiscard]] std::vector<CpuMaterialTexture>* GetMaterialTextures(EntityId entityId)
	{
		auto* meshComponent = GetMeshComponent(entityId);
		return meshComponent ? &meshComponent->MaterialTextures : nullptr;
	}

	[[nodiscard]] const std::vector<CpuMaterialTexture>* GetMaterialTextures(EntityId entityId) const
	{
		auto* meshComponent = GetMeshComponent(entityId);
		return meshComponent ? &meshComponent->MaterialTextures : nullptr;
	}

	[[nodiscard]] BoundsComponent* GetBoundsComponent(EntityId entityId)
	{
		auto boundsIt = m_Bounds.find(entityId);
		return boundsIt != m_Bounds.end() ? &boundsIt->second : nullptr;
	}

	[[nodiscard]] const BoundsComponent* GetBoundsComponent(EntityId entityId) const
	{
		auto boundsIt = m_Bounds.find(entityId);
		return boundsIt != m_Bounds.end() ? &boundsIt->second : nullptr;
	}

	[[nodiscard]] CameraComponent* GetCameraComponent(EntityId entityId)
	{
		auto cameraIt = m_Cameras.find(entityId);
		return cameraIt != m_Cameras.end() ? &cameraIt->second : nullptr;
	}

	[[nodiscard]] const CameraComponent* GetCameraComponent(EntityId entityId) const
	{
		auto cameraIt = m_Cameras.find(entityId);
		return cameraIt != m_Cameras.end() ? &cameraIt->second : nullptr;
	}

	[[nodiscard]] LightComponent* GetLightComponent(EntityId entityId)
	{
		auto lightIt = m_Lights.find(entityId);
		return lightIt != m_Lights.end() ? &lightIt->second : nullptr;
	}

	[[nodiscard]] const LightComponent* GetLightComponent(EntityId entityId) const
	{
		auto lightIt = m_Lights.find(entityId);
		return lightIt != m_Lights.end() ? &lightIt->second : nullptr;
	}

	[[nodiscard]] const std::string* GetEntityName(EntityId entityId) const
	{
		for (const SceneEntity& entity : m_Entities)
		{
			if (entity.Id == entityId)
			{
				return &entity.Name;
			}
		}

		return nullptr;
	}

	[[nodiscard]] const std::vector<SceneEntity>& GetEntities() const noexcept
	{
		return m_Entities;
	}

	void ResetSelection() noexcept
	{
		m_SelectedEntity = InvalidEntityId;
	}

	[[nodiscard]] EntityId GetPrimaryRenderableEntity() const noexcept
	{
		return m_PrimaryRenderableEntity;
	}

	void SetPrimaryRenderableEntity(EntityId entityId) noexcept
	{
		m_PrimaryRenderableEntity = entityId;
	}

	[[nodiscard]] EntityId GetSelectedEntity() const noexcept
	{
		return m_SelectedEntity;
	}

	void SetSelectedEntity(EntityId entityId) noexcept
	{
		m_SelectedEntity = entityId;
	}

	[[nodiscard]] std::unordered_map<EntityId, TransformComponent>& GetTransforms() noexcept
	{
		return m_Transforms;
	}

	[[nodiscard]] std::unordered_map<EntityId, BoundsComponent>& GetBounds() noexcept
	{
		return m_Bounds;
	}

	[[nodiscard]] const std::unordered_map<EntityId, BoundsComponent>& GetBounds() const noexcept
	{
		return m_Bounds;
	}

private:
	EntityId m_NextEntityId = 1;
	EntityId m_PrimaryRenderableEntity = InvalidEntityId;
	EntityId m_SelectedEntity = InvalidEntityId;
	std::vector<SceneEntity> m_Entities;
	std::unordered_map<EntityId, TransformComponent> m_Transforms;
	std::unordered_map<EntityId, MeshComponent> m_Meshes;
	std::unordered_map<EntityId, BoundsComponent> m_Bounds;
	std::unordered_map<EntityId, CameraComponent> m_Cameras;
	std::unordered_map<EntityId, LightComponent> m_Lights;
};
