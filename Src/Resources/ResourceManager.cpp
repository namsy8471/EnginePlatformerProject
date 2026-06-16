#include "Resources/ResourceManager.h"

#include "Assets/AssetFileSystem.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace Resources
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

		[[nodiscard]] bool IsRegularFile(const std::filesystem::directory_entry& entry)
		{
			std::error_code errorCode;
			return entry.is_regular_file(errorCode);
		}

		[[nodiscard]] bool IsDirectory(const std::filesystem::directory_entry& entry)
		{
			std::error_code errorCode;
			return entry.is_directory(errorCode);
		}

		[[nodiscard]] ResourceKind ResourceKindFromAssetKind(Asset::AssetFileKind kind, const std::filesystem::path& path)
		{
			switch (kind)
			{
			case Asset::AssetFileKind::Model:
				return ResourceKind::Mesh;
			case Asset::AssetFileKind::Image:
				return ResourceKind::Texture;
			case Asset::AssetFileKind::Text:
			{
				const std::string extension = ToLower(path.extension().string());
				if (extension == ".scene")
				{
					return ResourceKind::Scene;
				}
				if (extension == ".material")
				{
					return ResourceKind::Material;
				}
				return ResourceKind::Script;
			}
			case Asset::AssetFileKind::Source:
			{
				const std::string extension = ToLower(path.extension().string());
				if (extension == ".hlsl" || extension == ".vert" || extension == ".frag")
				{
					return ResourceKind::Shader;
				}
				return ResourceKind::Script;
			}
			case Asset::AssetFileKind::Other:
			{
				const std::string extension = ToLower(path.extension().string());
				if (extension == ".scene")
				{
					return ResourceKind::Scene;
				}
				if (extension == ".material")
				{
					return ResourceKind::Material;
				}
				if (extension == ".engineproject")
				{
					return ResourceKind::Script;
				}
				if (extension == ".mesh")
				{
					return ResourceKind::Mesh;
				}
				if (extension == ".skeleton")
				{
					return ResourceKind::Skeleton;
				}
				if (extension == ".anim" || extension == ".animation")
				{
					return ResourceKind::Animation;
				}
				if (extension == ".wav" || extension == ".ogg" || extension == ".mp3")
				{
					return ResourceKind::Audio;
				}
				return ResourceKind::Other;
			}
			case Asset::AssetFileKind::Directory:
			default:
				return ResourceKind::Unknown;
			}
		}

		void CollectEntriesRecursive(
			const Asset::AssetFileEntry& entry,
			std::string_view groupName,
			std::vector<ResourceDeclaration>& declarations)
		{
			if (entry.Kind == Asset::AssetFileKind::Directory)
			{
				for (const Asset::AssetFileEntry& child : entry.Children)
				{
					CollectEntriesRecursive(child, groupName, declarations);
				}
				return;
			}

			const ResourceKind kind = ResourceKindFromAssetKind(entry.Kind, entry.Path);
			if (kind == ResourceKind::Unknown)
			{
				return;
			}

			declarations.push_back(ResourceDeclaration{
				.Name = entry.Name,
				.SourcePath = entry.Path,
				.Kind = kind,
				.GroupName = std::string(groupName),
				.SizeBytes = entry.SizeBytes
				});
		}

		[[nodiscard]] uintmax_t FileSizeOrZero(const std::filesystem::path& path)
		{
			std::error_code errorCode;
			const uintmax_t size = std::filesystem::file_size(path, errorCode);
			return errorCode ? 0 : size;
		}
	}

	const char* ToString(ResourceKind kind) noexcept
	{
		switch (kind)
		{
		case ResourceKind::Mesh:
			return "Mesh";
		case ResourceKind::Texture:
			return "Texture";
		case ResourceKind::Material:
			return "Material";
		case ResourceKind::Skeleton:
			return "Skeleton";
		case ResourceKind::Animation:
			return "Animation";
		case ResourceKind::Scene:
			return "Scene";
		case ResourceKind::Shader:
			return "Shader";
		case ResourceKind::Script:
			return "Script";
		case ResourceKind::Audio:
			return "Audio";
		case ResourceKind::Other:
			return "Other";
		case ResourceKind::Unknown:
		default:
			return "Unknown";
		}
	}

	const char* ToString(ResourceLoadState state) noexcept
	{
		switch (state)
		{
		case ResourceLoadState::Declared:
			return "Declared";
		case ResourceLoadState::Preparing:
			return "Preparing";
		case ResourceLoadState::Prepared:
			return "Prepared";
		case ResourceLoadState::Loaded:
			return "Loaded";
		case ResourceLoadState::Failed:
			return "Failed";
		case ResourceLoadState::Unloaded:
			return "Unloaded";
		case ResourceLoadState::Undefined:
		default:
			return "Undefined";
		}
	}

	void ResourceManager::Reset()
	{
		m_Records.clear();
		m_Groups.clear();
		m_PathIndex.clear();
		m_NameIndex.clear();
		++m_Generation;
		m_NextId = 1;
	}

	void ResourceManager::SetRootPath(std::filesystem::path rootPath)
	{
		m_RootPath = std::move(rootPath).lexically_normal();
	}

	const std::filesystem::path& ResourceManager::GetRootPath() const noexcept
	{
		return m_RootPath;
	}

	ResourceGroup& ResourceManager::EnsureGroup(std::string_view groupName)
	{
		const std::string key = groupName.empty() ? std::string("General") : std::string(groupName);
		auto [it, inserted] = m_Groups.try_emplace(key);
		if (inserted)
		{
			it->second.Name = key;
		}
		return it->second;
	}

	const ResourceGroup* ResourceManager::FindGroup(std::string_view groupName) const
	{
		const auto it = m_Groups.find(groupName.empty() ? std::string("General") : std::string(groupName));
		return it != m_Groups.end() ? &it->second : nullptr;
	}

	void ResourceManager::AddResourceLocation(std::string_view groupName, std::filesystem::path path, std::string type, bool recursive)
	{
		ResourceGroup& group = EnsureGroup(groupName);
		ResourceLocation location{
			.Path = std::move(path).lexically_normal(),
			.Type = std::move(type),
			.Recursive = recursive
		};

		const std::string locationKey = ToLower(location.Path.generic_string());
		const auto alreadyExists = std::ranges::any_of(group.Locations, [&locationKey](const ResourceLocation& existing)
			{
				return ToLower(existing.Path.generic_string()) == locationKey;
			});
		if (!alreadyExists)
		{
			group.Locations.push_back(std::move(location));
		}
	}

	ResourceHandle ResourceManager::DeclareResource(ResourceDeclaration declaration)
	{
		if (declaration.Kind == ResourceKind::Unknown)
		{
			declaration.Kind = ResourceKind::Other;
		}
		if (declaration.GroupName.empty())
		{
			declaration.GroupName = "General";
		}
		if (declaration.Name.empty())
		{
			declaration.Name = declaration.SourcePath.filename().string();
		}

		const std::string pathKey = MakePathKey(declaration.SourcePath);
		if (const auto existingIt = m_PathIndex.find(pathKey); existingIt != m_PathIndex.end())
		{
			if (ResourceRecord* record = Get(existingIt->second))
			{
				record->Name = declaration.Name;
				record->Kind = declaration.Kind;
				record->GroupName = declaration.GroupName;
				record->Priority = declaration.Priority;
				record->SizeBytes = declaration.SizeBytes;
				record->Streamable = declaration.Streamable;
				record->SourcePath = declaration.SourcePath.lexically_normal();
				m_NameIndex[MakeNameKey(record->GroupName, record->Name)] = record->Handle;
				return record->Handle;
			}
		}

		ResourceRecord record;
		record.Handle = AllocateHandle(declaration.Kind);
		record.Name = declaration.Name;
		record.SourcePath = declaration.SourcePath.lexically_normal();
		record.Kind = declaration.Kind;
		record.GroupName = declaration.GroupName;
		record.Priority = declaration.Priority;
		record.SizeBytes = declaration.SizeBytes;
		record.Streamable = declaration.Streamable;

		const ResourceHandle handle = record.Handle;
		m_Records.emplace(handle.Id, std::move(record));
		m_PathIndex[pathKey] = handle;
		m_NameIndex[MakeNameKey(declaration.GroupName, declaration.Name)] = handle;

		ResourceGroup& group = EnsureGroup(declaration.GroupName);
		group.Resources.push_back(handle);
		return handle;
	}

	size_t ResourceManager::DeclareResourcesFromDirectory(const std::filesystem::path& directory, std::string_view groupName)
	{
		std::error_code errorCode;
		if (!std::filesystem::is_directory(directory, errorCode))
		{
			return 0;
		}

		size_t declaredCount = 0;
		for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, errorCode), end;
			it != end && !errorCode;
			it.increment(errorCode))
		{
			if (IsDirectory(*it))
			{
				continue;
			}
			if (!IsRegularFile(*it))
			{
				continue;
			}

			const std::filesystem::path path = it->path().lexically_normal();
			const ResourceKind kind = ResourceKindFromAssetKind(Asset::ClassifyAssetPath(path), path);
			if (kind == ResourceKind::Unknown)
			{
				continue;
			}

			static_cast<void>(DeclareResource(ResourceDeclaration{
				.Name = path.filename().string(),
				.SourcePath = path,
				.Kind = kind,
				.GroupName = std::string(groupName),
				.SizeBytes = FileSizeOrZero(path)
				}));
			++declaredCount;
		}

		if (ResourceGroup* group = &EnsureGroup(groupName))
		{
			group->Initialized = true;
		}
		return declaredCount;
	}

	size_t ResourceManager::DeclareResourcesFromSnapshot(const Asset::AssetFileSnapshot& snapshot, std::string_view groupName)
	{
		if (!snapshot.RootExists)
		{
			return 0;
		}

		std::vector<ResourceDeclaration> declarations;
		for (const Asset::AssetFileEntry& child : snapshot.Children)
		{
			CollectEntriesRecursive(child, groupName, declarations);
		}

		for (ResourceDeclaration& declaration : declarations)
		{
			static_cast<void>(DeclareResource(std::move(declaration)));
		}

		if (ResourceGroup* group = &EnsureGroup(groupName))
		{
			group->Initialized = true;
		}
		return declarations.size();
	}

	const ResourceRecord* ResourceManager::Get(ResourceHandle handle) const
	{
		const auto it = m_Records.find(handle.Id);
		if (it == m_Records.end() || it->second.Handle.Generation != handle.Generation)
		{
			return nullptr;
		}
		return &it->second;
	}

	ResourceRecord* ResourceManager::Get(ResourceHandle handle)
	{
		const auto it = m_Records.find(handle.Id);
		if (it == m_Records.end() || it->second.Handle.Generation != handle.Generation)
		{
			return nullptr;
		}
		return &it->second;
	}

	std::optional<ResourceHandle> ResourceManager::FindByPath(const std::filesystem::path& sourcePath) const
	{
		const auto it = m_PathIndex.find(MakePathKey(sourcePath));
		if (it == m_PathIndex.end())
		{
			return std::nullopt;
		}
		return it->second;
	}

	std::optional<ResourceHandle> ResourceManager::FindByName(std::string_view groupName, std::string_view name) const
	{
		const auto it = m_NameIndex.find(MakeNameKey(groupName, name));
		if (it == m_NameIndex.end())
		{
			return std::nullopt;
		}
		return it->second;
	}

	std::vector<ResourceRecord> ResourceManager::GetRecordsSnapshot() const
	{
		std::vector<ResourceRecord> records;
		records.reserve(m_Records.size());
		for (const auto& [id, record] : m_Records)
		{
			(void)id;
			records.push_back(record);
		}
		std::ranges::sort(records, [](const ResourceRecord& lhs, const ResourceRecord& rhs)
			{
				if (lhs.GroupName != rhs.GroupName)
				{
					return lhs.GroupName < rhs.GroupName;
				}
				return lhs.Name < rhs.Name;
			});
		return records;
	}

	void ResourceManager::AddRef(ResourceHandle handle)
	{
		if (ResourceRecord* record = Get(handle))
		{
			++record->RefCount;
		}
	}

	void ResourceManager::Release(ResourceHandle handle)
	{
		if (ResourceRecord* record = Get(handle); record && record->RefCount > 0)
		{
			--record->RefCount;
		}
	}

	void ResourceManager::Touch(ResourceHandle handle, uint64_t frameIndex)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->LastAccessFrame = frameIndex;
		}
	}

	void ResourceManager::MarkPreparing(ResourceHandle handle)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->State = ResourceLoadState::Preparing;
			record->ErrorMessage.clear();
		}
	}

	void ResourceManager::MarkPrepared(ResourceHandle handle)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->State = ResourceLoadState::Prepared;
			record->ErrorMessage.clear();
		}
	}

	void ResourceManager::MarkLoaded(ResourceHandle handle, uintmax_t loadedBytes)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->State = ResourceLoadState::Loaded;
			record->SizeBytes = loadedBytes != 0 ? loadedBytes : record->SizeBytes;
			record->ErrorMessage.clear();
		}
	}

	void ResourceManager::MarkFailed(ResourceHandle handle, std::string errorMessage)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->State = ResourceLoadState::Failed;
			record->ErrorMessage = std::move(errorMessage);
		}
	}

	void ResourceManager::MarkUnloaded(ResourceHandle handle)
	{
		if (ResourceRecord* record = Get(handle))
		{
			record->State = ResourceLoadState::Unloaded;
		}
	}

	ResourceManagerStats ResourceManager::GetStats() const
	{
		ResourceManagerStats stats;
		stats.GroupCount = m_Groups.size();
		stats.ResourceCount = m_Records.size();
		for (const auto& [id, record] : m_Records)
		{
			(void)id;
			stats.DeclaredBytes += record.SizeBytes;
			switch (record.State)
			{
			case ResourceLoadState::Declared:
			case ResourceLoadState::Unloaded:
				++stats.DeclaredCount;
				break;
			case ResourceLoadState::Prepared:
			case ResourceLoadState::Preparing:
				++stats.PreparedCount;
				break;
			case ResourceLoadState::Loaded:
				++stats.LoadedCount;
				stats.LoadedBytes += record.SizeBytes;
				break;
			case ResourceLoadState::Failed:
				++stats.FailedCount;
				break;
			case ResourceLoadState::Undefined:
			default:
				break;
			}
		}
		return stats;
	}

	ResourceHandle ResourceManager::AllocateHandle(ResourceKind kind)
	{
		return ResourceHandle{
			.Id = m_NextId++,
			.Generation = m_Generation,
			.Kind = kind
		};
	}

	std::string ResourceManager::MakePathKey(const std::filesystem::path& path) const
	{
		std::filesystem::path normalized = path;
		if (normalized.is_relative())
		{
			normalized = m_RootPath / normalized;
		}
		return ToLower(normalized.lexically_normal().generic_string());
	}

	std::string ResourceManager::MakeNameKey(std::string_view groupName, std::string_view name)
	{
		std::string key;
		key.reserve(groupName.size() + name.size() + 1);
		key.append(groupName.empty() ? "General" : groupName);
		key.push_back(':');
		key.append(name);
		return ToLower(std::move(key));
	}
}
