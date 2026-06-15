#include "AssetFileSystem.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <system_error>
#include <thread>

namespace Asset
{
	namespace
	{
		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
		}

		[[nodiscard]] bool IsDirectory(const std::filesystem::directory_entry& entry)
		{
			std::error_code errorCode;
			return entry.is_directory(errorCode);
		}

		[[nodiscard]] bool IsRegularFile(const std::filesystem::directory_entry& entry)
		{
			std::error_code errorCode;
			return entry.is_regular_file(errorCode);
		}

		[[nodiscard]] AssetFileEntry BuildEntryRecursive(const std::filesystem::directory_entry& entry)
		{
			AssetFileEntry result;
			result.Path = entry.path().lexically_normal();
			result.Name = result.Path.filename().string();
			result.Extension = ToLower(result.Path.extension().string());
			result.Kind = ClassifyAssetPath(result.Path);

			std::error_code errorCode;
			if (IsRegularFile(entry))
			{
				result.SizeBytes = entry.file_size(errorCode);
				if (errorCode)
				{
					result.SizeBytes = 0;
				}
			}

			if (!IsDirectory(entry))
			{
				return result;
			}

			std::vector<std::filesystem::directory_entry> children;
			for (std::filesystem::directory_iterator it(result.Path, std::filesystem::directory_options::skip_permission_denied, errorCode), end;
				it != end && !errorCode;
				it.increment(errorCode))
			{
				children.push_back(*it);
			}

			std::sort(children.begin(), children.end(), [](const auto& lhs, const auto& rhs)
				{
					const bool lhsDirectory = IsDirectory(lhs);
					const bool rhsDirectory = IsDirectory(rhs);
					if (lhsDirectory != rhsDirectory)
					{
						return lhsDirectory;
					}
					return ToLower(lhs.path().filename().string()) < ToLower(rhs.path().filename().string());
				});

			result.Children.reserve(children.size());
			for (const auto& child : children)
			{
				result.Children.push_back(BuildEntryRecursive(child));
			}

			return result;
		}
	}

	AssetFileSystem::AssetFileSystem()
	{
		m_Worker = std::jthread([this](std::stop_token stopToken)
			{
				WorkerLoop(stopToken);
			});
	}

	AssetFileSystem::~AssetFileSystem() = default;

	void AssetFileSystem::SetRootPath(std::filesystem::path rootPath)
	{
		{
			std::scoped_lock lock(m_Mutex);
			m_RootPath = std::move(rootPath);
		}
		RequestRefresh();
	}

	void AssetFileSystem::RequestRefresh() noexcept
	{
		m_RefreshRequested.store(true, std::memory_order_release);
	}

	std::shared_ptr<const AssetFileSnapshot> AssetFileSystem::GetSnapshot() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_Snapshot;
	}

	bool AssetFileSystem::IsRefreshInProgress() const noexcept
	{
		return m_RefreshInProgress.load(std::memory_order_acquire);
	}

	void AssetFileSystem::WorkerLoop(std::stop_token stopToken)
	{
		while (!stopToken.stop_requested())
		{
			if (!m_RefreshRequested.exchange(false, std::memory_order_acq_rel))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}

			m_RefreshInProgress.store(true, std::memory_order_release);
			auto snapshot = std::make_shared<AssetFileSnapshot>(BuildSnapshot());
			{
				std::scoped_lock lock(m_Mutex);
				m_Snapshot = std::move(snapshot);
			}
			m_RefreshInProgress.store(false, std::memory_order_release);
		}
	}

	AssetFileSnapshot AssetFileSystem::BuildSnapshot() const
	{
		std::filesystem::path rootPath;
		{
			std::scoped_lock lock(m_Mutex);
			rootPath = m_RootPath;
		}

		AssetFileSnapshot snapshot;
		snapshot.RootPath = rootPath.lexically_normal();

		std::error_code errorCode;
		snapshot.RootExists = std::filesystem::is_directory(snapshot.RootPath, errorCode);
		if (!snapshot.RootExists)
		{
			snapshot.Status = "Assets folder was not found.";
			return snapshot;
		}

		std::vector<std::filesystem::directory_entry> entries;
		for (std::filesystem::directory_iterator it(snapshot.RootPath, std::filesystem::directory_options::skip_permission_denied, errorCode), end;
			it != end && !errorCode;
			it.increment(errorCode))
		{
			entries.push_back(*it);
		}

		std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
			{
				const bool lhsDirectory = IsDirectory(lhs);
				const bool rhsDirectory = IsDirectory(rhs);
				if (lhsDirectory != rhsDirectory)
				{
					return lhsDirectory;
				}
				return ToLower(lhs.path().filename().string()) < ToLower(rhs.path().filename().string());
			});

		snapshot.Children.reserve(entries.size());
		for (const auto& entry : entries)
		{
			snapshot.Children.push_back(BuildEntryRecursive(entry));
		}

		snapshot.Status = "Ready";
		return snapshot;
	}

	AssetFileKind ClassifyAssetPath(const std::filesystem::path& path)
	{
		std::error_code errorCode;
		if (std::filesystem::is_directory(path, errorCode))
		{
			return AssetFileKind::Directory;
		}

		const std::string extension = ToLower(path.extension().string());
		if (extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb")
		{
			return AssetFileKind::Model;
		}
		if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".bmp" || extension == ".dds")
		{
			return AssetFileKind::Image;
		}
		if (extension == ".txt" || extension == ".md" || extension == ".json")
		{
			return AssetFileKind::Text;
		}
		if (extension == ".h" || extension == ".hpp" || extension == ".cpp" || extension == ".hlsl" || extension == ".vert" || extension == ".frag")
		{
			return AssetFileKind::Source;
		}
		return AssetFileKind::Other;
	}

	bool IsModelAssetPath(const std::filesystem::path& path)
	{
		return ClassifyAssetPath(path) == AssetFileKind::Model;
	}
}
