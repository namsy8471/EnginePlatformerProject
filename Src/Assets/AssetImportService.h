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

	struct AssetImportRequest
	{
		std::filesystem::path SourcePath;
		uint64_t Generation = 0;
		bool IsReload = false;
		AssetImportPlacement Placement;
	};

	struct AssetImportResult
	{
		std::filesystem::path SourcePath;
		uint64_t Generation = 0;
		bool IsReload = false;
		bool Success = false;
		bool IsAnimated = false;
		std::string ErrorMessage;
		std::vector<std::string> Diagnostics;
		AssetImportPlacement Placement;
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
