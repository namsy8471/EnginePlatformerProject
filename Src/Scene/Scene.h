#pragma once

#include "Scene/SceneComponents.h"
#include "Scene/SceneComponentStore.h"
#include "Scene/SceneTypes.h"

#include <DirectXMath.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

		for (auto& [otherEntityId, hierarchy] : m_Components.GetComponents<SceneHierarchyComponent>())
		{
			if (otherEntityId != entityId && hierarchy.Parent == entityId)
			{
				if (TransformComponent* transform = GetTransformComponent(otherEntityId))
				{
					transform->LocalTransform = transform->WorldTransform;
				}
				hierarchy.Parent = InvalidEntityId;
			}
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

	void Clear()
	{
		m_NextEntityId = 1;
		m_PrimaryRenderableEntity = InvalidEntityId;
		m_SelectedEntity = InvalidEntityId;
		m_Entities.clear();
		m_Components.Clear();
	}

	[[nodiscard]] bool MoveEntityToIndex(EntityId movedEntityId, size_t targetIndex)
	{
		auto movedIt = std::ranges::find_if(m_Entities, [movedEntityId](const SceneEntity& entity)
			{
				return entity.Id == movedEntityId;
			});
		if (movedIt == m_Entities.end())
		{
			return false;
		}

		SceneEntity movedEntity = *movedIt;
		const size_t oldIndex = static_cast<size_t>(std::distance(m_Entities.begin(), movedIt));
		m_Entities.erase(movedIt);
		if (oldIndex < targetIndex && targetIndex > 0)
		{
			--targetIndex;
		}
		targetIndex = (std::min)(targetIndex, m_Entities.size());
		m_Entities.insert(m_Entities.begin() + static_cast<std::ptrdiff_t>(targetIndex), movedEntity);
		return oldIndex != targetIndex;
	}

	[[nodiscard]] bool MoveEntityBefore(EntityId movedEntityId, EntityId targetEntityId)
	{
		if (movedEntityId == targetEntityId)
		{
			return false;
		}

		const auto targetIt = std::ranges::find_if(m_Entities, [targetEntityId](const SceneEntity& entity)
			{
				return entity.Id == targetEntityId;
			});
		if (targetIt == m_Entities.end())
		{
			return false;
		}

		return MoveEntityToIndex(movedEntityId, static_cast<size_t>(std::distance(m_Entities.begin(), targetIt)));
	}

	[[nodiscard]] bool MoveEntityAfter(EntityId movedEntityId, EntityId targetEntityId)
	{
		if (movedEntityId == targetEntityId)
		{
			return false;
		}

		const auto targetIt = std::ranges::find_if(m_Entities, [targetEntityId](const SceneEntity& entity)
			{
				return entity.Id == targetEntityId;
			});
		if (targetIt == m_Entities.end())
		{
			return false;
		}

		return MoveEntityToIndex(movedEntityId, static_cast<size_t>(std::distance(m_Entities.begin(), targetIt)) + 1);
	}

	[[nodiscard]] size_t GetEntityIndex(EntityId entityId) const
	{
		const auto it = std::ranges::find_if(m_Entities, [entityId](const SceneEntity& entity)
			{
				return entity.Id == entityId;
			});
		return it == m_Entities.end()
			? static_cast<size_t>(-1)
			: static_cast<size_t>(std::distance(m_Entities.begin(), it));
	}

	[[nodiscard]] EntityId GetParentEntity(EntityId entityId) const
	{
		const SceneHierarchyComponent* hierarchy = GetHierarchyComponent(entityId);
		return hierarchy ? hierarchy->Parent : InvalidEntityId;
	}

	[[nodiscard]] bool IsDescendantOf(EntityId entityId, EntityId possibleAncestor) const
	{
		if (entityId == InvalidEntityId || possibleAncestor == InvalidEntityId)
		{
			return false;
		}

		std::unordered_set<EntityId> visited;
		EntityId current = GetParentEntity(entityId);
		while (current != InvalidEntityId)
		{
			if (current == possibleAncestor)
			{
				return true;
			}
			if (!visited.insert(current).second)
			{
				return false;
			}
			current = GetParentEntity(current);
		}
		return false;
	}

	[[nodiscard]] bool SetParentEntity(EntityId childEntityId, EntityId parentEntityId, bool preserveWorldTransform = true)
	{
		if (!ContainsEntity(childEntityId) || childEntityId == parentEntityId)
		{
			return false;
		}
		if (parentEntityId != InvalidEntityId && !ContainsEntity(parentEntityId))
		{
			return false;
		}
		if (parentEntityId != InvalidEntityId && IsDescendantOf(parentEntityId, childEntityId))
		{
			return false;
		}

		const EntityId previousParent = GetParentEntity(childEntityId);
		if (previousParent == parentEntityId)
		{
			return false;
		}

		DirectX::XMMATRIX oldWorldMatrix = DirectX::XMMatrixIdentity();
		if (const TransformComponent* childTransform = GetTransformComponent(childEntityId))
		{
			oldWorldMatrix = childTransform->WorldTransform.ToXmMatrix();
		}

		SceneHierarchyComponent& hierarchy = EnsureHierarchyComponent(childEntityId);
		hierarchy.Parent = parentEntityId;

		if (preserveWorldTransform)
		{
			if (TransformComponent* childTransform = GetTransformComponent(childEntityId))
			{
				DirectX::XMMATRIX parentWorldMatrix = DirectX::XMMatrixIdentity();
				if (parentEntityId != InvalidEntityId)
				{
					if (const TransformComponent* parentTransform = GetTransformComponent(parentEntityId))
					{
						parentWorldMatrix = parentTransform->WorldTransform.ToXmMatrix();
					}
				}
				const DirectX::XMMATRIX localMatrix = oldWorldMatrix * DirectX::XMMatrixInverse(nullptr, parentWorldMatrix);
				childTransform->LocalTransform = Math::Transform::FromMatrix(Math::ToFloat4x4(localMatrix));
				childTransform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(childTransform->LocalTransform.Rotation);
			}
		}

		UpdateWorldTransforms();
		return true;
	}

	[[nodiscard]] std::vector<EntityId> GetChildEntities(EntityId parentEntityId) const
	{
		std::vector<EntityId> children;
		for (const SceneEntity& entity : m_Entities)
		{
			if (GetParentEntity(entity.Id) == parentEntityId)
			{
				children.push_back(entity.Id);
			}
		}
		return children;
	}

	[[nodiscard]] std::vector<EntityId> GetRootEntities() const
	{
		std::vector<EntityId> roots;
		for (const SceneEntity& entity : m_Entities)
		{
			const EntityId parentEntity = GetParentEntity(entity.Id);
			if (parentEntity == InvalidEntityId || !ContainsEntity(parentEntity))
			{
				roots.push_back(entity.Id);
			}
		}
		return roots;
	}

	void UpdateWorldTransforms()
	{
		std::unordered_set<EntityId> visited;
		std::unordered_set<EntityId> visiting;

		const auto updateEntity = [this, &visited, &visiting](auto&& self, EntityId entityId, const Math::Transform& parentTransform) -> void
			{
				if (!ContainsEntity(entityId) || visited.contains(entityId))
				{
					return;
				}
				if (!visiting.insert(entityId).second)
				{
					if (SceneHierarchyComponent* hierarchy = GetHierarchyComponent(entityId))
					{
						hierarchy->Parent = InvalidEntityId;
					}
					return;
				}

				Math::Transform worldTransform = parentTransform;
				if (TransformComponent* transform = GetTransformComponent(entityId))
				{
					transform->UpdateWorld(parentTransform);
					worldTransform = transform->WorldTransform;
				}

				for (EntityId childEntityId : GetChildEntities(entityId))
				{
					self(self, childEntityId, worldTransform);
				}

				visiting.erase(entityId);
				visited.insert(entityId);
			};

		for (EntityId rootEntityId : GetRootEntities())
		{
			updateEntity(updateEntity, rootEntityId, Math::Transform::Identity());
		}

		for (const SceneEntity& entity : m_Entities)
		{
			if (!visited.contains(entity.Id))
			{
				updateEntity(updateEntity, entity.Id, Math::Transform::Identity());
			}
		}
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

		if (const EditorStateComponent* sourceEditorState = GetEditorStateComponent(sourceEntityId))
		{
			EnsureEditorStateComponent(duplicateEntityId) = *sourceEditorState;
		}
		if (const SceneHierarchyComponent* sourceHierarchy = GetHierarchyComponent(sourceEntityId))
		{
			EnsureHierarchyComponent(duplicateEntityId) = *sourceHierarchy;
		}

		if (const MeshComponent* sourceMesh = GetMeshComponent(sourceEntityId))
		{
			MeshComponent& duplicateMesh = EnsureMeshComponent(duplicateEntityId);
			if (sourceMesh->Asset)
			{
				duplicateMesh.Asset = std::make_unique<Asset::StaticMeshAsset>(*sourceMesh->Asset);
			}
			duplicateMesh.MaterialTextures = sourceMesh->MaterialTextures;
			SetComponentEnabled<MeshComponent>(duplicateEntityId, IsComponentEnabled<MeshComponent>(sourceEntityId));
		}

		if (const AnimatorComponent* sourceAnimator = GetAnimatorComponent(sourceEntityId))
		{
			EnsureAnimatorComponent(duplicateEntityId) = *sourceAnimator;
			SetComponentEnabled<AnimatorComponent>(duplicateEntityId, IsComponentEnabled<AnimatorComponent>(sourceEntityId));
		}

		if (const BoundsComponent* sourceBounds = GetBoundsComponent(sourceEntityId))
		{
			EnsureBoundsComponent(duplicateEntityId) = *sourceBounds;
			SetComponentEnabled<BoundsComponent>(duplicateEntityId, IsComponentEnabled<BoundsComponent>(sourceEntityId));
		}

		if (const CameraComponent* sourceCamera = GetCameraComponent(sourceEntityId))
		{
			EnsureCameraComponent(duplicateEntityId) = *sourceCamera;
			SetComponentEnabled<CameraComponent>(duplicateEntityId, IsComponentEnabled<CameraComponent>(sourceEntityId));
		}

		if (const LightComponent* sourceLight = GetLightComponent(sourceEntityId))
		{
			EnsureLightComponent(duplicateEntityId) = *sourceLight;
			SetComponentEnabled<LightComponent>(duplicateEntityId, IsComponentEnabled<LightComponent>(sourceEntityId));
		}

		if (const RigidBodyComponent* sourceRigidBody = GetRigidBodyComponent(sourceEntityId))
		{
			EnsureRigidBodyComponent(duplicateEntityId) = *sourceRigidBody;
			SetComponentEnabled<RigidBodyComponent>(duplicateEntityId, IsComponentEnabled<RigidBodyComponent>(sourceEntityId));
		}

		if (const ColliderComponent* sourceCollider = GetColliderComponent(sourceEntityId))
		{
			EnsureColliderComponent(duplicateEntityId) = *sourceCollider;
			SetComponentEnabled<ColliderComponent>(duplicateEntityId, IsComponentEnabled<ColliderComponent>(sourceEntityId));
		}

		if (const PhysicsMaterialComponent* sourceMaterial = GetPhysicsMaterialComponent(sourceEntityId))
		{
			EnsurePhysicsMaterialComponent(duplicateEntityId) = *sourceMaterial;
			SetComponentEnabled<PhysicsMaterialComponent>(duplicateEntityId, IsComponentEnabled<PhysicsMaterialComponent>(sourceEntityId));
		}

		if (const PrefabInstanceComponent* sourcePrefab = GetPrefabInstanceComponent(sourceEntityId))
		{
			EnsurePrefabInstanceComponent(duplicateEntityId) = *sourcePrefab;
			SetComponentEnabled<PrefabInstanceComponent>(duplicateEntityId, IsComponentEnabled<PrefabInstanceComponent>(sourceEntityId));
		}

		if (const SceneReferenceComponent* sourceSceneReference = GetSceneReferenceComponent(sourceEntityId))
		{
			EnsureSceneReferenceComponent(duplicateEntityId) = *sourceSceneReference;
			SetComponentEnabled<SceneReferenceComponent>(duplicateEntityId, IsComponentEnabled<SceneReferenceComponent>(sourceEntityId));
		}

		if (const ScriptComponent* sourceScript = GetScriptComponent(sourceEntityId))
		{
			EnsureScriptComponent(duplicateEntityId) = *sourceScript;
			SetComponentEnabled<ScriptComponent>(duplicateEntityId, IsComponentEnabled<ScriptComponent>(sourceEntityId));
		}

		if (const Sprite2DComponent* sourceSprite = GetSprite2DComponent(sourceEntityId))
		{
			EnsureSprite2DComponent(duplicateEntityId) = *sourceSprite;
			SetComponentEnabled<Sprite2DComponent>(duplicateEntityId, IsComponentEnabled<Sprite2DComponent>(sourceEntityId));
		}

		if (const UiElementComponent* sourceUi = GetUiElementComponent(sourceEntityId))
		{
			EnsureUiElementComponent(duplicateEntityId) = *sourceUi;
			SetComponentEnabled<UiElementComponent>(duplicateEntityId, IsComponentEnabled<UiElementComponent>(sourceEntityId));
		}

		if (const AudioSourceComponent* sourceAudio = GetAudioSourceComponent(sourceEntityId))
		{
			EnsureAudioSourceComponent(duplicateEntityId) = *sourceAudio;
			SetComponentEnabled<AudioSourceComponent>(duplicateEntityId, IsComponentEnabled<AudioSourceComponent>(sourceEntityId));
		}

		if (const NavigationAgentComponent* sourceNavigation = GetNavigationAgentComponent(sourceEntityId))
		{
			EnsureNavigationAgentComponent(duplicateEntityId) = *sourceNavigation;
			SetComponentEnabled<NavigationAgentComponent>(duplicateEntityId, IsComponentEnabled<NavigationAgentComponent>(sourceEntityId));
		}

		if (const NetworkIdentityComponent* sourceNetwork = GetNetworkIdentityComponent(sourceEntityId))
		{
			EnsureNetworkIdentityComponent(duplicateEntityId) = *sourceNetwork;
			SetComponentEnabled<NetworkIdentityComponent>(duplicateEntityId, IsComponentEnabled<NetworkIdentityComponent>(sourceEntityId));
		}

		UpdateWorldTransforms();
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
		if constexpr (std::is_same_v<Component, TransformComponent>)
		{
			(void)entityId;
			return false;
		}

		return m_Components.RemoveComponent<Component>(entityId);
	}

	template <typename Component>
	[[nodiscard]] bool IsComponentEnabled(EntityId entityId) const
	{
		if constexpr (std::is_same_v<Component, TransformComponent>)
		{
			return HasComponent<TransformComponent>(entityId);
		}
		else
		{
			return m_Components.IsComponentEnabled<Component>(entityId);
		}
	}

	template <typename Component>
	bool SetComponentEnabled(EntityId entityId, bool enabled)
	{
		if constexpr (std::is_same_v<Component, TransformComponent>)
		{
			(void)entityId;
			(void)enabled;
			return false;
		}
		else
		{
			return m_Components.SetComponentEnabled<Component>(entityId, enabled);
		}
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

	[[nodiscard]] EditorStateComponent& EnsureEditorStateComponent(EntityId entityId)
	{
		return EnsureComponent<EditorStateComponent>(entityId);
	}

	[[nodiscard]] EditorStateComponent* GetEditorStateComponent(EntityId entityId)
	{
		return GetComponent<EditorStateComponent>(entityId);
	}

	[[nodiscard]] const EditorStateComponent* GetEditorStateComponent(EntityId entityId) const
	{
		return GetComponent<EditorStateComponent>(entityId);
	}

	[[nodiscard]] SceneHierarchyComponent& EnsureHierarchyComponent(EntityId entityId)
	{
		return EnsureComponent<SceneHierarchyComponent>(entityId);
	}

	[[nodiscard]] SceneHierarchyComponent* GetHierarchyComponent(EntityId entityId)
	{
		return GetComponent<SceneHierarchyComponent>(entityId);
	}

	[[nodiscard]] const SceneHierarchyComponent* GetHierarchyComponent(EntityId entityId) const
	{
		return GetComponent<SceneHierarchyComponent>(entityId);
	}

	[[nodiscard]] bool IsEntityVisibleInScene(EntityId entityId) const
	{
		const EditorStateComponent* editorState = GetEditorStateComponent(entityId);
		return !editorState || editorState->VisibleInScene;
	}

	[[nodiscard]] bool IsEntityPickableInScene(EntityId entityId) const
	{
		const EditorStateComponent* editorState = GetEditorStateComponent(entityId);
		return !editorState || editorState->PickableInScene;
	}

	bool SetEntityVisibleInScene(EntityId entityId, bool visible)
	{
		if (!ContainsEntity(entityId))
		{
			return false;
		}
		EditorStateComponent& editorState = EnsureEditorStateComponent(entityId);
		if (editorState.VisibleInScene == visible)
		{
			return false;
		}
		editorState.VisibleInScene = visible;
		return true;
	}

	bool SetEntityPickableInScene(EntityId entityId, bool pickable)
	{
		if (!ContainsEntity(entityId))
		{
			return false;
		}
		EditorStateComponent& editorState = EnsureEditorStateComponent(entityId);
		if (editorState.PickableInScene == pickable)
		{
			return false;
		}
		editorState.PickableInScene = pickable;
		return true;
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

	[[nodiscard]] RigidBodyComponent& EnsureRigidBodyComponent(EntityId entityId)
	{
		return EnsureComponent<RigidBodyComponent>(entityId);
	}

	[[nodiscard]] ColliderComponent& EnsureColliderComponent(EntityId entityId)
	{
		return EnsureComponent<ColliderComponent>(entityId);
	}

	[[nodiscard]] PhysicsMaterialComponent& EnsurePhysicsMaterialComponent(EntityId entityId)
	{
		return EnsureComponent<PhysicsMaterialComponent>(entityId);
	}

	[[nodiscard]] PrefabInstanceComponent& EnsurePrefabInstanceComponent(EntityId entityId)
	{
		return EnsureComponent<PrefabInstanceComponent>(entityId);
	}

	[[nodiscard]] SceneReferenceComponent& EnsureSceneReferenceComponent(EntityId entityId)
	{
		return EnsureComponent<SceneReferenceComponent>(entityId);
	}

	[[nodiscard]] ScriptComponent& EnsureScriptComponent(EntityId entityId)
	{
		return EnsureComponent<ScriptComponent>(entityId);
	}

	[[nodiscard]] Sprite2DComponent& EnsureSprite2DComponent(EntityId entityId)
	{
		return EnsureComponent<Sprite2DComponent>(entityId);
	}

	[[nodiscard]] UiElementComponent& EnsureUiElementComponent(EntityId entityId)
	{
		return EnsureComponent<UiElementComponent>(entityId);
	}

	[[nodiscard]] AudioSourceComponent& EnsureAudioSourceComponent(EntityId entityId)
	{
		return EnsureComponent<AudioSourceComponent>(entityId);
	}

	[[nodiscard]] NavigationAgentComponent& EnsureNavigationAgentComponent(EntityId entityId)
	{
		return EnsureComponent<NavigationAgentComponent>(entityId);
	}

	[[nodiscard]] NetworkIdentityComponent& EnsureNetworkIdentityComponent(EntityId entityId)
	{
		return EnsureComponent<NetworkIdentityComponent>(entityId);
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

	[[nodiscard]] RigidBodyComponent* GetRigidBodyComponent(EntityId entityId)
	{
		return GetComponent<RigidBodyComponent>(entityId);
	}

	[[nodiscard]] const RigidBodyComponent* GetRigidBodyComponent(EntityId entityId) const
	{
		return GetComponent<RigidBodyComponent>(entityId);
	}

	[[nodiscard]] ColliderComponent* GetColliderComponent(EntityId entityId)
	{
		return GetComponent<ColliderComponent>(entityId);
	}

	[[nodiscard]] const ColliderComponent* GetColliderComponent(EntityId entityId) const
	{
		return GetComponent<ColliderComponent>(entityId);
	}

	[[nodiscard]] PhysicsMaterialComponent* GetPhysicsMaterialComponent(EntityId entityId)
	{
		return GetComponent<PhysicsMaterialComponent>(entityId);
	}

	[[nodiscard]] const PhysicsMaterialComponent* GetPhysicsMaterialComponent(EntityId entityId) const
	{
		return GetComponent<PhysicsMaterialComponent>(entityId);
	}

	[[nodiscard]] PrefabInstanceComponent* GetPrefabInstanceComponent(EntityId entityId)
	{
		return GetComponent<PrefabInstanceComponent>(entityId);
	}

	[[nodiscard]] const PrefabInstanceComponent* GetPrefabInstanceComponent(EntityId entityId) const
	{
		return GetComponent<PrefabInstanceComponent>(entityId);
	}

	[[nodiscard]] SceneReferenceComponent* GetSceneReferenceComponent(EntityId entityId)
	{
		return GetComponent<SceneReferenceComponent>(entityId);
	}

	[[nodiscard]] const SceneReferenceComponent* GetSceneReferenceComponent(EntityId entityId) const
	{
		return GetComponent<SceneReferenceComponent>(entityId);
	}

	[[nodiscard]] ScriptComponent* GetScriptComponent(EntityId entityId)
	{
		return GetComponent<ScriptComponent>(entityId);
	}

	[[nodiscard]] const ScriptComponent* GetScriptComponent(EntityId entityId) const
	{
		return GetComponent<ScriptComponent>(entityId);
	}

	[[nodiscard]] Sprite2DComponent* GetSprite2DComponent(EntityId entityId)
	{
		return GetComponent<Sprite2DComponent>(entityId);
	}

	[[nodiscard]] const Sprite2DComponent* GetSprite2DComponent(EntityId entityId) const
	{
		return GetComponent<Sprite2DComponent>(entityId);
	}

	[[nodiscard]] UiElementComponent* GetUiElementComponent(EntityId entityId)
	{
		return GetComponent<UiElementComponent>(entityId);
	}

	[[nodiscard]] const UiElementComponent* GetUiElementComponent(EntityId entityId) const
	{
		return GetComponent<UiElementComponent>(entityId);
	}

	[[nodiscard]] AudioSourceComponent* GetAudioSourceComponent(EntityId entityId)
	{
		return GetComponent<AudioSourceComponent>(entityId);
	}

	[[nodiscard]] const AudioSourceComponent* GetAudioSourceComponent(EntityId entityId) const
	{
		return GetComponent<AudioSourceComponent>(entityId);
	}

	[[nodiscard]] NavigationAgentComponent* GetNavigationAgentComponent(EntityId entityId)
	{
		return GetComponent<NavigationAgentComponent>(entityId);
	}

	[[nodiscard]] const NavigationAgentComponent* GetNavigationAgentComponent(EntityId entityId) const
	{
		return GetComponent<NavigationAgentComponent>(entityId);
	}

	[[nodiscard]] NetworkIdentityComponent* GetNetworkIdentityComponent(EntityId entityId)
	{
		return GetComponent<NetworkIdentityComponent>(entityId);
	}

	[[nodiscard]] const NetworkIdentityComponent* GetNetworkIdentityComponent(EntityId entityId) const
	{
		return GetComponent<NetworkIdentityComponent>(entityId);
	}

	bool RemoveRigidBodyComponent(EntityId entityId)
	{
		return RemoveComponent<RigidBodyComponent>(entityId);
	}

	bool RemoveMeshComponent(EntityId entityId)
	{
		return RemoveComponent<MeshComponent>(entityId);
	}

	bool RemoveAnimatorComponent(EntityId entityId)
	{
		return RemoveComponent<AnimatorComponent>(entityId);
	}

	bool RemoveCameraComponent(EntityId entityId)
	{
		return RemoveComponent<CameraComponent>(entityId);
	}

	bool RemoveLightComponent(EntityId entityId)
	{
		return RemoveComponent<LightComponent>(entityId);
	}

	bool RemoveColliderComponent(EntityId entityId)
	{
		return RemoveComponent<ColliderComponent>(entityId);
	}

	bool RemovePhysicsMaterialComponent(EntityId entityId)
	{
		return RemoveComponent<PhysicsMaterialComponent>(entityId);
	}

	bool RemovePrefabInstanceComponent(EntityId entityId)
	{
		return RemoveComponent<PrefabInstanceComponent>(entityId);
	}

	bool RemoveSceneReferenceComponent(EntityId entityId)
	{
		return RemoveComponent<SceneReferenceComponent>(entityId);
	}

	bool RemoveScriptComponent(EntityId entityId)
	{
		return RemoveComponent<ScriptComponent>(entityId);
	}

	bool RemoveSprite2DComponent(EntityId entityId)
	{
		return RemoveComponent<Sprite2DComponent>(entityId);
	}

	bool RemoveUiElementComponent(EntityId entityId)
	{
		return RemoveComponent<UiElementComponent>(entityId);
	}

	bool RemoveAudioSourceComponent(EntityId entityId)
	{
		return RemoveComponent<AudioSourceComponent>(entityId);
	}

	bool RemoveNavigationAgentComponent(EntityId entityId)
	{
		return RemoveComponent<NavigationAgentComponent>(entityId);
	}

	bool RemoveNetworkIdentityComponent(EntityId entityId)
	{
		return RemoveComponent<NetworkIdentityComponent>(entityId);
	}

	[[nodiscard]] bool IsMeshEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<MeshComponent>(entityId);
	}

	[[nodiscard]] bool IsAnimatorEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<AnimatorComponent>(entityId);
	}

	[[nodiscard]] bool IsCameraEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<CameraComponent>(entityId);
	}

	[[nodiscard]] bool IsLightEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<LightComponent>(entityId);
	}

	[[nodiscard]] bool IsRigidBodyEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<RigidBodyComponent>(entityId);
	}

	[[nodiscard]] bool IsColliderEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<ColliderComponent>(entityId);
	}

	[[nodiscard]] bool IsPhysicsMaterialEnabled(EntityId entityId) const
	{
		return IsComponentEnabled<PhysicsMaterialComponent>(entityId);
	}

	bool SetMeshEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<MeshComponent>(entityId, enabled);
	}

	bool SetAnimatorEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<AnimatorComponent>(entityId, enabled);
	}

	bool SetCameraEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<CameraComponent>(entityId, enabled);
	}

	bool SetLightEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<LightComponent>(entityId, enabled);
	}

	bool SetRigidBodyEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<RigidBodyComponent>(entityId, enabled);
	}

	bool SetColliderEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<ColliderComponent>(entityId, enabled);
	}

	bool SetPhysicsMaterialEnabled(EntityId entityId, bool enabled)
	{
		return SetComponentEnabled<PhysicsMaterialComponent>(entityId, enabled);
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

	[[nodiscard]] SceneComponentStore::ComponentMap<TransformComponent>& GetTransforms()
	{
		return m_Components.GetComponents<TransformComponent>();
	}

	[[nodiscard]] SceneComponentStore::ComponentMap<BoundsComponent>& GetBounds()
	{
		return m_Components.GetComponents<BoundsComponent>();
	}

	[[nodiscard]] const SceneComponentStore::ComponentMap<BoundsComponent>& GetBounds() const noexcept
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
