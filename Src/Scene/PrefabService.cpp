#include "PrefabService.h"

#include "Scene/SceneRenderState.h"

#include <memory>

namespace ScenePersistence
{
	namespace
	{
		void CopyOptionalComponents(
			const Scene& sourceScene,
			EntityId sourceEntity,
			Scene& targetScene,
			EntityId targetEntity,
			const PrefabSaveOptions& options)
		{
			if (const TransformComponent* transform = sourceScene.GetTransformComponent(sourceEntity))
			{
				TransformComponent& targetTransform = targetScene.EnsureTransformComponent(targetEntity);
				targetTransform = *transform;
			}

			if (const EditorStateComponent* editorState = sourceScene.GetEditorStateComponent(sourceEntity))
			{
				targetScene.EnsureEditorStateComponent(targetEntity) = *editorState;
			}
			if (const SceneHierarchyComponent* hierarchy = sourceScene.GetHierarchyComponent(sourceEntity))
			{
				targetScene.EnsureHierarchyComponent(targetEntity) = *hierarchy;
			}

			if (const MeshComponent* mesh = sourceScene.GetMeshComponent(sourceEntity))
			{
				MeshComponent& targetMesh = targetScene.EnsureMeshComponent(targetEntity);
				if (mesh->Asset)
				{
					targetMesh.Asset = std::make_unique<Asset::StaticMeshAsset>(*mesh->Asset);
				}
				targetMesh.MaterialTextures = mesh->MaterialTextures;
				static_cast<void>(targetScene.SetComponentEnabled<MeshComponent>(targetEntity, sourceScene.IsComponentEnabled<MeshComponent>(sourceEntity)));
			}

			if (const BoundsComponent* bounds = sourceScene.GetBoundsComponent(sourceEntity))
			{
				targetScene.EnsureBoundsComponent(targetEntity) = *bounds;
			}

			if (const CameraComponent* camera = sourceScene.GetCameraComponent(sourceEntity))
			{
				targetScene.EnsureCameraComponent(targetEntity) = *camera;
				static_cast<void>(targetScene.SetComponentEnabled<CameraComponent>(targetEntity, sourceScene.IsComponentEnabled<CameraComponent>(sourceEntity)));
			}

			if (const LightComponent* light = sourceScene.GetLightComponent(sourceEntity))
			{
				targetScene.EnsureLightComponent(targetEntity) = *light;
				static_cast<void>(targetScene.SetComponentEnabled<LightComponent>(targetEntity, sourceScene.IsComponentEnabled<LightComponent>(sourceEntity)));
			}

			if (const AnimatorComponent* animator = sourceScene.GetAnimatorComponent(sourceEntity))
			{
				targetScene.EnsureAnimatorComponent(targetEntity) = *animator;
				static_cast<void>(targetScene.SetComponentEnabled<AnimatorComponent>(targetEntity, sourceScene.IsComponentEnabled<AnimatorComponent>(sourceEntity)));
			}

			if (const RigidBodyComponent* rigidBody = sourceScene.GetRigidBodyComponent(sourceEntity))
			{
				targetScene.EnsureRigidBodyComponent(targetEntity) = *rigidBody;
				static_cast<void>(targetScene.SetComponentEnabled<RigidBodyComponent>(targetEntity, sourceScene.IsComponentEnabled<RigidBodyComponent>(sourceEntity)));
			}

			if (const ColliderComponent* collider = sourceScene.GetColliderComponent(sourceEntity))
			{
				targetScene.EnsureColliderComponent(targetEntity) = *collider;
				static_cast<void>(targetScene.SetComponentEnabled<ColliderComponent>(targetEntity, sourceScene.IsComponentEnabled<ColliderComponent>(sourceEntity)));
			}

			if (const PhysicsMaterialComponent* physicsMaterial = sourceScene.GetPhysicsMaterialComponent(sourceEntity))
			{
				targetScene.EnsurePhysicsMaterialComponent(targetEntity) = *physicsMaterial;
				static_cast<void>(targetScene.SetComponentEnabled<PhysicsMaterialComponent>(targetEntity, sourceScene.IsComponentEnabled<PhysicsMaterialComponent>(sourceEntity)));
			}

			if (options.IncludePrefabInstanceComponent)
			{
				if (const PrefabInstanceComponent* prefab = sourceScene.GetPrefabInstanceComponent(sourceEntity))
				{
					targetScene.EnsurePrefabInstanceComponent(targetEntity) = *prefab;
					static_cast<void>(targetScene.SetComponentEnabled<PrefabInstanceComponent>(targetEntity, sourceScene.IsComponentEnabled<PrefabInstanceComponent>(sourceEntity)));
				}
			}

			if (const SceneReferenceComponent* sceneReference = sourceScene.GetSceneReferenceComponent(sourceEntity))
			{
				targetScene.EnsureSceneReferenceComponent(targetEntity) = *sceneReference;
				static_cast<void>(targetScene.SetComponentEnabled<SceneReferenceComponent>(targetEntity, sourceScene.IsComponentEnabled<SceneReferenceComponent>(sourceEntity)));
			}

			if (const ScriptComponent* script = sourceScene.GetScriptComponent(sourceEntity))
			{
				targetScene.EnsureScriptComponent(targetEntity) = *script;
				static_cast<void>(targetScene.SetComponentEnabled<ScriptComponent>(targetEntity, sourceScene.IsComponentEnabled<ScriptComponent>(sourceEntity)));
			}

			if (const Sprite2DComponent* sprite = sourceScene.GetSprite2DComponent(sourceEntity))
			{
				targetScene.EnsureSprite2DComponent(targetEntity) = *sprite;
				static_cast<void>(targetScene.SetComponentEnabled<Sprite2DComponent>(targetEntity, sourceScene.IsComponentEnabled<Sprite2DComponent>(sourceEntity)));
			}

			if (const UiElementComponent* ui = sourceScene.GetUiElementComponent(sourceEntity))
			{
				targetScene.EnsureUiElementComponent(targetEntity) = *ui;
				static_cast<void>(targetScene.SetComponentEnabled<UiElementComponent>(targetEntity, sourceScene.IsComponentEnabled<UiElementComponent>(sourceEntity)));
			}

			if (const AudioSourceComponent* audio = sourceScene.GetAudioSourceComponent(sourceEntity))
			{
				targetScene.EnsureAudioSourceComponent(targetEntity) = *audio;
				static_cast<void>(targetScene.SetComponentEnabled<AudioSourceComponent>(targetEntity, sourceScene.IsComponentEnabled<AudioSourceComponent>(sourceEntity)));
			}

			if (const NavigationAgentComponent* navigation = sourceScene.GetNavigationAgentComponent(sourceEntity))
			{
				targetScene.EnsureNavigationAgentComponent(targetEntity) = *navigation;
				static_cast<void>(targetScene.SetComponentEnabled<NavigationAgentComponent>(targetEntity, sourceScene.IsComponentEnabled<NavigationAgentComponent>(sourceEntity)));
			}

			if (const NetworkIdentityComponent* network = sourceScene.GetNetworkIdentityComponent(sourceEntity))
			{
				targetScene.EnsureNetworkIdentityComponent(targetEntity) = *network;
				static_cast<void>(targetScene.SetComponentEnabled<NetworkIdentityComponent>(targetEntity, sourceScene.IsComponentEnabled<NetworkIdentityComponent>(sourceEntity)));
			}
		}

		void ApplyLoadedEntityToScene(
			const LoadedSceneEntity& loadedEntity,
			Scene& targetScene,
			EntityId targetEntity,
			const PrefabSaveOptions& options)
		{
			if (loadedEntity.HasTransform)
			{
				TransformComponent& transform = targetScene.EnsureTransformComponent(targetEntity);
				transform.LocalTransform = loadedEntity.Transform;
				transform.WorldTransform = loadedEntity.Transform;
			}
			if (loadedEntity.HasEditorState)
			{
				targetScene.EnsureEditorStateComponent(targetEntity) = loadedEntity.EditorState;
			}
			if (loadedEntity.HasHierarchy)
			{
				SceneHierarchyComponent& hierarchy = targetScene.EnsureHierarchyComponent(targetEntity);
				hierarchy.Parent = InvalidEntityId;
				hierarchy.Expanded = loadedEntity.HierarchyExpanded;
			}
			if (loadedEntity.HasMesh)
			{
				MeshComponent& mesh = targetScene.EnsureMeshComponent(targetEntity);
				mesh.Asset = std::make_unique<Asset::StaticMeshAsset>();
				mesh.Asset->SourcePath = loadedEntity.MeshAssetPath;
				mesh.Asset->PrimitiveKind = loadedEntity.PrimitiveKind;
				mesh.Asset->Materials.assign(loadedEntity.MaterialOverrides.begin(), loadedEntity.MaterialOverrides.end());
				static_cast<void>(targetScene.SetMeshEnabled(targetEntity, loadedEntity.MeshEnabled));
			}
			if (loadedEntity.HasCamera)
			{
				targetScene.EnsureCameraComponent(targetEntity) = loadedEntity.Camera;
				static_cast<void>(targetScene.SetCameraEnabled(targetEntity, loadedEntity.CameraEnabled));
			}
			if (loadedEntity.HasLight)
			{
				targetScene.EnsureLightComponent(targetEntity) = loadedEntity.Light;
				static_cast<void>(targetScene.SetLightEnabled(targetEntity, loadedEntity.LightEnabled));
			}
			if (loadedEntity.HasAnimator)
			{
				targetScene.EnsureAnimatorComponent(targetEntity) = loadedEntity.Animator;
				static_cast<void>(targetScene.SetAnimatorEnabled(targetEntity, loadedEntity.AnimatorEnabled));
			}
			if (loadedEntity.HasRigidBody)
			{
				targetScene.EnsureRigidBodyComponent(targetEntity) = loadedEntity.RigidBody;
				static_cast<void>(targetScene.SetRigidBodyEnabled(targetEntity, loadedEntity.RigidBodyEnabled));
			}
			if (loadedEntity.HasCollider)
			{
				targetScene.EnsureColliderComponent(targetEntity) = loadedEntity.Collider;
				static_cast<void>(targetScene.SetColliderEnabled(targetEntity, loadedEntity.ColliderEnabled));
			}
			if (loadedEntity.HasPhysicsMaterial)
			{
				targetScene.EnsurePhysicsMaterialComponent(targetEntity) = loadedEntity.PhysicsMaterial;
				static_cast<void>(targetScene.SetPhysicsMaterialEnabled(targetEntity, loadedEntity.PhysicsMaterialEnabled));
			}
			if (options.IncludePrefabInstanceComponent && loadedEntity.HasPrefabInstance)
			{
				targetScene.EnsurePrefabInstanceComponent(targetEntity) = loadedEntity.PrefabInstance;
				static_cast<void>(targetScene.SetComponentEnabled<PrefabInstanceComponent>(targetEntity, loadedEntity.PrefabInstanceEnabled));
			}
			if (loadedEntity.HasSceneReference)
			{
				targetScene.EnsureSceneReferenceComponent(targetEntity) = loadedEntity.SceneReference;
				static_cast<void>(targetScene.SetComponentEnabled<SceneReferenceComponent>(targetEntity, loadedEntity.SceneReferenceEnabled));
			}
			if (loadedEntity.HasScript)
			{
				targetScene.EnsureScriptComponent(targetEntity) = loadedEntity.Script;
				static_cast<void>(targetScene.SetComponentEnabled<ScriptComponent>(targetEntity, loadedEntity.ScriptEnabled));
			}
			if (loadedEntity.HasSprite2D)
			{
				targetScene.EnsureSprite2DComponent(targetEntity) = loadedEntity.Sprite2D;
				static_cast<void>(targetScene.SetComponentEnabled<Sprite2DComponent>(targetEntity, loadedEntity.Sprite2DEnabled));
			}
			if (loadedEntity.HasUiElement)
			{
				targetScene.EnsureUiElementComponent(targetEntity) = loadedEntity.UiElement;
				static_cast<void>(targetScene.SetComponentEnabled<UiElementComponent>(targetEntity, loadedEntity.UiElementEnabled));
			}
			if (loadedEntity.HasAudioSource)
			{
				targetScene.EnsureAudioSourceComponent(targetEntity) = loadedEntity.AudioSource;
				static_cast<void>(targetScene.SetComponentEnabled<AudioSourceComponent>(targetEntity, loadedEntity.AudioSourceEnabled));
			}
			if (loadedEntity.HasNavigationAgent)
			{
				targetScene.EnsureNavigationAgentComponent(targetEntity) = loadedEntity.NavigationAgent;
				static_cast<void>(targetScene.SetComponentEnabled<NavigationAgentComponent>(targetEntity, loadedEntity.NavigationAgentEnabled));
			}
			if (loadedEntity.HasNetworkIdentity)
			{
				targetScene.EnsureNetworkIdentityComponent(targetEntity) = loadedEntity.NetworkIdentity;
				static_cast<void>(targetScene.SetComponentEnabled<NetworkIdentityComponent>(targetEntity, loadedEntity.NetworkIdentityEnabled));
			}
		}
	}

	bool PrefabService::SaveEntityAsPrefab(
		const Scene& scene,
		EntityId entityId,
		const Projects::ProjectDescriptor& project,
		const std::filesystem::path& prefabPath,
		const PrefabSaveOptions& options,
		std::string& errorMessage)
	{
		if (!scene.ContainsEntity(entityId))
		{
			errorMessage = "Prefab source entity does not exist.";
			return false;
		}

		Scene prefabScene;
		const std::string* name = scene.GetEntityName(entityId);
		const EntityId prefabEntity = prefabScene.CreateEntity(name && !name->empty() ? *name : "PrefabRoot");
		CopyOptionalComponents(scene, entityId, prefabScene, prefabEntity, options);

		SceneRenderState dummyRenderState;
		return ScenePersistenceService::SaveScene(
			prefabScene,
			dummyRenderState,
			project,
			prefabPath,
			options.AmbientColor,
			options.AmbientIntensity,
			options.Exposure,
			options.Skybox,
			errorMessage);
	}

	bool PrefabService::SaveLoadedEntityAsPrefab(
		const LoadedSceneEntity& entity,
		const Projects::ProjectDescriptor& project,
		const std::filesystem::path& prefabPath,
		const PrefabSaveOptions& options,
		std::string& errorMessage)
	{
		Scene prefabScene;
		const EntityId prefabEntity = prefabScene.CreateEntity(entity.Name.empty() ? "PrefabRoot" : entity.Name);
		ApplyLoadedEntityToScene(entity, prefabScene, prefabEntity, options);

		SceneRenderState dummyRenderState;
		return ScenePersistenceService::SaveScene(
			prefabScene,
			dummyRenderState,
			project,
			prefabPath,
			options.AmbientColor,
			options.AmbientIntensity,
			options.Exposure,
			options.Skybox,
			errorMessage);
	}

	LoadPrefabResult PrefabService::LoadPrefab(
		const std::filesystem::path& prefabPath,
		const Projects::ProjectDescriptor& project)
	{
		LoadPrefabResult result;
		LoadSceneResult sceneResult = ScenePersistenceService::LoadScene(prefabPath, project);
		if (!sceneResult.Success)
		{
			result.ErrorMessage = sceneResult.ErrorMessage;
			return result;
		}
		if (sceneResult.Entities.empty())
		{
			result.ErrorMessage = "Prefab has no root entity.";
			return result;
		}

		result.Root = std::move(sceneResult.Entities.front());
		result.Success = true;
		return result;
	}
}
