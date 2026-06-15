#pragma once

#include "Scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Asset
{
	struct RuntimeAssetRecord
	{
		std::filesystem::path SourcePath;
		std::vector<EntityId> Entities;
		std::vector<std::filesystem::path> TexturePaths;
		uint64_t Generation = 0;
		std::string Status;
	};

	class RuntimeAssetRegistry
	{
	public:
		[[nodiscard]] uint64_t NextGeneration(const std::filesystem::path& sourcePath);
		void RegisterEntity(const std::filesystem::path& sourcePath, EntityId entityId, std::vector<std::filesystem::path> texturePaths, std::string status);
		[[nodiscard]] std::vector<std::filesystem::path> UnregisterEntity(EntityId entityId);
		void Clear();
		void UpdateStatus(const std::filesystem::path& sourcePath, std::string status);
		[[nodiscard]] uint64_t GetGeneration(const std::filesystem::path& sourcePath) const;
		[[nodiscard]] std::vector<EntityId> GetEntities(const std::filesystem::path& sourcePath) const;
		[[nodiscard]] std::optional<std::filesystem::path> FindSourcePathForEntity(EntityId entityId) const;
		[[nodiscard]] std::vector<RuntimeAssetRecord> GetRecords() const;

	private:
		[[nodiscard]] static std::filesystem::path NormalizePath(const std::filesystem::path& path);

		std::unordered_map<std::wstring, RuntimeAssetRecord> m_Records;
	};
}
