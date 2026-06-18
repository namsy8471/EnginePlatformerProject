#pragma once

#include "Assets/StaticMesh.h"
#include "Math/Transform.h"
#include "Scene/Scene.h"

#include <DirectXMath.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Asset
{
	struct AssetImportPlacement
	{
		DirectX::XMFLOAT3 CameraPosition = { 0.0f, 0.0f, -6.0f };
		DirectX::XMFLOAT3 CameraForward = { 0.0f, 0.0f, 1.0f };
		bool HasPlacement = true;
	};

	struct AssetImportRestoreState
	{
		bool HasTargetEntity = false;
		EntityId TargetEntity = InvalidEntityId;
		uint64_t RestoreGeneration = 0;
		std::string EntityName;
		Math::Transform LocalTransform = Math::Transform::Identity();
		bool MeshEnabled = true;
		std::vector<StaticMeshMaterial> MaterialOverrides;
		bool HasAnimator = false;
		bool AnimatorEnabled = true;
		AnimatorComponent Animator;
	};

	struct AssetImportRequest
	{
		std::filesystem::path SourcePath;
		uint64_t Generation = 0;
		uint64_t SceneGeneration = 0;
		bool IsReload = false;
		AssetImportPlacement Placement;
		AssetImportRestoreState Restore;
	};

	struct AssetImportResult
	{
		std::filesystem::path SourcePath;
		uint64_t Generation = 0;
		uint64_t SceneGeneration = 0;
		bool IsReload = false;
		bool Success = false;
		bool IsAnimated = false;
		bool GenerateColliders = false;
		std::string ErrorMessage;
		std::vector<std::string> Diagnostics;
		AssetImportPlacement Placement;
		AssetImportRestoreState Restore;
		std::unique_ptr<StaticMeshAsset> Mesh;
		std::vector<CpuMaterialTexture> MaterialTextures;
		std::vector<bool> MaterialTransparency;
		DirectX::XMFLOAT3 LocalMin = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 LocalMax = { 0.0f, 0.0f, 0.0f };
	};

	class AssetImportService
	{
	public:
		AssetImportService();
		~AssetImportService();

		void Shutdown();
		void Queue(AssetImportRequest request);
		[[nodiscard]] bool TryPopCompleted(AssetImportResult& result);
		[[nodiscard]] bool HasPendingWork() const noexcept;

	private:
		void WorkerLoop(std::stop_token stopToken);
		[[nodiscard]] static AssetImportResult ImportModel(const AssetImportRequest& request);

		mutable std::mutex m_Mutex;
		std::condition_variable_any m_Condition;
		std::queue<AssetImportRequest> m_PendingRequests;
		std::queue<AssetImportResult> m_CompletedResults;
		std::vector<std::jthread> m_Workers;
		std::atomic_uint32_t m_ActiveWorkerCount = 0;
	};
}
