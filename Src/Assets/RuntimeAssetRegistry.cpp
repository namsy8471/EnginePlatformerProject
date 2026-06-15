#include "RuntimeAssetRegistry.h"

#include <algorithm>
#include <system_error>

namespace Asset
{
	std::filesystem::path RuntimeAssetRegistry::NormalizePath(const std::filesystem::path& path)
	{
		std::error_code errorCode;
		const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
		return (errorCode ? path : canonicalPath).lexically_normal();
	}

	uint64_t RuntimeAssetRegistry::NextGeneration(const std::filesystem::path& sourcePath)
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		RuntimeAssetRecord& record = m_Records[normalizedPath.wstring()];
		record.SourcePath = normalizedPath;
		++record.Generation;
		return record.Generation;
	}

	void RuntimeAssetRegistry::RegisterEntity(const std::filesystem::path& sourcePath, EntityId entityId, std::vector<std::filesystem::path> texturePaths, std::string status)
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		RuntimeAssetRecord& record = m_Records[normalizedPath.wstring()];
		record.SourcePath = normalizedPath;
		record.Status = std::move(status);
		record.TexturePaths = std::move(texturePaths);

		if (std::ranges::find(record.Entities, entityId) == record.Entities.end())
		{
			record.Entities.push_back(entityId);
		}
	}

	std::vector<std::filesystem::path> RuntimeAssetRegistry::UnregisterEntity(EntityId entityId)
	{
		std::vector<std::filesystem::path> removedSourcePaths;
		for (auto recordIt = m_Records.begin(); recordIt != m_Records.end();)
		{
			RuntimeAssetRecord& record = recordIt->second;
			std::erase(record.Entities, entityId);
			if (record.Entities.empty())
			{
				removedSourcePaths.push_back(record.SourcePath);
				recordIt = m_Records.erase(recordIt);
				continue;
			}

			++recordIt;
		}

		return removedSourcePaths;
	}

	void RuntimeAssetRegistry::UpdateStatus(const std::filesystem::path& sourcePath, std::string status)
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		RuntimeAssetRecord& record = m_Records[normalizedPath.wstring()];
		record.SourcePath = normalizedPath;
		record.Status = std::move(status);
	}

	uint64_t RuntimeAssetRegistry::GetGeneration(const std::filesystem::path& sourcePath) const
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		const auto recordIt = m_Records.find(normalizedPath.wstring());
		return recordIt != m_Records.end() ? recordIt->second.Generation : 0;
	}

	std::vector<EntityId> RuntimeAssetRegistry::GetEntities(const std::filesystem::path& sourcePath) const
	{
		const std::filesystem::path normalizedPath = NormalizePath(sourcePath);
		const auto recordIt = m_Records.find(normalizedPath.wstring());
		return recordIt != m_Records.end() ? recordIt->second.Entities : std::vector<EntityId>{};
	}

	std::optional<std::filesystem::path> RuntimeAssetRegistry::FindSourcePathForEntity(EntityId entityId) const
	{
		for (const auto& [key, record] : m_Records)
		{
			(void)key;
			if (std::ranges::find(record.Entities, entityId) != record.Entities.end())
			{
				return record.SourcePath;
			}
		}

		return std::nullopt;
	}

	std::vector<RuntimeAssetRecord> RuntimeAssetRegistry::GetRecords() const
	{
		std::vector<RuntimeAssetRecord> records;
		records.reserve(m_Records.size());
		for (const auto& [key, record] : m_Records)
		{
			(void)key;
			records.push_back(record);
		}
		return records;
	}
}
