#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Asset
{
	enum class AssetFileKind : uint8_t
	{
		Directory,
		Model,
		Image,
		Text,
		Source,
		Other
	};

	struct AssetFileEntry
	{
		std::filesystem::path Path;
		std::string Name;
		std::string Extension;
		AssetFileKind Kind = AssetFileKind::Other;
		uintmax_t SizeBytes = 0;
		std::vector<AssetFileEntry> Children;
	};

	struct AssetFileSnapshot
	{
		std::filesystem::path RootPath;
		bool RootExists = false;
		std::string Status;
		std::vector<AssetFileEntry> Children;
	};

	class AssetFileSystem
	{
	public:
		AssetFileSystem();
		~AssetFileSystem();

		void SetRootPath(std::filesystem::path rootPath);
		void RequestRefresh() noexcept;
		[[nodiscard]] std::shared_ptr<const AssetFileSnapshot> GetSnapshot() const;
		[[nodiscard]] bool IsRefreshInProgress() const noexcept;

	private:
		void WorkerLoop(std::stop_token stopToken);
		[[nodiscard]] AssetFileSnapshot BuildSnapshot() const;

		mutable std::mutex m_Mutex;
		std::filesystem::path m_RootPath = "Assets";
		std::shared_ptr<AssetFileSnapshot> m_Snapshot;
		std::jthread m_Worker;
		std::atomic_bool m_RefreshRequested = true;
		std::atomic_bool m_RefreshInProgress = false;
	};

	[[nodiscard]] AssetFileKind ClassifyAssetPath(const std::filesystem::path& path);
	[[nodiscard]] bool IsModelAssetPath(const std::filesystem::path& path);
}
