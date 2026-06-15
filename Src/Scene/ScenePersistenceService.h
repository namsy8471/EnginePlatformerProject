#pragma once

#include "Projects/ProjectService.h"
#include "Scene/Scene.h"
#include "Scene/SceneRenderState.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ScenePersistence
{
	struct LoadedSceneEntity
	{
		std::string Name = "Entity";
		bool HasTransform = false;
		Math::Transform Transform = Math::Transform::Identity();
		bool HasMesh = false;
		bool MeshEnabled = true;
		std::filesystem::path MeshAssetPath;
		Asset::PrimitiveMeshKind PrimitiveKind = Asset::PrimitiveMeshKind::None;
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
	};

	struct LoadSceneResult
	{
		bool Success = false;
		std::string ErrorMessage;
		std::string Name;
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
			std::string& errorMessage);

		[[nodiscard]] static LoadSceneResult LoadScene(
			const std::filesystem::path& scenePath,
			const Projects::ProjectDescriptor& project);
	};
}
