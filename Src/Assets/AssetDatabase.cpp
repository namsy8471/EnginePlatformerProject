#include "AssetDatabase.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace Asset
{
	namespace
	{
		struct ProjectAssetScanEntry
		{
			std::filesystem::path Path;
			AssetFileKind Kind = AssetFileKind::Other;
			uintmax_t SizeBytes = 0;
			uint64_t LastWriteTimeTicks = 0;
		};

		struct AssetTokenRecord
		{
			std::filesystem::path Path;
			std::vector<std::string> Tokens;
		};

		constexpr uintmax_t kMaxReferenceScanBytes = 4ull * 1024ull * 1024ull;

		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
		}

		[[nodiscard]] std::string NormalizeSearchText(std::string value)
		{
			std::replace(value.begin(), value.end(), '\\', '/');
			return ToLower(std::move(value));
		}

		[[nodiscard]] std::string PathKey(const std::filesystem::path& path)
		{
			return NormalizeSearchText(path.lexically_normal().string());
		}

		[[nodiscard]] bool SamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
		{
			return PathKey(lhs) == PathKey(rhs);
		}

		[[nodiscard]] bool IsReadableAssetFile(AssetFileKind kind) noexcept
		{
			return kind == AssetFileKind::Text || kind == AssetFileKind::Source;
		}

		[[nodiscard]] bool IsRelativeInsideRoot(const std::filesystem::path& relativePath)
		{
			return !relativePath.empty() && !relativePath.is_absolute() && *relativePath.begin() != "..";
		}

		void CollectAssetFiles(const std::vector<AssetFileEntry>& entries, std::vector<ProjectAssetScanEntry>& files)
		{
			for (const AssetFileEntry& entry : entries)
			{
				if (entry.Kind == AssetFileKind::Directory)
				{
					CollectAssetFiles(entry.Children, files);
					continue;
				}

				files.push_back(ProjectAssetScanEntry{
					.Path = entry.Path,
					.Kind = entry.Kind,
					.SizeBytes = entry.SizeBytes,
					.LastWriteTimeTicks = entry.LastWriteTimeTicks
				});
			}
		}

		void HashCombine(uint64_t& seed, uint64_t value) noexcept
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		}

		[[nodiscard]] std::string ToStoredProjectPath(const std::filesystem::path& path, const std::filesystem::path& projectRootPath)
		{
			if (path.empty())
			{
				return {};
			}

			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(path.lexically_normal(), projectRootPath.lexically_normal(), errorCode);
			if (!errorCode && IsRelativeInsideRoot(relativePath))
			{
				return relativePath.generic_string();
			}
			return path.lexically_normal().generic_string();
		}

		[[nodiscard]] std::filesystem::path ResolveStoredProjectPath(std::string_view storedPath, const std::filesystem::path& projectRootPath)
		{
			if (storedPath.empty())
			{
				return {};
			}

			std::filesystem::path path(storedPath);
			if (path.is_absolute())
			{
				return path.lexically_normal();
			}
			return (projectRootPath / path).lexically_normal();
		}

		[[nodiscard]] std::vector<std::string> BuildAssetReferenceTokens(
			const std::filesystem::path& assetPath,
			const std::filesystem::path& assetRootPath)
		{
			std::vector<std::string> tokens;
			const auto addToken = [&tokens](std::string token)
			{
				token = NormalizeSearchText(std::move(token));
				if (token.empty() || token == "." || token.size() < 4)
				{
					return;
				}
				if (std::ranges::find(tokens, token) == tokens.end())
				{
					tokens.push_back(std::move(token));
				}
			};

			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(assetPath.lexically_normal(), assetRootPath.lexically_normal(), errorCode);
			if (!errorCode && IsRelativeInsideRoot(relativePath))
			{
				const std::string relativeGeneric = relativePath.generic_string();
				addToken(relativeGeneric);
				const std::string rootName = assetRootPath.filename().generic_string();
				if (!rootName.empty())
				{
					addToken(rootName + "/" + relativeGeneric);
				}
			}

			addToken(assetPath.lexically_normal().generic_string());
			return tokens;
		}

		[[nodiscard]] bool TryReadScanText(const ProjectAssetScanEntry& file, std::string& text)
		{
			text.clear();
			if (!IsReadableAssetFile(file.Kind) || file.SizeBytes > kMaxReferenceScanBytes)
			{
				return false;
			}

			std::ifstream stream(file.Path, std::ios::binary);
			if (!stream)
			{
				return false;
			}

			text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			text = NormalizeSearchText(std::move(text));
			return true;
		}

		[[nodiscard]] bool ContainsAnyToken(std::string_view normalizedText, const std::vector<std::string>& tokens)
		{
			return std::ranges::any_of(tokens, [normalizedText](const std::string& token)
				{
					return normalizedText.find(token) != std::string_view::npos;
				});
		}
	}

	uint64_t AssetDatabase::ComputeReferenceIndexSignature(const AssetFileSnapshot& snapshot)
	{
		std::vector<ProjectAssetScanEntry> files;
		CollectAssetFiles(snapshot.Children, files);
		std::ranges::sort(files, [](const ProjectAssetScanEntry& lhs, const ProjectAssetScanEntry& rhs)
			{
				return PathKey(lhs.Path) < PathKey(rhs.Path);
			});

		uint64_t signature = snapshot.RootExists ? 1469598103934665603ull : 1099511628211ull;
		HashCombine(signature, static_cast<uint64_t>(std::hash<std::string>{}(PathKey(snapshot.RootPath))));
		for (const ProjectAssetScanEntry& file : files)
		{
			HashCombine(signature, static_cast<uint64_t>(std::hash<std::string>{}(PathKey(file.Path))));
			HashCombine(signature, static_cast<uint64_t>(file.Kind));
			HashCombine(signature, static_cast<uint64_t>(file.SizeBytes));
			HashCombine(signature, file.LastWriteTimeTicks);
		}
		return signature;
	}

	AssetReferenceIndex AssetDatabase::LoadOrBuildReferenceIndex(
		const AssetFileSnapshot& snapshot,
		const std::filesystem::path& projectRootPath,
		bool forceRebuild)
	{
		const std::filesystem::path assetRootPath = snapshot.RootPath.lexically_normal();
		const uint64_t signature = ComputeReferenceIndexSignature(snapshot);
		AssetReferenceIndex index;
		if (!forceRebuild && TryLoadReferenceIndex(projectRootPath, assetRootPath, signature, index))
		{
			return index;
		}

		index.AssetRootPath = assetRootPath;
		index.Signature = signature;

		std::vector<ProjectAssetScanEntry> files;
		CollectAssetFiles(snapshot.Children, files);
		std::ranges::sort(files, [](const ProjectAssetScanEntry& lhs, const ProjectAssetScanEntry& rhs)
			{
				return PathKey(lhs.Path) < PathKey(rhs.Path);
			});

		std::vector<AssetTokenRecord> tokenRecords;
		tokenRecords.reserve(files.size());
		index.Entries.reserve(files.size());
		for (const ProjectAssetScanEntry& file : files)
		{
			index.Entries.push_back(AssetReferenceIndexEntry{ .Path = file.Path.lexically_normal() });
			tokenRecords.push_back(AssetTokenRecord{
				.Path = file.Path.lexically_normal(),
				.Tokens = BuildAssetReferenceTokens(file.Path, assetRootPath)
			});
		}

		for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex)
		{
			const ProjectAssetScanEntry& file = files[fileIndex];
			std::string normalizedText;
			if (!TryReadScanText(file, normalizedText))
			{
				if (IsReadableAssetFile(file.Kind))
				{
					++index.SkippedFiles;
				}
				continue;
			}

			++index.ScannedFiles;
			std::vector<std::filesystem::path>& dependencies = index.Entries[fileIndex].Dependencies;
			for (const AssetTokenRecord& tokenRecord : tokenRecords)
			{
				if (SamePath(file.Path, tokenRecord.Path))
				{
					continue;
				}

				if (ContainsAnyToken(normalizedText, tokenRecord.Tokens))
				{
					dependencies.push_back(tokenRecord.Path.lexically_normal());
				}
			}

			std::ranges::sort(dependencies);
			dependencies.erase(std::ranges::unique(dependencies).begin(), dependencies.end());
		}

		SaveReferenceIndex(projectRootPath, index);
		return index;
	}

	std::vector<std::filesystem::path> AssetDatabase::GetDependencies(
		const AssetReferenceIndex& index,
		const std::filesystem::path& assetPath)
	{
		for (const AssetReferenceIndexEntry& entry : index.Entries)
		{
			if (SamePath(entry.Path, assetPath))
			{
				return entry.Dependencies;
			}
		}
		return {};
	}

	std::vector<std::filesystem::path> AssetDatabase::GetReferences(
		const AssetReferenceIndex& index,
		const std::filesystem::path& assetPath)
	{
		std::vector<std::filesystem::path> references;
		for (const AssetReferenceIndexEntry& entry : index.Entries)
		{
			const bool referencesAsset = std::ranges::any_of(entry.Dependencies, [&assetPath](const std::filesystem::path& dependencyPath)
				{
					return SamePath(dependencyPath, assetPath);
				});
			if (referencesAsset)
			{
				references.push_back(entry.Path.lexically_normal());
			}
		}

		std::ranges::sort(references);
		references.erase(std::ranges::unique(references).begin(), references.end());
		return references;
	}

	std::filesystem::path AssetDatabase::GetReferenceIndexPath(const std::filesystem::path& projectRootPath)
	{
		return projectRootPath / "Library" / "EditorAssetIndex.json";
	}

	bool AssetDatabase::TryLoadReferenceIndex(
		const std::filesystem::path& projectRootPath,
		const std::filesystem::path& assetRootPath,
		uint64_t signature,
		AssetReferenceIndex& index)
	{
		std::ifstream file(GetReferenceIndexPath(projectRootPath), std::ios::binary);
		if (!file)
		{
			return false;
		}

		const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		rapidjson::Document document;
		document.Parse(json.c_str(), json.size());
		if (document.HasParseError() || !document.IsObject())
		{
			return false;
		}

		const auto signatureIt = document.FindMember("signature");
		if (signatureIt == document.MemberEnd() || !signatureIt->value.IsUint64() || signatureIt->value.GetUint64() != signature)
		{
			return false;
		}
		const auto entriesIt = document.FindMember("entries");
		if (entriesIt == document.MemberEnd() || !entriesIt->value.IsArray())
		{
			return false;
		}

		AssetReferenceIndex loadedIndex;
		loadedIndex.AssetRootPath = assetRootPath.lexically_normal();
		loadedIndex.Signature = signature;
		loadedIndex.LoadedFromDisk = true;
		loadedIndex.ScannedFiles = document.HasMember("scannedFiles") && document["scannedFiles"].IsUint64()
			? static_cast<size_t>(document["scannedFiles"].GetUint64())
			: 0;
		loadedIndex.SkippedFiles = document.HasMember("skippedFiles") && document["skippedFiles"].IsUint64()
			? static_cast<size_t>(document["skippedFiles"].GetUint64())
			: 0;
		loadedIndex.Entries.reserve(entriesIt->value.Size());

		for (const rapidjson::Value& entryValue : entriesIt->value.GetArray())
		{
			if (!entryValue.IsObject() || !entryValue.HasMember("path") || !entryValue["path"].IsString())
			{
				continue;
			}

			AssetReferenceIndexEntry entry;
			entry.Path = ResolveStoredProjectPath(entryValue["path"].GetString(), projectRootPath);
			if (entry.Path.empty())
			{
				continue;
			}

			if (entryValue.HasMember("dependencies") && entryValue["dependencies"].IsArray())
			{
				for (const rapidjson::Value& dependencyValue : entryValue["dependencies"].GetArray())
				{
					if (!dependencyValue.IsString())
					{
						continue;
					}

					std::filesystem::path dependencyPath = ResolveStoredProjectPath(dependencyValue.GetString(), projectRootPath);
					if (!dependencyPath.empty())
					{
						entry.Dependencies.push_back(dependencyPath.lexically_normal());
					}
				}
			}

			std::ranges::sort(entry.Dependencies);
			entry.Dependencies.erase(std::ranges::unique(entry.Dependencies).begin(), entry.Dependencies.end());
			loadedIndex.Entries.push_back(std::move(entry));
		}

		if (loadedIndex.ScannedFiles == 0)
		{
			loadedIndex.ScannedFiles = loadedIndex.Entries.size();
		}
		index = std::move(loadedIndex);
		return true;
	}

	void AssetDatabase::SaveReferenceIndex(const std::filesystem::path& projectRootPath, const AssetReferenceIndex& index)
	{
		if (projectRootPath.empty())
		{
			return;
		}

		std::error_code errorCode;
		const std::filesystem::path indexPath = GetReferenceIndexPath(projectRootPath);
		std::filesystem::create_directories(indexPath.parent_path(), errorCode);
		if (errorCode)
		{
			return;
		}

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(1);
		writer.Key("signature");
		writer.Uint64(index.Signature);
		writer.Key("scannedFiles");
		writer.Uint64(static_cast<uint64_t>(index.ScannedFiles));
		writer.Key("skippedFiles");
		writer.Uint64(static_cast<uint64_t>(index.SkippedFiles));
		writer.Key("entries");
		writer.StartArray();
		for (const AssetReferenceIndexEntry& entry : index.Entries)
		{
			writer.StartObject();
			writer.Key("path");
			const std::string storedPath = ToStoredProjectPath(entry.Path, projectRootPath);
			writer.String(storedPath.c_str(), static_cast<rapidjson::SizeType>(storedPath.size()));
			writer.Key("dependencies");
			writer.StartArray();
			for (const std::filesystem::path& dependencyPath : entry.Dependencies)
			{
				const std::string storedDependencyPath = ToStoredProjectPath(dependencyPath, projectRootPath);
				writer.String(storedDependencyPath.c_str(), static_cast<rapidjson::SizeType>(storedDependencyPath.size()));
			}
			writer.EndArray();
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();

		std::ofstream file(indexPath, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return;
		}
		file.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
	}
}
