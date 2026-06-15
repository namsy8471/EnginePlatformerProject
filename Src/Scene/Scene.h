#pragma once

#include "Scene/SceneComponents.h"
#include "Scene/SceneComponentStore.h"
#include "Scene/SceneTypes.h"

#include <DirectXMath.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class Scene
{
public:
	[[nodiscard]] EntityId CreateEntity(std::string_view name)
	{
		const EntityId entityId = m_NextEntityId++;
		m_Entities.push_back({ entityId });
		static_cast<void>(AddComponent(entityId, NameComponent{ std::string(name) }));
		return entityId;
	}

	[[nodiscard]] bool ContainsEntity(EntityId entityId) const
	{
		return std::ranges::any_of(m_Entities, [entityId](const SceneEntity& entity)
			{
				return entity.Id == entityId;
			});
	}

	[[nodiscard]] bool RenameEntity(EntityId entityId, std::string_view name)
	{
		if (name.empty())
		{
			return false;
		}

		if (!ContainsEntity(entityId))
		{
			return false;
		}

		EnsureComponent<NameComponent>(entityId).Name = name;
		return true;
	}

	[[nodiscard]] bool DeleteEntity(EntityId entityId)
	{
		if (entityId == InvalidEntityId)
		{
			return false;
		}

		const auto oldEntityCount = m_Entities.size();
		std::erase_if(m_Entities, [entityId](const SceneEntity& entity)
			{
				return entity.Id == entityId;
			});
		if (m_Entities.size() == oldEntityCount)
		{
			return false;
		}

		m_Components.RemoveEntity(entityId);
		if (m_SelectedEntity == entityId)
		{
			m_SelectedEntity = InvalidEntityId;
		}
		if (m_PrimaryRenderableEntity == entityId)
		{
			m_PrimaryRenderableEntity = InvalidEntityId;
		}
		return true;
	}

	[[nodiscard]] EntityId DuplicateEntity(EntityId sourceEntityId, std::string_view newName, const DirectX::XMFLOAT3& transformOffset)
	{
		if (!ContainsEntity(sourceEntityId) || newName.empty())
		{
			return InvalidEntityId;
		}

		const EntityId duplicateEntityId = CreateEntity(newName);
		if (const TransformComponent* sourceTransform = GetTransformComponent(sourceEntityId))
		{
			TransformComponent& duplicateTransform = EnsureTransformComponent(duplicateEntityId);
			duplicateTransform = *sourceTransform;
			duplicateTransform.LocalTransform.Translation.x += transformOffset.x;
			duplicateTransform.LocalTransform.Translation.y += transformOffset.y;
			duplicateTransform.LocalTransform.Translation.z += transformOffset.z;
			duplicateTransform.UpdateWorld();
		}

		if (const MeshComponent* sourceMesh = GetMeshComponent(sourceEntityId))
		{
			MeshComponent& duplicateMesh = EnsureMeshComponent(duplicateEntityId);
			if (sourceMesh->Asset)
			{
				duplicateMesh.Asset = std::make_unique<Asset::StaticMeshAsset>(*sourceMesh->Asset);
			}
			duplicateMesh.MaterialTextures = sourceMesh->MaterialTextures;
		}

		if (const AnimatorComponent* sourceAnimator = GetAnimatorComponent(sourceEntityId))
		{
			EnsureAnimatorComponent(duplicateEntityId) = *sourceAnimator;
		}

		if (const BoundsComponent* sourceBounds = GetBoundsComponent(sourceEntityId))
		{
			EnsureBoundsComponent(duplicateEntityId) = *sourceBounds;
		}

		if (const CameraComponent* sourceCamera = GetCameraComponent(sourceEntityId))
		{
			EnsureCameraComponent(duplicateEntityId) = *sourceCamera;
		}

		if (const LightComponent* sourceLight = GetLightComponent(sourceEntityId))
		{
			EnsureLightComponent(duplicateEntityId) = *sourceLight;
		}

		return duplicateEntityId;
	}

	[[nodiscard]] EntityId CreateModelEntity(
		std::string_view name,
		const Math::Transform& localTransform,
		std::unique_ptr<Asset::StaticMeshAsset> meshAsset,
		std::vector<CpuMaterialTexture> materialTextures,
		const BoundsComponent& bounds)
	{
		const EntityId entityId = CreateEntity(name);
		TransformComponent& transform = EnsureTransformComponent(entityId);
		transform.LocalTransform = localTransform;
		transform.UpdateWorld();

		MeshComponent& mesh = EnsureMeshComponent(entityId);
		mesh.Asset = std::move(meshAsset);
		mesh.MaterialTextures = std::move(materialTextures);
		EnsureAnimatorForMesh(entityId);

		BoundsComponent& storedBounds = EnsureBoundsComponent(entityId);
		storedBounds = bounds;
		return entityId;
	}

	void ReplaceEntityModel(
		EntityId entityId,
		std::unique_ptr<Asset::StaticMeshAsset> meshAsset,
		std::vector<CpuMaterialTexture> materialTextures,
		const BoundsComponent& bounds)
	{
		if (entityId == InvalidEntityId)
		{
			return;
		}

		MeshComponent& mesh = EnsureMeshComponent(entityId);
		mesh.Asset = std::move(meshAsset);
		mesh.MaterialTextures = std::move(materialTextures);
		EnsureAnimatorForMesh(entityId);

		BoundsComponent& storedBounds = EnsureBoundsComponent(entityId);
		storedBounds = bounds;
	}

	template <typename Component>
	[[nodiscard]] Component& AddComponent(EntityId entityId, Component component)
	{
		return m_Components.AddComponent<Component>(entityId, std::move(component));
	}

	template <typename Component>
	[[nodiscard]] Component& EnsureComponent(EntityId entityId)
	{
		return m_Components.EnsureComponent<Component>(entityId);
	}

	template <typename Component>
	[[nodiscard]] Component* GetComponent(EntityId entityId)
	{
		return m_Components.GetComponent<Component>(entityId);
	}

	template <typename Component>
	[[nodiscard]] const Component* GetComponent(EntityId entityId) const
	{
		return m_Components.GetComponent<Component>(entityId);
	}

	template <typename Component>
	[[nodiscard]] bool HasComponent(EntityId entityId) const
	{
		return m_Components.HasComponent<Component>(entityId);
	}

	template <typename Component>
	bool RemoveComponent(EntityId entityId)
	{
		return m_Components.RemoveComponent<Component>(entityId);
	}

	[[nodiscard]] TransformComponent* GetTransformComponent(EntityId entityId)
	{
		return GetComponent<TransformComponent>(entityId);
	}

	[[nodiscard]] const TransformComponent* GetTransformComponent(EntityId entityId) const
	{
		return GetComponent<TransformComponent>(entityId);
	}

	[[nodiscard]] MeshComponent* GetMeshComponent(EntityId entityId)
	{
		return GetComponent<MeshComponent>(entityId);
	}

	[[nodiscard]] TransformComponent& EnsureTransformComponent(EntityId entityId)
	{
		return EnsureComponent<TransformComponent>(entityId);
	}

	[[nodiscard]] MeshComponent& EnsureMeshComponent(EntityId entityId)
	{
		return EnsureComponent<MeshComponent>(entityId);
	}

	[[nodiscard]] BoundsComponent& EnsureBoundsComponent(EntityId entityId)
	{
		return EnsureComponent<BoundsComponent>(entityId);
	}

	[[nodiscard]] AnimatorComponent& EnsureAnimatorComponent(EntityId entityId)
	{
		return EnsureComponent<AnimatorComponent>(entityId);
	}

	[[nodiscard]] CameraComponent& EnsureCameraComponent(EntityId entityId)
	{
		return EnsureComponent<CameraComponent>(entityId);
	}

	[[nodiscard]] LightComponent& EnsureLightComponent(EntityId entityId)
	{
		return EnsureComponent<LightComponent>(entityId);
	}

	[[nodiscard]] const MeshComponent* GetMeshComponent(EntityId entityId) const
	{
		return GetComponent<MeshComponent>(entityId);
	}

	[[nodiscard]] AnimatorComponent* GetAnimatorComponent(EntityId entityId)
	{
		return GetComponent<AnimatorComponent>(entityId);
	}

	[[nodiscard]] const AnimatorComponent* GetAnimatorComponent(EntityId entityId) const
	{
		return GetComponent<AnimatorComponent>(entityId);
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
		return GetComponent<BoundsComponent>(entityId);
	}

	[[nodiscard]] const BoundsComponent* GetBoundsComponent(EntityId entityId) const
	{
		return GetComponent<BoundsComponent>(entityId);
	}

	[[nodiscard]] CameraComponent* GetCameraComponent(EntityId entityId)
	{
		return GetComponent<CameraComponent>(entityId);
	}

	[[nodiscard]] const CameraComponent* GetCameraComponent(EntityId entityId) const
	{
		return GetComponent<CameraComponent>(entityId);
	}

	[[nodiscard]] LightComponent* GetLightComponent(EntityId entityId)
	{
		return GetComponent<LightComponent>(entityId);
	}

	[[nodiscard]] const LightComponent* GetLightComponent(EntityId entityId) const
	{
		return GetComponent<LightComponent>(entityId);
	}

	[[nodiscard]] const std::string* GetEntityName(EntityId entityId) const
	{
		const NameComponent* name = GetComponent<NameComponent>(entityId);
		return name ? &name->Name : nullptr;
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

	[[nodiscard]] std::unordered_map<EntityId, TransformComponent>& GetTransforms()
	{
		return m_Components.GetComponents<TransformComponent>();
	}

	[[nodiscard]] std::unordered_map<EntityId, BoundsComponent>& GetBounds()
	{
		return m_Components.GetComponents<BoundsComponent>();
	}

	[[nodiscard]] const std::unordered_map<EntityId, BoundsComponent>& GetBounds() const noexcept
	{
		return m_Components.GetComponents<BoundsComponent>();
	}

private:
	void EnsureAnimatorForMesh(EntityId entityId)
	{
		const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entityId);
		if (!meshAsset || !meshAsset->IsAnimated || meshAsset->Animations.empty())
		{
			RemoveComponent<AnimatorComponent>(entityId);
			return;
		}

		AnimatorComponent& animator = EnsureAnimatorComponent(entityId);
		const uint32_t lastClipIndex = static_cast<uint32_t>(meshAsset->Animations.size() - 1);
		animator.CurrentClipIndex = (std::min)(animator.CurrentClipIndex, lastClipIndex);
		animator.TimeSeconds = (std::max)(animator.TimeSeconds, 0.0f);
	}

	EntityId m_NextEntityId = 1;
	EntityId m_PrimaryRenderableEntity = InvalidEntityId;
	EntityId m_SelectedEntity = InvalidEntityId;
	std::vector<SceneEntity> m_Entities;
	SceneComponentStore m_Components;
};
