#pragma once

#include "Assets/AssetFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Asset
{
	struct AssetReferenceIndexEntry
	{
		std::filesystem::path Path;
		std::vector<std::filesystem::path> Dependencies;
	};

	struct AssetReferenceIndex
	{
		std::filesystem::path AssetRootPath;
		uint64_t Signature = 0;
		std::vector<AssetReferenceIndexEntry> Entries;
		size_t ScannedFiles = 0;
		size_t SkippedFiles = 0;
		bool LoadedFromDisk = false;
	};

	class AssetDatabase
	{
	public:
		[[nodiscard]] static uint64_t ComputeReferenceIndexSignature(const AssetFileSnapshot& snapshot);
		[[nodiscard]] static AssetReferenceIndex LoadOrBuildReferenceIndex(
			const AssetFileSnapshot& snapshot,
			const std::filesystem::path& projectRootPath,
			bool forceRebuild);
		[[nodiscard]] static std::vector<std::filesystem::path> GetDependencies(
			const AssetReferenceIndex& index,
			const std::filesystem::path& assetPath);
		[[nodiscard]] static std::vector<std::filesystem::path> GetReferences(
			const AssetReferenceIndex& index,
			const std::filesystem::path& assetPath);
		[[nodiscard]] static std::filesystem::path GetReferenceIndexPath(const std::filesystem::path& projectRootPath);

	private:
		[[nodiscard]] static bool TryLoadReferenceIndex(
			const std::filesystem::path& projectRootPath,
			const std::filesystem::path& assetRootPath,
			uint64_t signature,
			AssetReferenceIndex& index);
		static void SaveReferenceIndex(const std::filesystem::path& projectRootPath, const AssetReferenceIndex& index);
	};
}
