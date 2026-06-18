#pragma once

#include "Projects/ProjectService.h"
#include "Rendering/Sky/SkyboxSettings.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderState.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace ScenePersistence
{
	struct LoadedSceneEntity
	{
		std::string Name = "Entity";
		bool HasEditorState = false;
		EditorStateComponent EditorState;
		bool HasHierarchy = false;
		size_t ParentIndex = static_cast<size_t>(-1);
		EntityId ParentEntityId = InvalidEntityId;
		bool HierarchyExpanded = true;
		bool HasTransform = false;
		Math::Transform Transform = Math::Transform::Identity();
		bool HasMesh = false;
		bool MeshEnabled = true;
		std::filesystem::path MeshAssetPath;
		Asset::PrimitiveMeshKind PrimitiveKind = Asset::PrimitiveMeshKind::None;
		std::vector<Asset::StaticMeshMaterial> MaterialOverrides;
		bool HasCamera = false;
		bool CameraEnabled = true;
		CameraComponent Camera;
		bool HasLight = false;
		bool LightEnabled = true;
		LightComponent Light;
		bool HasAnimator = false;
		bool AnimatorEnabled = true;
		AnimatorComponent Animator;
		bool HasRigidBody = false;
		bool RigidBodyEnabled = true;
		RigidBodyComponent RigidBody;
		bool HasCollider = false;
		bool ColliderEnabled = true;
		ColliderComponent Collider;
		bool HasPhysicsMaterial = false;
		bool PhysicsMaterialEnabled = true;
		PhysicsMaterialComponent PhysicsMaterial;
		bool HasPrefabInstance = false;
		bool PrefabInstanceEnabled = true;
		PrefabInstanceComponent PrefabInstance;
		bool HasSceneReference = false;
		bool SceneReferenceEnabled = true;
		SceneReferenceComponent SceneReference;
		bool HasScript = false;
		bool ScriptEnabled = true;
		ScriptComponent Script;
		bool HasSprite2D = false;
		bool Sprite2DEnabled = true;
		Sprite2DComponent Sprite2D;
		bool HasUiElement = false;
		bool UiElementEnabled = true;
		UiElementComponent UiElement;
		bool HasAudioSource = false;
		bool AudioSourceEnabled = true;
		AudioSourceComponent AudioSource;
		bool HasNavigationAgent = false;
		bool NavigationAgentEnabled = true;
		NavigationAgentComponent NavigationAgent;
		bool HasNetworkIdentity = false;
		bool NetworkIdentityEnabled = true;
		NetworkIdentityComponent NetworkIdentity;
	};

	struct LoadSceneResult
	{
		bool Success = false;
		std::string ErrorMessage;
		std::string Name;
		DirectX::XMFLOAT3 AmbientColor = { 0.62f, 0.68f, 0.78f };
		float AmbientIntensity = 0.35f;
		float Exposure = 1.0f;
		Rendering::SkyboxSettings Skybox;
		std::vector<LoadedSceneEntity> Entities;
	};

	class ScenePersistenceService
	{
	public:
		[[nodiscard]] static bool SaveScene(
			const Scene& scene,
			const SceneRenderState& renderState,
			const Projects::ProjectDescriptor& project,
			const std::filesystem::path& scenePath,
			const DirectX::XMFLOAT3& ambientColor,
			float ambientIntensity,
			float exposure,
			const Rendering::SkyboxSettings& skybox,
			std::string& errorMessage,
			const std::unordered_set<EntityId>* excludedEntities = nullptr);

		[[nodiscard]] static LoadSceneResult LoadScene(
			const std::filesystem::path& scenePath,
			const Projects::ProjectDescriptor& project);
	};
}
