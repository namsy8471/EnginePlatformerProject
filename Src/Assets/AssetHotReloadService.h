#pragma once

#include "Scene/Scene.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Asset
{
	struct AssetHotReloadEvent
	{
		std::filesystem::path SourcePath;
		std::filesystem::path ChangedPath;
	};

	class AssetHotReloadService
	{
	public:
		AssetHotReloadService();
		~AssetHotReloadService();

		void WatchLoadedAsset(const std::filesystem::path& sourcePath, std::vector<std::filesystem::path> dependentPaths);
		void UnwatchLoadedAsset(const std::filesystem::path& sourcePath);
		void Clear();
		[[nodiscard]] std::vector<AssetHotReloadEvent> ConsumeEvents();

	private:
		struct WatchRecord
		{
			std::filesystem::path SourcePath;
			std::vector<std::filesystem::path> Paths;
			std::unordered_map<std::wstring, std::filesystem::file_time_type> LastWriteTimes;
		};

		void WorkerLoop(std::stop_token stopToken);
		[[nodiscard]] static std::filesystem::path NormalizePath(const std::filesystem::path& path);

		std::mutex m_Mutex;
		std::unordered_map<std::wstring, WatchRecord> m_Watches;
		std::vector<AssetHotReloadEvent> m_PendingEvents;
		std::jthread m_Worker;
	};
}
