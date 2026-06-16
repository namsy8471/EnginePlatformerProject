#pragma once

#include "Resources/ResourceTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Asset
{
	struct AssetFileSnapshot;
}

namespace Resources
{
	class ResourceManager
	{
	public:
		void Reset();
		void SetRootPath(std::filesystem::path rootPath);
		[[nodiscard]] const std::filesystem::path& GetRootPath() const noexcept;

		ResourceGroup& EnsureGroup(std::string_view groupName);
		[[nodiscard]] const ResourceGroup* FindGroup(std::string_view groupName) const;
		void AddResourceLocation(std::string_view groupName, std::filesystem::path path, std::string type = "FileSystem", bool recursive = true);

		[[nodiscard]] ResourceHandle DeclareResource(ResourceDeclaration declaration);
		[[nodiscard]] size_t DeclareResourcesFromDirectory(const std::filesystem::path& directory, std::string_view groupName);
		[[nodiscard]] size_t DeclareResourcesFromSnapshot(const Asset::AssetFileSnapshot& snapshot, std::string_view groupName);

		[[nodiscard]] const ResourceRecord* Get(ResourceHandle handle) const;
		[[nodiscard]] ResourceRecord* Get(ResourceHandle handle);
		[[nodiscard]] std::optional<ResourceHandle> FindByPath(const std::filesystem::path& sourcePath) const;
		[[nodiscard]] std::optional<ResourceHandle> FindByName(std::string_view groupName, std::string_view name) const;
		[[nodiscard]] std::vector<ResourceRecord> GetRecordsSnapshot() const;

		void AddRef(ResourceHandle handle);
		void Release(ResourceHandle handle);
		void Touch(ResourceHandle handle, uint64_t frameIndex);
		void MarkPreparing(ResourceHandle handle);
		void MarkPrepared(ResourceHandle handle);
		void MarkLoaded(ResourceHandle handle, uintmax_t loadedBytes);
		void MarkFailed(ResourceHandle handle, std::string errorMessage);
		void MarkUnloaded(ResourceHandle handle);

		[[nodiscard]] ResourceManagerStats GetStats() const;

	private:
		[[nodiscard]] ResourceHandle AllocateHandle(ResourceKind kind);
		[[nodiscard]] std::string MakePathKey(const std::filesystem::path& path) const;
		[[nodiscard]] static std::string MakeNameKey(std::string_view groupName, std::string_view name);

		std::filesystem::path m_RootPath = "Assets";
		std::unordered_map<uint64_t, ResourceRecord> m_Records;
		std::unordered_map<std::string, ResourceGroup> m_Groups;
		std::unordered_map<std::string, ResourceHandle> m_PathIndex;
		std::unordered_map<std::string, ResourceHandle> m_NameIndex;
		uint64_t m_NextId = 1;
		uint32_t m_Generation = 1;
	};
}
