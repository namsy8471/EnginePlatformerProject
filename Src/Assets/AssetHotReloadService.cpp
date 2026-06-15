#include "AssetHotReloadService.h"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace Asset
{
	namespace
	{
		[[nodiscard]] std::filesystem::file_time_type ReadLastWriteTime(const std::filesystem::path& path)
		{
			std::error_code errorCode;
			return std::filesystem::last_write_time(path, errorCode);
		}
	}

	AssetHotReloadService::AssetHotReloadService()
	{
		m_Worker = std::jthread([this](std::stop_token stopToken)
			{
				WorkerLoop(stopToken);
			});
	}

	AssetHotReloadService::~AssetHotReloadService() = default;

	void AssetHotReloadService::WatchLoadedAsset(const std::filesystem::path& sourcePath, std::vector<std::filesystem::path> dependentPaths)
	{
		const std::filesystem::path normalizedSourcePath = NormalizePath(sourcePath);
		std::vector<std::filesystem::path> paths;
		paths.reserve(dependentPaths.size() + 1);
		paths.push_back(normalizedSourcePath);
		for (const auto& dependentPath : dependentPaths)
		{
			if (dependentPath.empty())
			{
				continue;
			}

			const std::filesystem::path normalizedDependentPath = NormalizePath(dependentPath);
			if (std::ranges::find(paths, normalizedDependentPath) == paths.end())
			{
				paths.push_back(normalizedDependentPath);
			}
		}

		WatchRecord record;
		record.SourcePath = normalizedSourcePath;
		record.Paths = std::move(paths);
		for (const auto& path : record.Paths)
		{
			record.LastWriteTimes[path.wstring()] = ReadLastWriteTime(path);
		}

		std::scoped_lock lock(m_Mutex);
		m_Watches[normalizedSourcePath.wstring()] = std::move(record);
	}

	void AssetHotReloadService::UnwatchLoadedAsset(const std::filesystem::path& sourcePath)
	{
		const std::filesystem::path normalizedSourcePath = NormalizePath(sourcePath);
		std::scoped_lock lock(m_Mutex);
		m_Watches.erase(normalizedSourcePath.wstring());
		std::erase_if(m_PendingEvents, [&normalizedSourcePath](const AssetHotReloadEvent& event)
			{
				return event.SourcePath == normalizedSourcePath;
			});
	}

	void AssetHotReloadService::Clear()
	{
		std::scoped_lock lock(m_Mutex);
		m_Watches.clear();
		m_PendingEvents.clear();
	}

	std::vector<AssetHotReloadEvent> AssetHotReloadService::ConsumeEvents()
	{
		std::scoped_lock lock(m_Mutex);
		std::vector<AssetHotReloadEvent> events;
		events.swap(m_PendingEvents);
		return events;
	}

	void AssetHotReloadService::WorkerLoop(std::stop_token stopToken)
	{
		while (!stopToken.stop_requested())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));

			std::vector<AssetHotReloadEvent> events;
			{
				std::scoped_lock lock(m_Mutex);
				for (auto& [key, record] : m_Watches)
				{
					(void)key;
					for (const auto& path : record.Paths)
					{
						const std::filesystem::file_time_type currentWriteTime = ReadLastWriteTime(path);
						auto& previousWriteTime = record.LastWriteTimes[path.wstring()];
						if (previousWriteTime != std::filesystem::file_time_type{} && currentWriteTime != previousWriteTime)
						{
							previousWriteTime = currentWriteTime;
							events.push_back({ record.SourcePath, path });
						}
						else if (previousWriteTime == std::filesystem::file_time_type{})
						{
							previousWriteTime = currentWriteTime;
						}
					}
				}

				m_PendingEvents.insert(m_PendingEvents.end(), events.begin(), events.end());
			}
		}
	}

	std::filesystem::path AssetHotReloadService::NormalizePath(const std::filesystem::path& path)
	{
		std::error_code errorCode;
		const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
		return (errorCode ? path : canonicalPath).lexically_normal();
	}
}
