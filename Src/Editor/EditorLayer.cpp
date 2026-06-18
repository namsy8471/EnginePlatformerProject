#include "EditorLayer.h"

#include "Assets/AssetDatabase.h"
#include "Assets/AssimpModelLoader.h"
#include "Assets/PrimitiveMeshFactory.h"
#include "Assets/TextureMatching.h"
#include "Reflection/SceneComponentReflection.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Editor
{
	namespace
	{
		constexpr const char* kAssetPathPayload = "ENGINE_ASSET_PATH";
		constexpr const char* kHierarchyEntityPayload = "ENGINE_HIERARCHY_ENTITY";

		struct CommandPaletteItem
		{
			std::string Label;
			std::string Detail;
			CommandPaletteScope Scope = CommandPaletteScope::Commands;
			bool Enabled = true;
			std::function<void()> Execute;
		};

		[[nodiscard]] constexpr const char* GraphicsApiName(GraphicsAPI api) noexcept
		{
			switch (api)
			{
			case GraphicsAPI::DirectX12:
				return "DirectX12";
			case GraphicsAPI::Vulkan:
				return "Vulkan";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] constexpr const char* SampleModeName(Samples::Benchmark::SampleMode sampleMode) noexcept
		{
			return Samples::Benchmark::ToString(sampleMode).data();
		}

		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
		}

		[[nodiscard]] std::string TrimCopy(std::string_view value)
		{
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
			{
				value.remove_prefix(1);
			}
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
			{
				value.remove_suffix(1);
			}
			return std::string(value);
		}

		[[nodiscard]] constexpr std::string_view ProjectCreateAssetKindName(ProjectCreateAssetKind kind) noexcept
		{
			switch (kind)
			{
			case ProjectCreateAssetKind::Folder:
				return "Folder";
			case ProjectCreateAssetKind::Scene:
				return "Scene";
			case ProjectCreateAssetKind::Material:
				return "Material";
			case ProjectCreateAssetKind::Skybox:
				return "Skybox";
			case ProjectCreateAssetKind::Script:
				return "Script";
			case ProjectCreateAssetKind::Prefab:
				return "Prefab";
			default:
				return "Asset";
			}
		}

		[[nodiscard]] constexpr std::string_view ProjectCreateDefaultName(ProjectCreateAssetKind kind) noexcept
		{
			switch (kind)
			{
			case ProjectCreateAssetKind::Folder:
				return "New Folder";
			case ProjectCreateAssetKind::Scene:
				return "New Scene";
			case ProjectCreateAssetKind::Material:
				return "New Material";
			case ProjectCreateAssetKind::Skybox:
				return "New Skybox";
			case ProjectCreateAssetKind::Script:
				return "NewScript";
			case ProjectCreateAssetKind::Prefab:
				return "New Prefab";
			default:
				return "New Asset";
			}
		}

		[[nodiscard]] constexpr std::string_view ProjectCreateExtension(ProjectCreateAssetKind kind) noexcept
		{
			switch (kind)
			{
			case ProjectCreateAssetKind::Scene:
				return ".scene";
			case ProjectCreateAssetKind::Material:
				return ".material";
			case ProjectCreateAssetKind::Skybox:
				return ".skybox";
			case ProjectCreateAssetKind::Script:
				return ".cpp";
			case ProjectCreateAssetKind::Prefab:
				return ".prefab";
			case ProjectCreateAssetKind::Folder:
			default:
				return "";
			}
		}

		[[nodiscard]] std::vector<std::string> TokenizeCommand(std::string_view command)
		{
			std::vector<std::string> tokens;
			std::string current;
			bool inQuotes = false;
			for (const char character : command)
			{
				if (character == '"')
				{
					inQuotes = !inQuotes;
					continue;
				}

				if (!inQuotes && std::isspace(static_cast<unsigned char>(character)))
				{
					if (!current.empty())
					{
						tokens.push_back(std::move(current));
						current.clear();
					}
					continue;
				}

				current.push_back(character);
			}

			if (!current.empty())
			{
				tokens.push_back(std::move(current));
			}
			return tokens;
		}

		[[nodiscard]] std::string QuoteShellArgument(std::string_view text)
		{
			std::string quoted;
			quoted.reserve(text.size() + 2);
			quoted.push_back('"');
			for (const char character : text)
			{
				if (character == '"')
				{
					quoted.push_back('\\');
				}
				quoted.push_back(character);
			}
			quoted.push_back('"');
			return quoted;
		}

		[[nodiscard]] std::string QuoteCommandArgument(const std::filesystem::path& path)
		{
			const std::string text = path.string();
			return QuoteShellArgument(std::string_view(text));
		}

		[[nodiscard]] std::string SourceControlPathFromPorcelainLine(std::string_view line)
		{
			if (line.size() < 4 || line.rfind("## ", 0) == 0)
			{
				return {};
			}

			std::string path(line.substr(3));
			const std::string renameMarker = " -> ";
			if (const size_t renameIndex = path.rfind(renameMarker); renameIndex != std::string::npos)
			{
				path = path.substr(renameIndex + renameMarker.size());
			}
			return TrimCopy(path);
		}

		[[nodiscard]] bool SourceControlLineHasStagedChange(std::string_view line) noexcept
		{
			return line.size() >= 2 && line[0] != ' ' && line[0] != '?';
		}

		[[nodiscard]] bool SourceControlLineHasUnstagedChange(std::string_view line) noexcept
		{
			return line.size() >= 2 && (line[1] != ' ' || (line[0] == '?' && line[1] == '?'));
		}

		[[nodiscard]] bool SourceControlLineIsConflict(std::string_view line) noexcept
		{
			if (line.size() < 2)
			{
				return false;
			}

			const char indexStatus = line[0];
			const char workTreeStatus = line[1];
			return indexStatus == 'U' ||
				workTreeStatus == 'U' ||
				(indexStatus == 'A' && workTreeStatus == 'A') ||
				(indexStatus == 'D' && workTreeStatus == 'D') ||
				(indexStatus == 'A' && workTreeStatus == 'D') ||
				(indexStatus == 'D' && workTreeStatus == 'A');
		}

		[[nodiscard]] int ParseSourceControlCount(std::string_view text)
		{
			int value = 0;
			for (const char character : text)
			{
				if (character < '0' || character > '9')
				{
					continue;
				}
				value = value * 10 + static_cast<int>(character - '0');
			}
			return value;
		}

		template <typename Summary>
		void ParseSourceControlBranchLine(std::string_view line, Summary& summary)
		{
			if (line.rfind("## ", 0) != 0)
			{
				return;
			}

			std::string branchLine(line.substr(3));
			std::string detail;
			if (const size_t detailBegin = branchLine.find(" ["); detailBegin != std::string::npos)
			{
				detail = branchLine.substr(detailBegin + 2);
				if (!detail.empty() && detail.back() == ']')
				{
					detail.pop_back();
				}
				branchLine = branchLine.substr(0, detailBegin);
			}

			if (const size_t upstreamSeparator = branchLine.find("..."); upstreamSeparator != std::string::npos)
			{
				summary.Branch = branchLine.substr(0, upstreamSeparator);
				summary.Upstream = branchLine.substr(upstreamSeparator + 3);
				summary.HasUpstream = !summary.Upstream.empty();
			}
			else
			{
				summary.Branch = branchLine.empty() ? "<detached>" : branchLine;
				summary.Upstream.clear();
				summary.HasUpstream = false;
			}

			if (summary.Branch.empty())
			{
				summary.Branch = "<detached>";
			}

			if (!detail.empty())
			{
				size_t begin = 0;
				while (begin < detail.size())
				{
					size_t end = detail.find(',', begin);
					if (end == std::string::npos)
					{
						end = detail.size();
					}
					const std::string token = TrimCopy(std::string_view(detail).substr(begin, end - begin));
					if (token.rfind("ahead", 0) == 0)
					{
						summary.AheadCount = ParseSourceControlCount(token);
					}
					else if (token.rfind("behind", 0) == 0)
					{
						summary.BehindCount = ParseSourceControlCount(token);
					}
					begin = end + 1;
				}
			}
		}

		[[nodiscard]] std::vector<std::string> ReadCommandLines(const std::string& command, int& exitCode)
		{
			std::vector<std::string> lines;
			exitCode = -1;
			FILE* pipe = _popen(command.c_str(), "r");
			if (!pipe)
			{
				return lines;
			}

			std::array<char, 1024> buffer = {};
			while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
			{
				std::string line(buffer.data());
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
				{
					line.pop_back();
				}
				lines.push_back(std::move(line));
			}

			exitCode = _pclose(pipe);
			return lines;
		}

		template <size_t BufferSize>
		[[nodiscard]] std::string_view TextFilter(const std::array<char, BufferSize>& buffer) noexcept
		{
			return std::string_view(buffer.data());
		}

		[[nodiscard]] bool ContainsCaseInsensitive(std::string_view text, std::string_view filter)
		{
			if (filter.empty())
			{
				return true;
			}

			return ToLower(std::string(text)).find(ToLower(std::string(filter))) != std::string::npos;
		}

		[[nodiscard]] constexpr const char* HierarchyQuickFilterName(HierarchyQuickFilter filter) noexcept
		{
			switch (filter)
			{
			case HierarchyQuickFilter::All:
				return "All";
			case HierarchyQuickFilter::Mesh:
				return "Mesh";
			case HierarchyQuickFilter::Camera:
				return "Camera";
			case HierarchyQuickFilter::Light:
				return "Light";
			case HierarchyQuickFilter::Physics:
				return "Physics";
			case HierarchyQuickFilter::Script:
				return "Script";
			case HierarchyQuickFilter::Hidden:
				return "Hidden";
			case HierarchyQuickFilter::Locked:
				return "Locked";
			case HierarchyQuickFilter::Nested:
				return "Nested";
			default:
				return "All";
			}
		}

		[[nodiscard]] constexpr const char* ProjectQuickFilterName(ProjectQuickFilter filter) noexcept
		{
			switch (filter)
			{
			case ProjectQuickFilter::All:
				return "All";
			case ProjectQuickFilter::Favorites:
				return "Favorites";
			case ProjectQuickFilter::Folders:
				return "Folders";
			case ProjectQuickFilter::Models:
				return "Models";
			case ProjectQuickFilter::Images:
				return "Images";
			case ProjectQuickFilter::Scenes:
				return "Scenes";
			case ProjectQuickFilter::Materials:
				return "Materials";
			case ProjectQuickFilter::Prefabs:
				return "Prefabs";
			case ProjectQuickFilter::Source:
				return "Source";
			case ProjectQuickFilter::Text:
				return "Text";
			default:
				return "All";
			}
		}

		[[nodiscard]] constexpr std::string_view ProjectQuickFilterToken(ProjectQuickFilter filter) noexcept
		{
			switch (filter)
			{
			case ProjectQuickFilter::Favorites:
				return "favorites";
			case ProjectQuickFilter::Folders:
				return "folders";
			case ProjectQuickFilter::Models:
				return "models";
			case ProjectQuickFilter::Images:
				return "images";
			case ProjectQuickFilter::Scenes:
				return "scenes";
			case ProjectQuickFilter::Materials:
				return "materials";
			case ProjectQuickFilter::Prefabs:
				return "prefabs";
			case ProjectQuickFilter::Source:
				return "source";
			case ProjectQuickFilter::Text:
				return "text";
			case ProjectQuickFilter::All:
			default:
				return "all";
			}
		}

		[[nodiscard]] constexpr ProjectQuickFilter ProjectQuickFilterFromToken(std::string_view token) noexcept
		{
			if (token == "favorites") { return ProjectQuickFilter::Favorites; }
			if (token == "folders") { return ProjectQuickFilter::Folders; }
			if (token == "models") { return ProjectQuickFilter::Models; }
			if (token == "images") { return ProjectQuickFilter::Images; }
			if (token == "scenes") { return ProjectQuickFilter::Scenes; }
			if (token == "materials") { return ProjectQuickFilter::Materials; }
			if (token == "prefabs") { return ProjectQuickFilter::Prefabs; }
			if (token == "source") { return ProjectQuickFilter::Source; }
			if (token == "text") { return ProjectQuickFilter::Text; }
			return ProjectQuickFilter::All;
		}

		[[nodiscard]] ProjectQuickFilter ProjectQuickFilterForAsset(const std::filesystem::path& path, Asset::AssetFileKind kind)
		{
			const std::string extension = ToLower(path.extension().string());
			if (kind == Asset::AssetFileKind::Directory)
			{
				return ProjectQuickFilter::Folders;
			}
			if (extension == ".scene")
			{
				return ProjectQuickFilter::Scenes;
			}
			if (extension == ".material" || extension == ".skybox")
			{
				return ProjectQuickFilter::Materials;
			}
			if (extension == ".prefab")
			{
				return ProjectQuickFilter::Prefabs;
			}
			if (kind == Asset::AssetFileKind::Model)
			{
				return ProjectQuickFilter::Models;
			}
			if (kind == Asset::AssetFileKind::Image)
			{
				return ProjectQuickFilter::Images;
			}
			if (kind == Asset::AssetFileKind::Source)
			{
				return ProjectQuickFilter::Source;
			}
			if (kind == Asset::AssetFileKind::Text)
			{
				return ProjectQuickFilter::Text;
			}
			return ProjectQuickFilter::All;
		}

		[[nodiscard]] constexpr const char* ContentDrawerSortModeName(ContentDrawerSortMode mode) noexcept
		{
			switch (mode)
			{
			case ContentDrawerSortMode::Name:
				return "Name";
			case ContentDrawerSortMode::Type:
				return "Type";
			case ContentDrawerSortMode::SizeDescending:
				return "Size";
			case ContentDrawerSortMode::ModifiedDescending:
				return "Modified";
			case ContentDrawerSortMode::Path:
			default:
				return "Path";
			}
		}

		[[nodiscard]] constexpr std::string_view ContentDrawerSortModeToken(ContentDrawerSortMode mode) noexcept
		{
			switch (mode)
			{
			case ContentDrawerSortMode::Name:
				return "name";
			case ContentDrawerSortMode::Type:
				return "type";
			case ContentDrawerSortMode::SizeDescending:
				return "sizeDesc";
			case ContentDrawerSortMode::ModifiedDescending:
				return "modifiedDesc";
			case ContentDrawerSortMode::Path:
			default:
				return "path";
			}
		}

		[[nodiscard]] constexpr ContentDrawerSortMode ContentDrawerSortModeFromToken(std::string_view token) noexcept
		{
			if (token == "name") { return ContentDrawerSortMode::Name; }
			if (token == "type") { return ContentDrawerSortMode::Type; }
			if (token == "sizeDesc") { return ContentDrawerSortMode::SizeDescending; }
			if (token == "modifiedDesc") { return ContentDrawerSortMode::ModifiedDescending; }
			return ContentDrawerSortMode::Path;
		}

		[[nodiscard]] constexpr const char* CommandPaletteScopeName(CommandPaletteScope scope) noexcept
		{
			switch (scope)
			{
			case CommandPaletteScope::Commands:
				return "Commands";
			case CommandPaletteScope::Entities:
				return "Entities";
			case CommandPaletteScope::Assets:
				return "Assets";
			case CommandPaletteScope::All:
			default:
				return "All";
			}
		}

		[[nodiscard]] constexpr std::string_view CommandPaletteScopeToken(CommandPaletteScope scope) noexcept
		{
			switch (scope)
			{
			case CommandPaletteScope::Commands:
				return "commands";
			case CommandPaletteScope::Entities:
				return "entities";
			case CommandPaletteScope::Assets:
				return "assets";
			case CommandPaletteScope::All:
			default:
				return "all";
			}
		}

		[[nodiscard]] constexpr CommandPaletteScope CommandPaletteScopeFromToken(std::string_view token) noexcept
		{
			if (token == "commands") { return CommandPaletteScope::Commands; }
			if (token == "entities") { return CommandPaletteScope::Entities; }
			if (token == "assets") { return CommandPaletteScope::Assets; }
			return CommandPaletteScope::All;
		}

		[[nodiscard]] const char* ExtensionTag(const std::filesystem::path& path)
		{
			const std::string extension = ToLower(path.extension().string());
			if (extension == ".fbx")
			{
				return "[FBX]";
			}
			if (extension == ".png")
			{
				return "[PNG]";
			}
			if (extension == ".jpg" || extension == ".jpeg")
			{
				return "[JPG]";
			}
			if (extension == ".skybox")
			{
				return "[SKY]";
			}
			if (extension == ".txt" || extension == ".md" || extension == ".json" || extension == ".scene" || extension == ".prefab" || extension == ".material")
			{
				return "[TXT]";
			}
			if (extension == ".h" || extension == ".cpp" || extension == ".hlsl" || extension == ".vert" || extension == ".frag")
			{
				return "[SRC]";
			}
			return "[FILE]";
		}

		[[nodiscard]] constexpr const char* AssetKindTag(Asset::AssetFileKind kind) noexcept
		{
			switch (kind)
			{
			case Asset::AssetFileKind::Directory:
				return "[D]";
			case Asset::AssetFileKind::Model:
				return "[MODEL]";
			case Asset::AssetFileKind::Image:
				return "[IMG]";
			case Asset::AssetFileKind::Text:
				return "[TXT]";
			case Asset::AssetFileKind::Source:
				return "[SRC]";
			default:
				return "[FILE]";
			}
		}

		struct ProjectDirectorySummary
		{
			size_t DirectDirectories = 0;
			size_t DirectFiles = 0;
			size_t DirectChildren = 0;
			size_t RecursiveDirectories = 0;
			size_t RecursiveFiles = 0;
			size_t Models = 0;
			size_t Images = 0;
			size_t Scenes = 0;
			size_t Materials = 0;
			size_t Prefabs = 0;
			size_t Source = 0;
			size_t Text = 0;
			size_t Other = 0;
			uintmax_t TotalBytes = 0;
		};

		[[nodiscard]] const Asset::AssetFileEntry* FindAssetEntryByPath(
			const std::vector<Asset::AssetFileEntry>& entries,
			const std::filesystem::path& path)
		{
			for (const Asset::AssetFileEntry& entry : entries)
			{
				if (ToLower(entry.Path.lexically_normal().string()) == ToLower(path.lexically_normal().string()))
				{
					return &entry;
				}
				if (!entry.Children.empty())
				{
					if (const Asset::AssetFileEntry* child = FindAssetEntryByPath(entry.Children, path))
					{
						return child;
					}
				}
			}
			return nullptr;
		}

		void AccumulateProjectDirectorySummary(const Asset::AssetFileEntry& entry, ProjectDirectorySummary& summary)
		{
			if (entry.Kind == Asset::AssetFileKind::Directory)
			{
				++summary.RecursiveDirectories;
				for (const Asset::AssetFileEntry& child : entry.Children)
				{
					AccumulateProjectDirectorySummary(child, summary);
				}
				return;
			}

			++summary.RecursiveFiles;
			summary.TotalBytes += entry.SizeBytes;
			const std::string extension = ToLower(entry.Path.extension().string());
			if (extension == ".scene")
			{
				++summary.Scenes;
			}
			else if (extension == ".material" || extension == ".skybox")
			{
				++summary.Materials;
			}
			else if (extension == ".prefab")
			{
				++summary.Prefabs;
			}
			else if (entry.Kind == Asset::AssetFileKind::Model)
			{
				++summary.Models;
			}
			else if (entry.Kind == Asset::AssetFileKind::Image)
			{
				++summary.Images;
			}
			else if (entry.Kind == Asset::AssetFileKind::Source)
			{
				++summary.Source;
			}
			else if (entry.Kind == Asset::AssetFileKind::Text)
			{
				++summary.Text;
			}
			else
			{
				++summary.Other;
			}
		}

		[[nodiscard]] ProjectDirectorySummary BuildProjectDirectorySummary(const std::vector<Asset::AssetFileEntry>& entries)
		{
			ProjectDirectorySummary summary;
			summary.DirectChildren = entries.size();
			for (const Asset::AssetFileEntry& entry : entries)
			{
				if (entry.Kind == Asset::AssetFileKind::Directory)
				{
					++summary.DirectDirectories;
				}
				else
				{
					++summary.DirectFiles;
				}
				AccumulateProjectDirectorySummary(entry, summary);
			}
			return summary;
		}

		void AcceptModelDrop(EditorContext& context, AssetDropTarget target)
		{
			if (!ImGui::BeginDragDropTarget())
			{
				return;
			}

			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathPayload))
			{
				if (payload->Data && payload->DataSize > 0 && context.OnModelDrop)
				{
					const char* pathText = static_cast<const char*>(payload->Data);
					context.OnModelDrop(std::filesystem::path(pathText), target);
				}
			}

			ImGui::EndDragDropTarget();
		}

		[[nodiscard]] std::string RelativeDisplayPath(const std::filesystem::path& path, const std::filesystem::path& rootPath)
		{
			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(path, rootPath, errorCode);
			return errorCode ? path.string() : relativePath.string();
		}

		void CopyTextToClipboard(std::string_view text)
		{
			const std::string copyText(text);
			ImGui::SetClipboardText(copyText.c_str());
		}

		template <size_t BufferSize>
		void SetTextBuffer(std::array<char, BufferSize>& buffer, std::string_view text)
		{
			buffer.fill('\0');
			const size_t copyLength = (std::min)(text.size(), buffer.size() - 1);
			std::copy_n(text.data(), copyLength, buffer.data());
		}

		void DrawProjectPathCopyMenuItems(const std::filesystem::path& path, const std::filesystem::path& rootPath)
		{
			if (path.empty())
			{
				return;
			}

			if (ImGui::MenuItem("Copy Absolute Path"))
			{
				CopyTextToClipboard(path.string());
			}
			if (!rootPath.empty() && ImGui::MenuItem("Copy Project Relative Path"))
			{
				CopyTextToClipboard(RelativeDisplayPath(path, rootPath));
			}
			if (ImGui::MenuItem("Copy File Name"))
			{
				CopyTextToClipboard(path.filename().string());
			}
		}

		void SetInitialWindowRect(const char* name, float x, float y, float width, float height)
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + x, viewport->Pos.y + y), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
			(void)name;
		}

		[[nodiscard]] bool SamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
		{
			return ToLower(lhs.lexically_normal().string()) == ToLower(rhs.lexically_normal().string());
		}

		[[nodiscard]] std::filesystem::path EditorProjectStatePath(const std::filesystem::path& projectRootPath)
		{
			return projectRootPath / "Settings" / "EditorProjectState.json";
		}

		[[nodiscard]] std::string ToStoredProjectPath(const std::filesystem::path& path, const std::filesystem::path& projectRootPath)
		{
			if (path.empty())
			{
				return {};
			}

			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(path.lexically_normal(), projectRootPath.lexically_normal(), errorCode);
			if (!errorCode && !relativePath.empty() && *relativePath.begin() != "..")
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

		void DrawVector3Text(const char* label, const DirectX::XMFLOAT3& value)
		{
			ImGui::Text("%s: %.2f, %.2f, %.2f", label, value.x, value.y, value.z);
		}

		[[nodiscard]] constexpr const char* LightTypeName(LightType type) noexcept
		{
			switch (type)
			{
			case LightType::Directional:
				return "Directional";
			case LightType::Point:
				return "Point";
			case LightType::Spot:
				return "Spot";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] constexpr const char* MaterialDebugViewName(MaterialDebugView view) noexcept
		{
			switch (view)
			{
			case MaterialDebugView::BaseColor:
				return "BaseColor";
			case MaterialDebugView::Normal:
				return "Normal";
			case MaterialDebugView::Metallic:
				return "Metallic";
			case MaterialDebugView::Roughness:
				return "Roughness";
			case MaterialDebugView::AO:
				return "AO";
			case MaterialDebugView::Emissive:
				return "Emissive";
			case MaterialDebugView::LightingOnly:
				return "LightingOnly";
			case MaterialDebugView::VertexColor:
				return "VertexColor";
			case MaterialDebugView::Shadow:
				return "Shadow";
			case MaterialDebugView::DeferredTileLights:
				return "TileLights";
			case MaterialDebugView::Lit:
			default:
				return "Lit";
			}
		}

		[[nodiscard]] constexpr const char* TransformGizmoModeName(TransformGizmoMode mode) noexcept
		{
			switch (mode)
			{
			case TransformGizmoMode::Translate:
				return "Translate";
			case TransformGizmoMode::Rotate:
				return "Rotate";
			case TransformGizmoMode::Scale:
				return "Scale";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] constexpr const char* TransformGizmoSpaceName(TransformGizmoSpace space) noexcept
		{
			switch (space)
			{
			case TransformGizmoSpace::Local:
				return "Local";
			case TransformGizmoSpace::World:
			default:
				return "World";
			}
		}

		[[nodiscard]] constexpr const char* TransformGizmoPivotName(TransformGizmoPivot pivot) noexcept
		{
			switch (pivot)
			{
			case TransformGizmoPivot::Center:
				return "Center";
			case TransformGizmoPivot::Pivot:
			default:
				return "Pivot";
			}
		}

		[[nodiscard]] constexpr const char* SceneMeasureTargetName(SceneMeasureTarget target) noexcept
		{
			switch (target)
			{
			case SceneMeasureTarget::MeshSurface:
				return "Mesh";
			case SceneMeasureTarget::ViewPlane:
				return "View";
			case SceneMeasureTarget::SelectionBounds:
				return "Bounds";
			case SceneMeasureTarget::Ground:
			default:
				return "Y=0";
			}
		}

		[[nodiscard]] constexpr const char* TransformGizmoAxisName(TransformGizmoAxis axis) noexcept
		{
			switch (axis)
			{
			case TransformGizmoAxis::X:
				return "X";
			case TransformGizmoAxis::Y:
				return "Y";
			case TransformGizmoAxis::Z:
				return "Z";
			default:
				return "";
			}
		}

		[[nodiscard]] constexpr const char* TransformGizmoPlaneName(TransformGizmoPlane plane) noexcept
		{
			switch (plane)
			{
			case TransformGizmoPlane::XY:
				return "XY";
			case TransformGizmoPlane::XZ:
				return "XZ";
			case TransformGizmoPlane::YZ:
				return "YZ";
			default:
				return "";
			}
		}

		[[nodiscard]] constexpr std::pair<TransformGizmoAxis, TransformGizmoAxis> TransformGizmoPlaneAxes(TransformGizmoPlane plane) noexcept
		{
			switch (plane)
			{
			case TransformGizmoPlane::XY:
				return { TransformGizmoAxis::X, TransformGizmoAxis::Y };
			case TransformGizmoPlane::XZ:
				return { TransformGizmoAxis::X, TransformGizmoAxis::Z };
			case TransformGizmoPlane::YZ:
				return { TransformGizmoAxis::Y, TransformGizmoAxis::Z };
			default:
				return { TransformGizmoAxis::None, TransformGizmoAxis::None };
			}
		}

		[[nodiscard]] constexpr ImU32 TransformGizmoAxisColor(TransformGizmoAxis axis, bool highlighted) noexcept
		{
			const int alpha = highlighted ? 255 : 220;
			switch (axis)
			{
			case TransformGizmoAxis::X:
				return IM_COL32(239, 82, 82, alpha);
			case TransformGizmoAxis::Y:
				return IM_COL32(83, 213, 104, alpha);
			case TransformGizmoAxis::Z:
				return IM_COL32(86, 151, 255, alpha);
			default:
				return IM_COL32(240, 240, 240, alpha);
			}
		}

		[[nodiscard]] constexpr DirectX::XMFLOAT3 TransformGizmoAxisVector(TransformGizmoAxis axis) noexcept
		{
			switch (axis)
			{
			case TransformGizmoAxis::X:
				return { 1.0f, 0.0f, 0.0f };
			case TransformGizmoAxis::Y:
				return { 0.0f, 1.0f, 0.0f };
			case TransformGizmoAxis::Z:
				return { 0.0f, 0.0f, 1.0f };
			default:
				return { 0.0f, 0.0f, 0.0f };
			}
		}

		[[nodiscard]] DirectX::XMFLOAT3 TransformGizmoAxisDirection(
			TransformGizmoAxis axis,
			TransformGizmoSpace space,
			const Math::Transform& referenceTransform) noexcept
		{
			DirectX::XMFLOAT3 direction = TransformGizmoAxisVector(axis);
			if (space == TransformGizmoSpace::World || axis == TransformGizmoAxis::None)
			{
				return direction;
			}

			const DirectX::XMVECTOR localDirection = DirectX::XMLoadFloat3(&direction);
			const DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&referenceTransform.Rotation);
			const DirectX::XMVECTOR worldDirection = DirectX::XMVector3Normalize(DirectX::XMVector3Rotate(localDirection, rotation));
			DirectX::XMStoreFloat3(&direction, worldDirection);
			return direction;
		}

		[[nodiscard]] constexpr size_t TransformGizmoAxisIndex(TransformGizmoAxis axis) noexcept
		{
			switch (axis)
			{
			case TransformGizmoAxis::X:
				return 0;
			case TransformGizmoAxis::Y:
				return 1;
			case TransformGizmoAxis::Z:
				return 2;
			default:
				return 0;
			}
		}

		[[nodiscard]] float DistancePointToSegment(const ImVec2& point, const ImVec2& begin, const ImVec2& end) noexcept
		{
			const float segmentX = end.x - begin.x;
			const float segmentY = end.y - begin.y;
			const float lengthSquared = segmentX * segmentX + segmentY * segmentY;
			if (lengthSquared <= 0.0001f)
			{
				const float dx = point.x - begin.x;
				const float dy = point.y - begin.y;
				return std::sqrt(dx * dx + dy * dy);
			}

			const float t = std::clamp(
				((point.x - begin.x) * segmentX + (point.y - begin.y) * segmentY) / lengthSquared,
				0.0f,
				1.0f);
			const float closestX = begin.x + segmentX * t;
			const float closestY = begin.y + segmentY * t;
			const float dx = point.x - closestX;
			const float dy = point.y - closestY;
			return std::sqrt(dx * dx + dy * dy);
		}

		[[nodiscard]] bool NearlyEqual(float lhs, float rhs, float epsilon = 1.0e-4f) noexcept
		{
			return std::fabs(lhs - rhs) <= epsilon;
		}

		[[nodiscard]] bool TransformNearlyEqual(const Math::Transform& lhs, const Math::Transform& rhs) noexcept
		{
			return NearlyEqual(lhs.Translation.x, rhs.Translation.x) &&
				NearlyEqual(lhs.Translation.y, rhs.Translation.y) &&
				NearlyEqual(lhs.Translation.z, rhs.Translation.z) &&
				NearlyEqual(lhs.Rotation.x, rhs.Rotation.x) &&
				NearlyEqual(lhs.Rotation.y, rhs.Rotation.y) &&
				NearlyEqual(lhs.Rotation.z, rhs.Rotation.z) &&
				NearlyEqual(lhs.Rotation.w, rhs.Rotation.w) &&
				NearlyEqual(lhs.Scale.x, rhs.Scale.x) &&
				NearlyEqual(lhs.Scale.y, rhs.Scale.y) &&
				NearlyEqual(lhs.Scale.z, rhs.Scale.z);
		}

		[[nodiscard]] float SnapValue(float value, float increment) noexcept
		{
			if (increment <= 0.0001f)
			{
				return value;
			}
			return std::round(value / increment) * increment;
		}

		[[nodiscard]] bool PointInRect(const ImVec2& point, const ImVec2& rectMin, const ImVec2& rectMax) noexcept
		{
			return point.x >= rectMin.x && point.x <= rectMax.x && point.y >= rectMin.y && point.y <= rectMax.y;
		}

		[[nodiscard]] float Distance(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs) noexcept
		{
			const float dx = lhs.x - rhs.x;
			const float dy = lhs.y - rhs.y;
			const float dz = lhs.z - rhs.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		[[nodiscard]] bool IntersectRayPlane(
			const DirectX::XMFLOAT3& rayOrigin,
			const DirectX::XMFLOAT3& rayDirection,
			const DirectX::XMFLOAT3& planePoint,
			const DirectX::XMFLOAT3& planeNormal,
			DirectX::XMFLOAT3& hitPoint) noexcept
		{
			const float denominator =
				rayDirection.x * planeNormal.x +
				rayDirection.y * planeNormal.y +
				rayDirection.z * planeNormal.z;
			if (std::fabs(denominator) <= 0.0001f)
			{
				return false;
			}

			const DirectX::XMFLOAT3 toPlane = {
				planePoint.x - rayOrigin.x,
				planePoint.y - rayOrigin.y,
				planePoint.z - rayOrigin.z
			};
			const float t =
				(toPlane.x * planeNormal.x + toPlane.y * planeNormal.y + toPlane.z * planeNormal.z) /
				denominator;
			if (!std::isfinite(t) || t < 0.0f)
			{
				return false;
			}

			hitPoint = {
				rayOrigin.x + rayDirection.x * t,
				rayOrigin.y + rayDirection.y * t,
				rayOrigin.z + rayDirection.z * t
			};
			return std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z);
		}

		[[nodiscard]] bool IntersectRayAabbPoint(
			const DirectX::XMFLOAT3& rayOrigin,
			const DirectX::XMFLOAT3& rayDirection,
			const DirectX::XMFLOAT3& aabbMin,
			const DirectX::XMFLOAT3& aabbMax,
			DirectX::XMFLOAT3& hitPoint) noexcept
		{
			float tMin = 0.0f;
			float tMax = (std::numeric_limits<float>::max)();
			constexpr float kEpsilon = 1.0e-6f;
			const std::array<float, 3> origin = { rayOrigin.x, rayOrigin.y, rayOrigin.z };
			const std::array<float, 3> direction = { rayDirection.x, rayDirection.y, rayDirection.z };
			const std::array<float, 3> minValue = { aabbMin.x, aabbMin.y, aabbMin.z };
			const std::array<float, 3> maxValue = { aabbMax.x, aabbMax.y, aabbMax.z };

			for (size_t axis = 0; axis < 3; ++axis)
			{
				if (std::fabs(direction[axis]) < kEpsilon)
				{
					if (origin[axis] < minValue[axis] || origin[axis] > maxValue[axis])
					{
						return false;
					}
					continue;
				}

				const float inverseDirection = 1.0f / direction[axis];
				float t1 = (minValue[axis] - origin[axis]) * inverseDirection;
				float t2 = (maxValue[axis] - origin[axis]) * inverseDirection;
				if (t1 > t2)
				{
					std::swap(t1, t2);
				}
				tMin = (std::max)(tMin, t1);
				tMax = (std::min)(tMax, t2);
				if (tMin > tMax)
				{
					return false;
				}
			}

			if (!std::isfinite(tMin))
			{
				return false;
			}

			hitPoint = {
				rayOrigin.x + rayDirection.x * tMin,
				rayOrigin.y + rayDirection.y * tMin,
				rayOrigin.z + rayDirection.z * tMin
			};
			return true;
		}

		[[nodiscard]] bool IntersectRayTrianglePoint(
			const DirectX::XMFLOAT3& rayOrigin,
			const DirectX::XMFLOAT3& rayDirection,
			const DirectX::XMFLOAT3& v0,
			const DirectX::XMFLOAT3& v1,
			const DirectX::XMFLOAT3& v2,
			float& t,
			DirectX::XMFLOAT3& hitPoint) noexcept
		{
			const DirectX::XMVECTOR origin = DirectX::XMLoadFloat3(&rayOrigin);
			const DirectX::XMVECTOR direction = DirectX::XMLoadFloat3(&rayDirection);
			const DirectX::XMVECTOR p0 = DirectX::XMLoadFloat3(&v0);
			const DirectX::XMVECTOR p1 = DirectX::XMLoadFloat3(&v1);
			const DirectX::XMVECTOR p2 = DirectX::XMLoadFloat3(&v2);

			const DirectX::XMVECTOR edge1 = DirectX::XMVectorSubtract(p1, p0);
			const DirectX::XMVECTOR edge2 = DirectX::XMVectorSubtract(p2, p0);
			const DirectX::XMVECTOR h = DirectX::XMVector3Cross(direction, edge2);
			const float determinant = DirectX::XMVectorGetX(DirectX::XMVector3Dot(edge1, h));
			if (std::fabs(determinant) <= 1.0e-6f)
			{
				return false;
			}

			const float inverseDeterminant = 1.0f / determinant;
			const DirectX::XMVECTOR s = DirectX::XMVectorSubtract(origin, p0);
			const float u = inverseDeterminant * DirectX::XMVectorGetX(DirectX::XMVector3Dot(s, h));
			if (u < 0.0f || u > 1.0f)
			{
				return false;
			}

			const DirectX::XMVECTOR q = DirectX::XMVector3Cross(s, edge1);
			const float v = inverseDeterminant * DirectX::XMVectorGetX(DirectX::XMVector3Dot(direction, q));
			if (v < 0.0f || u + v > 1.0f)
			{
				return false;
			}

			t = inverseDeterminant * DirectX::XMVectorGetX(DirectX::XMVector3Dot(edge2, q));
			if (!std::isfinite(t) || t < 0.0f)
			{
				return false;
			}

			DirectX::XMStoreFloat3(&hitPoint, DirectX::XMVectorAdd(origin, DirectX::XMVectorScale(direction, t)));
			return std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z);
		}

		struct ViewFocus
		{
			DirectX::XMFLOAT3 Center = { 0.0f, 0.0f, 0.0f };
			float Radius = 5.0f;
		};

		[[nodiscard]] ViewFocus ComputeEntityViewFocus(const Scene& scene, EntityId entityId) noexcept
		{
			ViewFocus focus = {};
			const TransformComponent* transform = scene.GetTransformComponent(entityId);
			if (!transform)
			{
				return focus;
			}

			focus.Center = transform->WorldTransform.Translation;
			focus.Radius = 5.0f;
			const BoundsComponent* bounds = scene.GetBoundsComponent(entityId);
			if (!bounds)
			{
				return focus;
			}

			const DirectX::XMFLOAT3 localCenter = {
				(bounds->LocalMin.x + bounds->LocalMax.x) * 0.5f,
				(bounds->LocalMin.y + bounds->LocalMax.y) * 0.5f,
				(bounds->LocalMin.z + bounds->LocalMax.z) * 0.5f
			};
			const std::array<DirectX::XMFLOAT3, 8> localCorners = {{
				{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMin.z },
				{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMin.z },
				{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMin.z },
				{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMin.z },
				{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMax.z },
				{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMax.z },
				{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMax.z },
				{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMax.z }
			}};

			const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
			DirectX::XMStoreFloat3(
				&focus.Center,
				DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCenter), worldMatrix));
			focus.Radius = 0.5f;
			for (const DirectX::XMFLOAT3& localCorner : localCorners)
			{
				DirectX::XMFLOAT3 worldCorner = {};
				DirectX::XMStoreFloat3(
					&worldCorner,
					DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorner), worldMatrix));
				focus.Radius = (std::max)(focus.Radius, Distance(focus.Center, worldCorner));
			}
			return focus;
		}

		void OrbitCameraAroundFocus(Camera& camera, const ViewFocus& focus, const ImVec2& mouseDelta, float sensitivity) noexcept
		{
			const DirectX::XMFLOAT3 cameraPosition = camera.GetPosition();
			DirectX::XMFLOAT3 offset = {
				cameraPosition.x - focus.Center.x,
				cameraPosition.y - focus.Center.y,
				cameraPosition.z - focus.Center.z
			};
			float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
			const float minimumDistance = (std::max)(focus.Radius * 1.25f, 1.0f);
			if (distance < minimumDistance || !std::isfinite(distance))
			{
				distance = (std::max)(focus.Radius * 3.0f, 6.0f);
				offset = { 0.0f, focus.Radius * 0.4f, -distance };
			}

			float yaw = std::atan2(offset.x, offset.z);
			const float horizontalDistance = std::sqrt(offset.x * offset.x + offset.z * offset.z);
			float pitch = std::atan2(offset.y, (std::max)(horizontalDistance, 0.0001f));
			yaw += mouseDelta.x * sensitivity;
			pitch = std::clamp(pitch + mouseDelta.y * sensitivity, -DirectX::XM_PIDIV2 + 0.04f, DirectX::XM_PIDIV2 - 0.04f);

			const float cosPitch = std::cos(pitch);
			const DirectX::XMFLOAT3 newOffset = {
				std::sin(yaw) * cosPitch * distance,
				std::sin(pitch) * distance,
				std::cos(yaw) * cosPitch * distance
			};
			const DirectX::XMFLOAT3 eye = {
				focus.Center.x + newOffset.x,
				focus.Center.y + newOffset.y,
				focus.Center.z + newOffset.z
			};
			camera.LookAt(eye, focus.Center, { 0.0f, 1.0f, 0.0f });
		}

		enum class RoadmapHealthState : uint8_t
		{
			Active,
			Ready,
			Idle,
			Warning
		};

		[[nodiscard]] constexpr const char* RoadmapHealthStateName(RoadmapHealthState state) noexcept
		{
			switch (state)
			{
			case RoadmapHealthState::Active:
				return "Active";
			case RoadmapHealthState::Ready:
				return "Ready";
			case RoadmapHealthState::Idle:
				return "Idle";
			case RoadmapHealthState::Warning:
				return "Warning";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] constexpr ImVec4 RoadmapHealthStateColor(RoadmapHealthState state) noexcept
		{
			switch (state)
			{
			case RoadmapHealthState::Active:
				return ImVec4(0.35f, 0.88f, 0.48f, 1.0f);
			case RoadmapHealthState::Ready:
				return ImVec4(0.42f, 0.70f, 1.0f, 1.0f);
			case RoadmapHealthState::Idle:
				return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
			case RoadmapHealthState::Warning:
				return ImVec4(1.0f, 0.72f, 0.22f, 1.0f);
			default:
				return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
			}
		}

		void DrawRoadmapHealthRow(uint32_t index, const char* item, RoadmapHealthState state, std::string_view evidence)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%u", index);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(item);
			ImGui::TableSetColumnIndex(2);
			ImGui::TextColored(RoadmapHealthStateColor(state), "%s", RoadmapHealthStateName(state));
			ImGui::TableSetColumnIndex(3);
			ImGui::TextWrapped("%.*s", static_cast<int>(evidence.size()), evidence.data());
		}

		[[nodiscard]] constexpr const char* ComponentKindName(SceneComponentKind kind) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				return "Mesh";
			case SceneComponentKind::Animator:
				return "Animator";
			case SceneComponentKind::Camera:
				return "Camera";
			case SceneComponentKind::Light:
				return "Light";
			case SceneComponentKind::RigidBody:
				return "Rigidbody";
			case SceneComponentKind::Collider:
				return "Collider";
			case SceneComponentKind::PhysicsMaterial:
				return "Physics Material";
			case SceneComponentKind::PrefabInstance:
				return "Prefab Instance";
			case SceneComponentKind::SceneReference:
				return "Scene Reference";
			case SceneComponentKind::Script:
				return "Script";
			case SceneComponentKind::Sprite2D:
				return "Sprite 2D";
			case SceneComponentKind::UiElement:
				return "UI Element";
			case SceneComponentKind::AudioSource:
				return "Audio Source";
			case SceneComponentKind::NavigationAgent:
				return "Navigation Agent";
			case SceneComponentKind::NetworkIdentity:
				return "Network Identity";
			default:
				return "Component";
			}
		}

		[[nodiscard]] constexpr const char* ComponentKindToken(SceneComponentKind kind) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				return "Mesh";
			case SceneComponentKind::Animator:
				return "Animator";
			case SceneComponentKind::Camera:
				return "Camera";
			case SceneComponentKind::Light:
				return "Light";
			case SceneComponentKind::RigidBody:
				return "RigidBody";
			case SceneComponentKind::Collider:
				return "Collider";
			case SceneComponentKind::PhysicsMaterial:
				return "PhysicsMaterial";
			case SceneComponentKind::PrefabInstance:
				return "PrefabInstance";
			case SceneComponentKind::SceneReference:
				return "SceneReference";
			case SceneComponentKind::Script:
				return "Script";
			case SceneComponentKind::Sprite2D:
				return "Sprite2D";
			case SceneComponentKind::UiElement:
				return "UiElement";
			case SceneComponentKind::AudioSource:
				return "AudioSource";
			case SceneComponentKind::NavigationAgent:
				return "NavigationAgent";
			case SceneComponentKind::NetworkIdentity:
				return "NetworkIdentity";
			default:
				return "Unknown";
			}
		}

		struct ComponentSectionState
		{
			bool Open = false;
			bool Enabled = true;
			bool Removed = false;
			bool PinToggledRequested = false;
			bool ResetRequested = false;
			bool CopyRequested = false;
			bool PasteRequested = false;
			bool MoveUpRequested = false;
			bool MoveDownRequested = false;
			bool PrefabRevertRequested = false;
			bool PrefabApplyRequested = false;
		};

		constexpr std::array kInspectorDefaultComponentOrder = {
			SceneComponentKind::Camera,
			SceneComponentKind::Light,
			SceneComponentKind::RigidBody,
			SceneComponentKind::Collider,
			SceneComponentKind::PhysicsMaterial,
			SceneComponentKind::PrefabInstance,
			SceneComponentKind::SceneReference,
			SceneComponentKind::Script,
			SceneComponentKind::Sprite2D,
			SceneComponentKind::UiElement,
			SceneComponentKind::AudioSource,
			SceneComponentKind::NavigationAgent,
			SceneComponentKind::NetworkIdentity,
			SceneComponentKind::Mesh,
			SceneComponentKind::Animator
		};

		[[nodiscard]] bool TryParseComponentKindToken(std::string_view token, SceneComponentKind& kind) noexcept
		{
			for (const SceneComponentKind candidate : kInspectorDefaultComponentOrder)
			{
				if (token == ComponentKindToken(candidate))
				{
					kind = candidate;
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool HasInspectableComponent(const Scene& scene, EntityId entityId, SceneComponentKind kind)
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				return scene.GetMeshComponent(entityId) != nullptr;
			case SceneComponentKind::Animator:
				return scene.GetAnimatorComponent(entityId) != nullptr;
			case SceneComponentKind::Camera:
				return scene.GetCameraComponent(entityId) != nullptr;
			case SceneComponentKind::Light:
				return scene.GetLightComponent(entityId) != nullptr;
			case SceneComponentKind::RigidBody:
				return scene.GetRigidBodyComponent(entityId) != nullptr;
			case SceneComponentKind::Collider:
				return scene.GetColliderComponent(entityId) != nullptr;
			case SceneComponentKind::PhysicsMaterial:
				return scene.GetPhysicsMaterialComponent(entityId) != nullptr;
			case SceneComponentKind::PrefabInstance:
				return scene.GetPrefabInstanceComponent(entityId) != nullptr;
			case SceneComponentKind::SceneReference:
				return scene.GetSceneReferenceComponent(entityId) != nullptr;
			case SceneComponentKind::Script:
				return scene.GetScriptComponent(entityId) != nullptr;
			case SceneComponentKind::Sprite2D:
				return scene.GetSprite2DComponent(entityId) != nullptr;
			case SceneComponentKind::UiElement:
				return scene.GetUiElementComponent(entityId) != nullptr;
			case SceneComponentKind::AudioSource:
				return scene.GetAudioSourceComponent(entityId) != nullptr;
			case SceneComponentKind::NavigationAgent:
				return scene.GetNavigationAgentComponent(entityId) != nullptr;
			case SceneComponentKind::NetworkIdentity:
				return scene.GetNetworkIdentityComponent(entityId) != nullptr;
			default:
				return false;
			}
		}

		[[nodiscard]] bool IsInspectableComponentEnabled(const Scene& scene, EntityId entityId, SceneComponentKind kind)
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				return scene.IsComponentEnabled<MeshComponent>(entityId);
			case SceneComponentKind::Animator:
				return scene.IsComponentEnabled<AnimatorComponent>(entityId);
			case SceneComponentKind::Camera:
				return scene.IsComponentEnabled<CameraComponent>(entityId);
			case SceneComponentKind::Light:
				return scene.IsComponentEnabled<LightComponent>(entityId);
			case SceneComponentKind::RigidBody:
				return scene.IsComponentEnabled<RigidBodyComponent>(entityId);
			case SceneComponentKind::Collider:
				return scene.IsComponentEnabled<ColliderComponent>(entityId);
			case SceneComponentKind::PhysicsMaterial:
				return scene.IsComponentEnabled<PhysicsMaterialComponent>(entityId);
			case SceneComponentKind::PrefabInstance:
				return scene.IsComponentEnabled<PrefabInstanceComponent>(entityId);
			case SceneComponentKind::SceneReference:
				return scene.IsComponentEnabled<SceneReferenceComponent>(entityId);
			case SceneComponentKind::Script:
				return scene.IsComponentEnabled<ScriptComponent>(entityId);
			case SceneComponentKind::Sprite2D:
				return scene.IsComponentEnabled<Sprite2DComponent>(entityId);
			case SceneComponentKind::UiElement:
				return scene.IsComponentEnabled<UiElementComponent>(entityId);
			case SceneComponentKind::AudioSource:
				return scene.IsComponentEnabled<AudioSourceComponent>(entityId);
			case SceneComponentKind::NavigationAgent:
				return scene.IsComponentEnabled<NavigationAgentComponent>(entityId);
			case SceneComponentKind::NetworkIdentity:
				return scene.IsComponentEnabled<NetworkIdentityComponent>(entityId);
			default:
				return false;
			}
		}

		void NormalizeInspectorComponentOrder(const Scene& scene, EntityId entityId, std::vector<SceneComponentKind>& order)
		{
			std::vector<SceneComponentKind> normalized;
			normalized.reserve(order.size());
			for (SceneComponentKind kind : order)
			{
				if (!HasInspectableComponent(scene, entityId, kind) ||
					std::ranges::find(normalized, kind) != normalized.end())
				{
					continue;
				}
				normalized.push_back(kind);
			}

			for (SceneComponentKind kind : kInspectorDefaultComponentOrder)
			{
				if (HasInspectableComponent(scene, entityId, kind) &&
					std::ranges::find(normalized, kind) == normalized.end())
				{
					normalized.push_back(kind);
				}
			}

			order = std::move(normalized);
		}

		void NormalizeInspectorPinnedComponents(std::vector<SceneComponentKind>& pinnedComponents)
		{
			std::vector<SceneComponentKind> normalized;
			normalized.reserve(pinnedComponents.size());
			for (SceneComponentKind kind : pinnedComponents)
			{
				if (std::ranges::find(kInspectorDefaultComponentOrder, kind) == kInspectorDefaultComponentOrder.end() ||
					std::ranges::find(normalized, kind) != normalized.end())
				{
					continue;
				}
				normalized.push_back(kind);
			}
			pinnedComponents = std::move(normalized);
		}

		template <typename Component>
		[[nodiscard]] ComponentSectionState BeginComponentSection(
			EditorContext& context,
			EntityId entityId,
			SceneComponentKind kind,
			const char* label,
			bool canPaste,
			bool canReset,
			bool canMoveUp,
			bool canMoveDown,
			bool hasPrefabOverride = false,
			bool canRevertPrefabOverride = false,
			bool canApplyPrefabOverride = false,
			bool pinned = false,
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen)
		{
			ComponentSectionState state;
			state.Enabled = context.ActiveScene.IsComponentEnabled<Component>(entityId);

			ImGui::PushID(label);
			bool enabled = state.Enabled;
			if (ImGui::Checkbox("##enabled", &enabled))
			{
				state.Enabled = enabled;
				if (context.OnComponentEnabledChanged)
				{
					context.OnComponentEnabledChanged(entityId, kind, enabled);
				}
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Enable component");
			}

			ImGui::SameLine();
			state.Open = ImGui::TreeNodeEx("##tree", flags | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label);
			if (hasPrefabOverride)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "[Override]");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("This component differs from its prefab source.");
				}
			}
			if (pinned)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.45f, 0.72f, 1.0f, 1.0f), "[Pinned]");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Pinned components stay near the top of the Inspector.");
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("..."))
			{
				ImGui::OpenPopup("ComponentActions");
			}
			if (ImGui::BeginPopup("ComponentActions"))
			{
				if (ImGui::MenuItem(pinned ? "Unpin From Top" : "Pin To Top"))
				{
					state.PinToggledRequested = true;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Move Up", nullptr, false, canMoveUp))
				{
					state.MoveUpRequested = true;
				}
				if (ImGui::MenuItem("Move Down", nullptr, false, canMoveDown))
				{
					state.MoveDownRequested = true;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Reset", nullptr, false, canReset))
				{
					state.ResetRequested = true;
				}
				if (!canReset && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Reset is not available for this component in v1.");
				}
				if (ImGui::MenuItem("Copy Values"))
				{
					state.CopyRequested = true;
				}
				if (ImGui::MenuItem("Paste Values", nullptr, false, canPaste))
				{
					state.PasteRequested = true;
				}
				if (!canPaste && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Copy the same component type first.");
				}
				if (hasPrefabOverride)
				{
					ImGui::Separator();
					if (ImGui::MenuItem("Revert Prefab Override", nullptr, false, canRevertPrefabOverride))
					{
						state.PrefabRevertRequested = true;
					}
					if (!canRevertPrefabOverride && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						ImGui::SetTooltip("This component is protected from prefab revert in v1.");
					}
					if (ImGui::MenuItem("Apply Component To Prefab", nullptr, false, canApplyPrefabOverride))
					{
						state.PrefabApplyRequested = true;
					}
					if (!canApplyPrefabOverride && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					{
						ImGui::SetTooltip("Prefab apply is unavailable for this component.");
					}
				}
				ImGui::EndPopup();
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Component actions");
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
			{
				state.Removed = true;
				if (context.OnComponentRemoved)
				{
					context.OnComponentRemoved(entityId, kind);
				}
				if (state.Open)
				{
					ImGui::TreePop();
					state.Open = false;
				}
			}
			ImGui::PopID();
			return state;
		}

		[[nodiscard]] constexpr bool CanResetComponentKind(SceneComponentKind kind) noexcept
		{
			return kind != SceneComponentKind::Mesh;
		}

		[[nodiscard]] bool SnapshotHasComponent(SceneComponentKind kind, const ScenePersistence::LoadedSceneEntity& snapshot) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				return snapshot.HasMesh;
			case SceneComponentKind::Animator:
				return snapshot.HasAnimator;
			case SceneComponentKind::Camera:
				return snapshot.HasCamera;
			case SceneComponentKind::Light:
				return snapshot.HasLight;
			case SceneComponentKind::RigidBody:
				return snapshot.HasRigidBody;
			case SceneComponentKind::Collider:
				return snapshot.HasCollider;
			case SceneComponentKind::PhysicsMaterial:
				return snapshot.HasPhysicsMaterial;
			case SceneComponentKind::PrefabInstance:
				return snapshot.HasPrefabInstance;
			case SceneComponentKind::SceneReference:
				return snapshot.HasSceneReference;
			case SceneComponentKind::Script:
				return snapshot.HasScript;
			case SceneComponentKind::Sprite2D:
				return snapshot.HasSprite2D;
			case SceneComponentKind::UiElement:
				return snapshot.HasUiElement;
			case SceneComponentKind::AudioSource:
				return snapshot.HasAudioSource;
			case SceneComponentKind::NavigationAgent:
				return snapshot.HasNavigationAgent;
			case SceneComponentKind::NetworkIdentity:
				return snapshot.HasNetworkIdentity;
			default:
				return false;
			}
		}

		[[nodiscard]] bool CaptureComponentSnapshot(
			const Scene& scene,
			EntityId entityId,
			SceneComponentKind kind,
			ScenePersistence::LoadedSceneEntity& snapshot)
		{
			snapshot = {};
			if (!scene.ContainsEntity(entityId))
			{
				return false;
			}
			if (const std::string* name = scene.GetEntityName(entityId))
			{
				snapshot.Name = *name;
			}

			switch (kind)
			{
			case SceneComponentKind::Mesh:
				if (const MeshComponent* mesh = scene.GetMeshComponent(entityId))
				{
					snapshot.HasMesh = true;
					snapshot.MeshEnabled = scene.IsMeshEnabled(entityId);
					if (mesh->Asset)
					{
						snapshot.PrimitiveKind = mesh->Asset->PrimitiveKind;
						snapshot.MeshAssetPath = mesh->Asset->SourcePath;
						snapshot.MaterialOverrides.assign(mesh->Asset->Materials.begin(), mesh->Asset->Materials.end());
					}
				}
				break;
			case SceneComponentKind::Animator:
				if (const AnimatorComponent* animator = scene.GetAnimatorComponent(entityId))
				{
					snapshot.HasAnimator = true;
					snapshot.AnimatorEnabled = scene.IsAnimatorEnabled(entityId);
					snapshot.Animator = *animator;
				}
				break;
			case SceneComponentKind::Camera:
				if (const CameraComponent* camera = scene.GetCameraComponent(entityId))
				{
					snapshot.HasCamera = true;
					snapshot.CameraEnabled = scene.IsCameraEnabled(entityId);
					snapshot.Camera = *camera;
				}
				break;
			case SceneComponentKind::Light:
				if (const LightComponent* light = scene.GetLightComponent(entityId))
				{
					snapshot.HasLight = true;
					snapshot.LightEnabled = scene.IsLightEnabled(entityId);
					snapshot.Light = *light;
				}
				break;
			case SceneComponentKind::RigidBody:
				if (const RigidBodyComponent* rigidBody = scene.GetRigidBodyComponent(entityId))
				{
					snapshot.HasRigidBody = true;
					snapshot.RigidBodyEnabled = scene.IsRigidBodyEnabled(entityId);
					snapshot.RigidBody = *rigidBody;
				}
				break;
			case SceneComponentKind::Collider:
				if (const ColliderComponent* collider = scene.GetColliderComponent(entityId))
				{
					snapshot.HasCollider = true;
					snapshot.ColliderEnabled = scene.IsColliderEnabled(entityId);
					snapshot.Collider = *collider;
				}
				break;
			case SceneComponentKind::PhysicsMaterial:
				if (const PhysicsMaterialComponent* physicsMaterial = scene.GetPhysicsMaterialComponent(entityId))
				{
					snapshot.HasPhysicsMaterial = true;
					snapshot.PhysicsMaterialEnabled = scene.IsPhysicsMaterialEnabled(entityId);
					snapshot.PhysicsMaterial = *physicsMaterial;
				}
				break;
			case SceneComponentKind::PrefabInstance:
				if (const PrefabInstanceComponent* prefab = scene.GetPrefabInstanceComponent(entityId))
				{
					snapshot.HasPrefabInstance = true;
					snapshot.PrefabInstanceEnabled = scene.IsComponentEnabled<PrefabInstanceComponent>(entityId);
					snapshot.PrefabInstance = *prefab;
				}
				break;
			case SceneComponentKind::SceneReference:
				if (const SceneReferenceComponent* sceneReference = scene.GetSceneReferenceComponent(entityId))
				{
					snapshot.HasSceneReference = true;
					snapshot.SceneReferenceEnabled = scene.IsComponentEnabled<SceneReferenceComponent>(entityId);
					snapshot.SceneReference = *sceneReference;
				}
				break;
			case SceneComponentKind::Script:
				if (const ScriptComponent* script = scene.GetScriptComponent(entityId))
				{
					snapshot.HasScript = true;
					snapshot.ScriptEnabled = scene.IsComponentEnabled<ScriptComponent>(entityId);
					snapshot.Script = *script;
				}
				break;
			case SceneComponentKind::Sprite2D:
				if (const Sprite2DComponent* sprite = scene.GetSprite2DComponent(entityId))
				{
					snapshot.HasSprite2D = true;
					snapshot.Sprite2DEnabled = scene.IsComponentEnabled<Sprite2DComponent>(entityId);
					snapshot.Sprite2D = *sprite;
				}
				break;
			case SceneComponentKind::UiElement:
				if (const UiElementComponent* ui = scene.GetUiElementComponent(entityId))
				{
					snapshot.HasUiElement = true;
					snapshot.UiElementEnabled = scene.IsComponentEnabled<UiElementComponent>(entityId);
					snapshot.UiElement = *ui;
				}
				break;
			case SceneComponentKind::AudioSource:
				if (const AudioSourceComponent* audio = scene.GetAudioSourceComponent(entityId))
				{
					snapshot.HasAudioSource = true;
					snapshot.AudioSourceEnabled = scene.IsComponentEnabled<AudioSourceComponent>(entityId);
					snapshot.AudioSource = *audio;
				}
				break;
			case SceneComponentKind::NavigationAgent:
				if (const NavigationAgentComponent* navigation = scene.GetNavigationAgentComponent(entityId))
				{
					snapshot.HasNavigationAgent = true;
					snapshot.NavigationAgentEnabled = scene.IsComponentEnabled<NavigationAgentComponent>(entityId);
					snapshot.NavigationAgent = *navigation;
				}
				break;
			case SceneComponentKind::NetworkIdentity:
				if (const NetworkIdentityComponent* network = scene.GetNetworkIdentityComponent(entityId))
				{
					snapshot.HasNetworkIdentity = true;
					snapshot.NetworkIdentityEnabled = scene.IsComponentEnabled<NetworkIdentityComponent>(entityId);
					snapshot.NetworkIdentity = *network;
				}
				break;
			default:
				break;
			}

			return SnapshotHasComponent(kind, snapshot);
		}

		[[nodiscard]] std::string FormatBool(bool value)
		{
			return value ? "true" : "false";
		}

		[[nodiscard]] std::string FormatFloat2(const DirectX::XMFLOAT2& value)
		{
			return std::format("{:.3f}, {:.3f}", value.x, value.y);
		}

		[[nodiscard]] std::string FormatFloat3(const DirectX::XMFLOAT3& value)
		{
			return std::format("{:.3f}, {:.3f}, {:.3f}", value.x, value.y, value.z);
		}

		[[nodiscard]] std::string FormatFloat4(const DirectX::XMFLOAT4& value)
		{
			return std::format("{:.3f}, {:.3f}, {:.3f}, {:.3f}", value.x, value.y, value.z, value.w);
		}

		[[nodiscard]] std::string FormatPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string("<none>") : path.lexically_normal().string();
		}

		[[nodiscard]] constexpr const char* ScriptLanguageName(ScriptLanguage language) noexcept
		{
			switch (language)
			{
			case ScriptLanguage::Native:
				return "Native";
			case ScriptLanguage::Lua:
				return "Lua";
			case ScriptLanguage::CSharpLike:
				return "CSharp-like";
			case ScriptLanguage::GDScriptLike:
				return "GDScript-like";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] constexpr const char* PrimitiveMeshKindName(Asset::PrimitiveMeshKind kind) noexcept
		{
			switch (kind)
			{
			case Asset::PrimitiveMeshKind::Cube:
				return "Cube";
			case Asset::PrimitiveMeshKind::Sphere:
				return "Sphere";
			case Asset::PrimitiveMeshKind::Capsule:
				return "Capsule";
			case Asset::PrimitiveMeshKind::Plane:
				return "Plane";
			case Asset::PrimitiveMeshKind::None:
			default:
				return "None";
			}
		}

		[[nodiscard]] constexpr const char* UiElementKindName(UiElementKind kind) noexcept
		{
			switch (kind)
			{
			case UiElementKind::Panel:
				return "Panel";
			case UiElementKind::Label:
				return "Label";
			case UiElementKind::Button:
				return "Button";
			case UiElementKind::Image:
				return "Image";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] bool SnapshotComponentEnabled(
			SceneComponentKind kind,
			const ScenePersistence::LoadedSceneEntity& snapshot,
			bool& enabled) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				enabled = snapshot.MeshEnabled;
				return snapshot.HasMesh;
			case SceneComponentKind::Animator:
				enabled = snapshot.AnimatorEnabled;
				return snapshot.HasAnimator;
			case SceneComponentKind::Camera:
				enabled = snapshot.CameraEnabled;
				return snapshot.HasCamera;
			case SceneComponentKind::Light:
				enabled = snapshot.LightEnabled;
				return snapshot.HasLight;
			case SceneComponentKind::RigidBody:
				enabled = snapshot.RigidBodyEnabled;
				return snapshot.HasRigidBody;
			case SceneComponentKind::Collider:
				enabled = snapshot.ColliderEnabled;
				return snapshot.HasCollider;
			case SceneComponentKind::PhysicsMaterial:
				enabled = snapshot.PhysicsMaterialEnabled;
				return snapshot.HasPhysicsMaterial;
			case SceneComponentKind::PrefabInstance:
				enabled = snapshot.PrefabInstanceEnabled;
				return snapshot.HasPrefabInstance;
			case SceneComponentKind::SceneReference:
				enabled = snapshot.SceneReferenceEnabled;
				return snapshot.HasSceneReference;
			case SceneComponentKind::Script:
				enabled = snapshot.ScriptEnabled;
				return snapshot.HasScript;
			case SceneComponentKind::Sprite2D:
				enabled = snapshot.Sprite2DEnabled;
				return snapshot.HasSprite2D;
			case SceneComponentKind::UiElement:
				enabled = snapshot.UiElementEnabled;
				return snapshot.HasUiElement;
			case SceneComponentKind::AudioSource:
				enabled = snapshot.AudioSourceEnabled;
				return snapshot.HasAudioSource;
			case SceneComponentKind::NavigationAgent:
				enabled = snapshot.NavigationAgentEnabled;
				return snapshot.HasNavigationAgent;
			case SceneComponentKind::NetworkIdentity:
				enabled = snapshot.NetworkIdentityEnabled;
				return snapshot.HasNetworkIdentity;
			default:
				return false;
			}
		}

		[[nodiscard]] std::string SnapshotPropertyValue(
			SceneComponentKind kind,
			std::string_view propertyName,
			const ScenePersistence::LoadedSceneEntity& snapshot)
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
				if (propertyName == "Asset") { return FormatPath(snapshot.MeshAssetPath); }
				if (propertyName == "PrimitiveKind") { return PrimitiveMeshKindName(snapshot.PrimitiveKind); }
				if (propertyName == "MaterialCount") { return std::to_string(snapshot.MaterialOverrides.size()); }
				if (propertyName == "Materials") { return std::format("{} material override(s)", snapshot.MaterialOverrides.size()); }
				break;
			case SceneComponentKind::Animator:
				if (propertyName == "CurrentClipIndex") { return std::to_string(snapshot.Animator.CurrentClipIndex); }
				if (propertyName == "TimeSeconds") { return std::format("{:.3f}", snapshot.Animator.TimeSeconds); }
				if (propertyName == "Speed") { return std::format("{:.3f}", snapshot.Animator.Speed); }
				if (propertyName == "Playing") { return FormatBool(snapshot.Animator.Playing); }
				if (propertyName == "Loop") { return FormatBool(snapshot.Animator.Loop); }
				break;
			case SceneComponentKind::Camera:
				if (propertyName == "FovY") { return std::format("{:.3f}", snapshot.Camera.FovY); }
				if (propertyName == "NearZ") { return std::format("{:.3f}", snapshot.Camera.NearZ); }
				if (propertyName == "FarZ") { return std::format("{:.3f}", snapshot.Camera.FarZ); }
				if (propertyName == "IsGameCamera") { return FormatBool(snapshot.Camera.IsGameCamera); }
				break;
			case SceneComponentKind::Light:
				if (propertyName == "Type") { return LightTypeName(snapshot.Light.Type); }
				if (propertyName == "Color") { return FormatFloat3(snapshot.Light.Color); }
				if (propertyName == "Intensity") { return std::format("{:.3f}", snapshot.Light.Intensity); }
				if (propertyName == "Range") { return std::format("{:.3f}", snapshot.Light.Range); }
				if (propertyName == "SpotAngle") { return std::format("{:.3f}", snapshot.Light.SpotAngle); }
				if (propertyName == "Enabled") { return FormatBool(snapshot.Light.Enabled); }
				if (propertyName == "CastShadows") { return FormatBool(snapshot.Light.CastShadows); }
				break;
			case SceneComponentKind::RigidBody:
				if (propertyName == "Type") { return std::string(Physics::ToString(snapshot.RigidBody.Type)); }
				if (propertyName == "Mass") { return std::format("{:.3f}", snapshot.RigidBody.Mass); }
				if (propertyName == "UseGravity") { return FormatBool(snapshot.RigidBody.UseGravity); }
				if (propertyName == "LinearVelocity") { return FormatFloat3(snapshot.RigidBody.LinearVelocity); }
				if (propertyName == "AngularVelocity") { return FormatFloat3(snapshot.RigidBody.AngularVelocity); }
				break;
			case SceneComponentKind::Collider:
				if (propertyName == "Shape") { return std::string(Physics::ToString(snapshot.Collider.Shape)); }
				if (propertyName == "Size") { return FormatFloat3(snapshot.Collider.Size); }
				if (propertyName == "Radius") { return std::format("{:.3f}", snapshot.Collider.Radius); }
				if (propertyName == "Height") { return std::format("{:.3f}", snapshot.Collider.Height); }
				if (propertyName == "Offset") { return FormatFloat3(snapshot.Collider.Offset); }
				if (propertyName == "IsTrigger") { return FormatBool(snapshot.Collider.IsTrigger); }
				break;
			case SceneComponentKind::PhysicsMaterial:
				if (propertyName == "StaticFriction") { return std::format("{:.3f}", snapshot.PhysicsMaterial.StaticFriction); }
				if (propertyName == "DynamicFriction") { return std::format("{:.3f}", snapshot.PhysicsMaterial.DynamicFriction); }
				if (propertyName == "Restitution") { return std::format("{:.3f}", snapshot.PhysicsMaterial.Restitution); }
				break;
			case SceneComponentKind::PrefabInstance:
				if (propertyName == "PrefabPath") { return FormatPath(snapshot.PrefabInstance.PrefabPath); }
				if (propertyName == "SourceName") { return snapshot.PrefabInstance.SourceName; }
				if (propertyName == "TrackPrefabOverrides") { return FormatBool(snapshot.PrefabInstance.TrackPrefabOverrides); }
				break;
			case SceneComponentKind::SceneReference:
				if (propertyName == "ScenePath") { return FormatPath(snapshot.SceneReference.ScenePath); }
				if (propertyName == "LoadAdditively") { return FormatBool(snapshot.SceneReference.LoadAdditively); }
				if (propertyName == "AutoLoad") { return FormatBool(snapshot.SceneReference.AutoLoad); }
				break;
			case SceneComponentKind::Script:
				if (propertyName == "ScriptPath") { return FormatPath(snapshot.Script.ScriptPath); }
				if (propertyName == "ClassName") { return snapshot.Script.ClassName; }
				if (propertyName == "Language") { return ScriptLanguageName(snapshot.Script.Language); }
				if (propertyName == "RunInEditor") { return FormatBool(snapshot.Script.RunInEditor); }
				break;
			case SceneComponentKind::Sprite2D:
				if (propertyName == "TexturePath") { return FormatPath(snapshot.Sprite2D.TexturePath); }
				if (propertyName == "Color") { return FormatFloat4(snapshot.Sprite2D.Color); }
				if (propertyName == "Size") { return FormatFloat2(snapshot.Sprite2D.Size); }
				if (propertyName == "Pivot") { return FormatFloat2(snapshot.Sprite2D.Pivot); }
				if (propertyName == "SortingLayer") { return std::to_string(snapshot.Sprite2D.SortingLayer); }
				if (propertyName == "OrderInLayer") { return std::to_string(snapshot.Sprite2D.OrderInLayer); }
				break;
			case SceneComponentKind::UiElement:
				if (propertyName == "Kind") { return UiElementKindName(snapshot.UiElement.Kind); }
				if (propertyName == "Text") { return snapshot.UiElement.Text; }
				if (propertyName == "AnchorMin") { return FormatFloat2(snapshot.UiElement.AnchorMin); }
				if (propertyName == "AnchorMax") { return FormatFloat2(snapshot.UiElement.AnchorMax); }
				if (propertyName == "Position") { return FormatFloat2(snapshot.UiElement.Position); }
				if (propertyName == "Size") { return FormatFloat2(snapshot.UiElement.Size); }
				if (propertyName == "Color") { return FormatFloat4(snapshot.UiElement.Color); }
				break;
			case SceneComponentKind::AudioSource:
				if (propertyName == "ClipPath") { return FormatPath(snapshot.AudioSource.ClipPath); }
				if (propertyName == "Volume") { return std::format("{:.3f}", snapshot.AudioSource.Volume); }
				if (propertyName == "Pitch") { return std::format("{:.3f}", snapshot.AudioSource.Pitch); }
				if (propertyName == "Loop") { return FormatBool(snapshot.AudioSource.Loop); }
				if (propertyName == "PlayOnStart") { return FormatBool(snapshot.AudioSource.PlayOnStart); }
				if (propertyName == "Spatialize") { return FormatBool(snapshot.AudioSource.Spatialize); }
				break;
			case SceneComponentKind::NavigationAgent:
				if (propertyName == "Radius") { return std::format("{:.3f}", snapshot.NavigationAgent.Radius); }
				if (propertyName == "Height") { return std::format("{:.3f}", snapshot.NavigationAgent.Height); }
				if (propertyName == "Speed") { return std::format("{:.3f}", snapshot.NavigationAgent.Speed); }
				if (propertyName == "Acceleration") { return std::format("{:.3f}", snapshot.NavigationAgent.Acceleration); }
				if (propertyName == "Target") { return FormatFloat3(snapshot.NavigationAgent.Target); }
				if (propertyName == "HasTarget") { return FormatBool(snapshot.NavigationAgent.HasTarget); }
				break;
			case SceneComponentKind::NetworkIdentity:
				if (propertyName == "NetworkId") { return std::to_string(snapshot.NetworkIdentity.NetworkId); }
				if (propertyName == "PrefabKey") { return snapshot.NetworkIdentity.PrefabKey; }
				if (propertyName == "ReplicateTransform") { return FormatBool(snapshot.NetworkIdentity.ReplicateTransform); }
				if (propertyName == "ServerAuthoritative") { return FormatBool(snapshot.NetworkIdentity.ServerAuthoritative); }
				break;
			default:
				break;
			}
			return {};
		}

		[[nodiscard]] bool CopySnapshotPropertyFromSource(
			SceneComponentKind kind,
			std::string_view propertyName,
			ScenePersistence::LoadedSceneEntity& target,
			const ScenePersistence::LoadedSceneEntity& source) noexcept
		{
			if (propertyName == "Component Enabled")
			{
				switch (kind)
				{
				case SceneComponentKind::Mesh: target.MeshEnabled = source.MeshEnabled; return true;
				case SceneComponentKind::Animator: target.AnimatorEnabled = source.AnimatorEnabled; return true;
				case SceneComponentKind::Camera: target.CameraEnabled = source.CameraEnabled; return true;
				case SceneComponentKind::Light: target.LightEnabled = source.LightEnabled; return true;
				case SceneComponentKind::RigidBody: target.RigidBodyEnabled = source.RigidBodyEnabled; return true;
				case SceneComponentKind::Collider: target.ColliderEnabled = source.ColliderEnabled; return true;
				case SceneComponentKind::PhysicsMaterial: target.PhysicsMaterialEnabled = source.PhysicsMaterialEnabled; return true;
				case SceneComponentKind::PrefabInstance: target.PrefabInstanceEnabled = source.PrefabInstanceEnabled; return true;
				case SceneComponentKind::SceneReference: target.SceneReferenceEnabled = source.SceneReferenceEnabled; return true;
				case SceneComponentKind::Script: target.ScriptEnabled = source.ScriptEnabled; return true;
				case SceneComponentKind::Sprite2D: target.Sprite2DEnabled = source.Sprite2DEnabled; return true;
				case SceneComponentKind::UiElement: target.UiElementEnabled = source.UiElementEnabled; return true;
				case SceneComponentKind::AudioSource: target.AudioSourceEnabled = source.AudioSourceEnabled; return true;
				case SceneComponentKind::NavigationAgent: target.NavigationAgentEnabled = source.NavigationAgentEnabled; return true;
				case SceneComponentKind::NetworkIdentity: target.NetworkIdentityEnabled = source.NetworkIdentityEnabled; return true;
				default: return false;
				}
			}

			switch (kind)
			{
			case SceneComponentKind::Animator:
				if (propertyName == "CurrentClipIndex") { target.Animator.CurrentClipIndex = source.Animator.CurrentClipIndex; return true; }
				if (propertyName == "TimeSeconds") { target.Animator.TimeSeconds = source.Animator.TimeSeconds; return true; }
				if (propertyName == "Speed") { target.Animator.Speed = source.Animator.Speed; return true; }
				if (propertyName == "Playing") { target.Animator.Playing = source.Animator.Playing; return true; }
				if (propertyName == "Loop") { target.Animator.Loop = source.Animator.Loop; return true; }
				break;
			case SceneComponentKind::Camera:
				if (propertyName == "FovY") { target.Camera.FovY = source.Camera.FovY; return true; }
				if (propertyName == "NearZ") { target.Camera.NearZ = source.Camera.NearZ; return true; }
				if (propertyName == "FarZ") { target.Camera.FarZ = source.Camera.FarZ; return true; }
				if (propertyName == "IsGameCamera") { target.Camera.IsGameCamera = source.Camera.IsGameCamera; return true; }
				break;
			case SceneComponentKind::Light:
				if (propertyName == "Type") { target.Light.Type = source.Light.Type; return true; }
				if (propertyName == "Color") { target.Light.Color = source.Light.Color; return true; }
				if (propertyName == "Intensity") { target.Light.Intensity = source.Light.Intensity; return true; }
				if (propertyName == "Range") { target.Light.Range = source.Light.Range; return true; }
				if (propertyName == "SpotAngle") { target.Light.SpotAngle = source.Light.SpotAngle; return true; }
				if (propertyName == "Enabled") { target.Light.Enabled = source.Light.Enabled; return true; }
				if (propertyName == "CastShadows") { target.Light.CastShadows = source.Light.CastShadows; return true; }
				break;
			case SceneComponentKind::RigidBody:
				if (propertyName == "Type") { target.RigidBody.Type = source.RigidBody.Type; return true; }
				if (propertyName == "Mass") { target.RigidBody.Mass = source.RigidBody.Mass; return true; }
				if (propertyName == "UseGravity") { target.RigidBody.UseGravity = source.RigidBody.UseGravity; return true; }
				if (propertyName == "LinearVelocity") { target.RigidBody.LinearVelocity = source.RigidBody.LinearVelocity; return true; }
				if (propertyName == "AngularVelocity") { target.RigidBody.AngularVelocity = source.RigidBody.AngularVelocity; return true; }
				break;
			case SceneComponentKind::Collider:
				if (propertyName == "Shape") { target.Collider.Shape = source.Collider.Shape; return true; }
				if (propertyName == "Size") { target.Collider.Size = source.Collider.Size; return true; }
				if (propertyName == "Radius") { target.Collider.Radius = source.Collider.Radius; return true; }
				if (propertyName == "Height") { target.Collider.Height = source.Collider.Height; return true; }
				if (propertyName == "Offset") { target.Collider.Offset = source.Collider.Offset; return true; }
				if (propertyName == "IsTrigger") { target.Collider.IsTrigger = source.Collider.IsTrigger; return true; }
				break;
			case SceneComponentKind::PhysicsMaterial:
				if (propertyName == "StaticFriction") { target.PhysicsMaterial.StaticFriction = source.PhysicsMaterial.StaticFriction; return true; }
				if (propertyName == "DynamicFriction") { target.PhysicsMaterial.DynamicFriction = source.PhysicsMaterial.DynamicFriction; return true; }
				if (propertyName == "Restitution") { target.PhysicsMaterial.Restitution = source.PhysicsMaterial.Restitution; return true; }
				break;
			case SceneComponentKind::SceneReference:
				if (propertyName == "ScenePath") { target.SceneReference.ScenePath = source.SceneReference.ScenePath; return true; }
				if (propertyName == "LoadAdditively") { target.SceneReference.LoadAdditively = source.SceneReference.LoadAdditively; return true; }
				if (propertyName == "AutoLoad") { target.SceneReference.AutoLoad = source.SceneReference.AutoLoad; return true; }
				break;
			case SceneComponentKind::Script:
				if (propertyName == "ScriptPath") { target.Script.ScriptPath = source.Script.ScriptPath; return true; }
				if (propertyName == "ClassName") { target.Script.ClassName = source.Script.ClassName; return true; }
				if (propertyName == "Language") { target.Script.Language = source.Script.Language; return true; }
				if (propertyName == "RunInEditor") { target.Script.RunInEditor = source.Script.RunInEditor; return true; }
				break;
			case SceneComponentKind::Sprite2D:
				if (propertyName == "TexturePath") { target.Sprite2D.TexturePath = source.Sprite2D.TexturePath; return true; }
				if (propertyName == "Color") { target.Sprite2D.Color = source.Sprite2D.Color; return true; }
				if (propertyName == "Size") { target.Sprite2D.Size = source.Sprite2D.Size; return true; }
				if (propertyName == "Pivot") { target.Sprite2D.Pivot = source.Sprite2D.Pivot; return true; }
				if (propertyName == "SortingLayer") { target.Sprite2D.SortingLayer = source.Sprite2D.SortingLayer; return true; }
				if (propertyName == "OrderInLayer") { target.Sprite2D.OrderInLayer = source.Sprite2D.OrderInLayer; return true; }
				break;
			case SceneComponentKind::UiElement:
				if (propertyName == "Kind") { target.UiElement.Kind = source.UiElement.Kind; return true; }
				if (propertyName == "Text") { target.UiElement.Text = source.UiElement.Text; return true; }
				if (propertyName == "AnchorMin") { target.UiElement.AnchorMin = source.UiElement.AnchorMin; return true; }
				if (propertyName == "AnchorMax") { target.UiElement.AnchorMax = source.UiElement.AnchorMax; return true; }
				if (propertyName == "Position") { target.UiElement.Position = source.UiElement.Position; return true; }
				if (propertyName == "Size") { target.UiElement.Size = source.UiElement.Size; return true; }
				if (propertyName == "Color") { target.UiElement.Color = source.UiElement.Color; return true; }
				break;
			case SceneComponentKind::AudioSource:
				if (propertyName == "ClipPath") { target.AudioSource.ClipPath = source.AudioSource.ClipPath; return true; }
				if (propertyName == "Volume") { target.AudioSource.Volume = source.AudioSource.Volume; return true; }
				if (propertyName == "Pitch") { target.AudioSource.Pitch = source.AudioSource.Pitch; return true; }
				if (propertyName == "Loop") { target.AudioSource.Loop = source.AudioSource.Loop; return true; }
				if (propertyName == "PlayOnStart") { target.AudioSource.PlayOnStart = source.AudioSource.PlayOnStart; return true; }
				if (propertyName == "Spatialize") { target.AudioSource.Spatialize = source.AudioSource.Spatialize; return true; }
				break;
			case SceneComponentKind::NavigationAgent:
				if (propertyName == "Radius") { target.NavigationAgent.Radius = source.NavigationAgent.Radius; return true; }
				if (propertyName == "Height") { target.NavigationAgent.Height = source.NavigationAgent.Height; return true; }
				if (propertyName == "Speed") { target.NavigationAgent.Speed = source.NavigationAgent.Speed; return true; }
				if (propertyName == "Acceleration") { target.NavigationAgent.Acceleration = source.NavigationAgent.Acceleration; return true; }
				if (propertyName == "Target") { target.NavigationAgent.Target = source.NavigationAgent.Target; return true; }
				if (propertyName == "HasTarget") { target.NavigationAgent.HasTarget = source.NavigationAgent.HasTarget; return true; }
				break;
			case SceneComponentKind::NetworkIdentity:
				if (propertyName == "NetworkId") { target.NetworkIdentity.NetworkId = source.NetworkIdentity.NetworkId; return true; }
				if (propertyName == "PrefabKey") { target.NetworkIdentity.PrefabKey = source.NetworkIdentity.PrefabKey; return true; }
				if (propertyName == "ReplicateTransform") { target.NetworkIdentity.ReplicateTransform = source.NetworkIdentity.ReplicateTransform; return true; }
				if (propertyName == "ServerAuthoritative") { target.NetworkIdentity.ServerAuthoritative = source.NetworkIdentity.ServerAuthoritative; return true; }
				break;
			default:
				break;
			}
			return false;
		}

		[[nodiscard]] bool CopySnapshotComponentFromSource(
			SceneComponentKind kind,
			ScenePersistence::LoadedSceneEntity& target,
			const ScenePersistence::LoadedSceneEntity& source)
		{
			if (!SnapshotHasComponent(kind, source))
			{
				return false;
			}

			switch (kind)
			{
			case SceneComponentKind::Animator:
				target.HasAnimator = true;
				target.AnimatorEnabled = source.AnimatorEnabled;
				target.Animator = source.Animator;
				return true;
			case SceneComponentKind::Camera:
				target.HasCamera = true;
				target.CameraEnabled = source.CameraEnabled;
				target.Camera = source.Camera;
				return true;
			case SceneComponentKind::Light:
				target.HasLight = true;
				target.LightEnabled = source.LightEnabled;
				target.Light = source.Light;
				return true;
			case SceneComponentKind::RigidBody:
				target.HasRigidBody = true;
				target.RigidBodyEnabled = source.RigidBodyEnabled;
				target.RigidBody = source.RigidBody;
				return true;
			case SceneComponentKind::Collider:
				target.HasCollider = true;
				target.ColliderEnabled = source.ColliderEnabled;
				target.Collider = source.Collider;
				return true;
			case SceneComponentKind::PhysicsMaterial:
				target.HasPhysicsMaterial = true;
				target.PhysicsMaterialEnabled = source.PhysicsMaterialEnabled;
				target.PhysicsMaterial = source.PhysicsMaterial;
				return true;
			case SceneComponentKind::SceneReference:
				target.HasSceneReference = true;
				target.SceneReferenceEnabled = source.SceneReferenceEnabled;
				target.SceneReference = source.SceneReference;
				return true;
			case SceneComponentKind::Script:
				target.HasScript = true;
				target.ScriptEnabled = source.ScriptEnabled;
				target.Script = source.Script;
				return true;
			case SceneComponentKind::Sprite2D:
				target.HasSprite2D = true;
				target.Sprite2DEnabled = source.Sprite2DEnabled;
				target.Sprite2D = source.Sprite2D;
				return true;
			case SceneComponentKind::UiElement:
				target.HasUiElement = true;
				target.UiElementEnabled = source.UiElementEnabled;
				target.UiElement = source.UiElement;
				return true;
			case SceneComponentKind::AudioSource:
				target.HasAudioSource = true;
				target.AudioSourceEnabled = source.AudioSourceEnabled;
				target.AudioSource = source.AudioSource;
				return true;
			case SceneComponentKind::NavigationAgent:
				target.HasNavigationAgent = true;
				target.NavigationAgentEnabled = source.NavigationAgentEnabled;
				target.NavigationAgent = source.NavigationAgent;
				return true;
			case SceneComponentKind::NetworkIdentity:
				target.HasNetworkIdentity = true;
				target.NetworkIdentityEnabled = source.NetworkIdentityEnabled;
				target.NetworkIdentity = source.NetworkIdentity;
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] bool RemoveSnapshotComponent(
			SceneComponentKind kind,
			ScenePersistence::LoadedSceneEntity& target) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Animator:
				target.HasAnimator = false;
				target.AnimatorEnabled = true;
				target.Animator = {};
				return true;
			case SceneComponentKind::Camera:
				target.HasCamera = false;
				target.CameraEnabled = true;
				target.Camera = {};
				return true;
			case SceneComponentKind::Light:
				target.HasLight = false;
				target.LightEnabled = true;
				target.Light = {};
				return true;
			case SceneComponentKind::RigidBody:
				target.HasRigidBody = false;
				target.RigidBodyEnabled = true;
				target.RigidBody = {};
				return true;
			case SceneComponentKind::Collider:
				target.HasCollider = false;
				target.ColliderEnabled = true;
				target.Collider = {};
				return true;
			case SceneComponentKind::PhysicsMaterial:
				target.HasPhysicsMaterial = false;
				target.PhysicsMaterialEnabled = true;
				target.PhysicsMaterial = {};
				return true;
			case SceneComponentKind::SceneReference:
				target.HasSceneReference = false;
				target.SceneReferenceEnabled = true;
				target.SceneReference = {};
				return true;
			case SceneComponentKind::Script:
				target.HasScript = false;
				target.ScriptEnabled = true;
				target.Script = {};
				return true;
			case SceneComponentKind::Sprite2D:
				target.HasSprite2D = false;
				target.Sprite2DEnabled = true;
				target.Sprite2D = {};
				return true;
			case SceneComponentKind::UiElement:
				target.HasUiElement = false;
				target.UiElementEnabled = true;
				target.UiElement = {};
				return true;
			case SceneComponentKind::AudioSource:
				target.HasAudioSource = false;
				target.AudioSourceEnabled = true;
				target.AudioSource = {};
				return true;
			case SceneComponentKind::NavigationAgent:
				target.HasNavigationAgent = false;
				target.NavigationAgentEnabled = true;
				target.NavigationAgent = {};
				return true;
			case SceneComponentKind::NetworkIdentity:
				target.HasNetworkIdentity = false;
				target.NetworkIdentityEnabled = true;
				target.NetworkIdentity = {};
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] constexpr bool CanRevertPrefabOverrideComponent(SceneComponentKind kind) noexcept
		{
			return kind != SceneComponentKind::Mesh &&
				kind != SceneComponentKind::PrefabInstance;
		}

		[[nodiscard]] bool Float3NearlyEqual(const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs) noexcept
		{
			return NearlyEqual(lhs.x, rhs.x) &&
				NearlyEqual(lhs.y, rhs.y) &&
				NearlyEqual(lhs.z, rhs.z);
		}

		[[nodiscard]] bool Float4NearlyEqual(const DirectX::XMFLOAT4& lhs, const DirectX::XMFLOAT4& rhs) noexcept
		{
			return NearlyEqual(lhs.x, rhs.x) &&
				NearlyEqual(lhs.y, rhs.y) &&
				NearlyEqual(lhs.z, rhs.z) &&
				NearlyEqual(lhs.w, rhs.w);
		}

		[[nodiscard]] std::string MaterialTextureSourceText(const Asset::StaticMeshMaterial& material, Asset::MaterialTextureSlot slot)
		{
			const std::filesystem::path path = Asset::GetMaterialTexturePath(material, slot);
			if (!path.empty())
			{
				return path.string();
			}

			const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
			if (binding.Embedded.IsValid())
			{
				return std::format("<embedded {}x{}>", binding.Embedded.Width, binding.Embedded.Height);
			}
			return "<fallback>";
		}

		[[nodiscard]] bool MaterialTextureSlotDiffers(
			const Asset::StaticMeshMaterial& current,
			const Asset::StaticMeshMaterial& prefab,
			Asset::MaterialTextureSlot slot)
		{
			const std::filesystem::path currentPath = Asset::GetMaterialTexturePath(current, slot);
			const std::filesystem::path prefabPath = Asset::GetMaterialTexturePath(prefab, slot);
			if (currentPath.empty() != prefabPath.empty())
			{
				return true;
			}
			if (!currentPath.empty() && !SamePath(currentPath, prefabPath))
			{
				return true;
			}

			const Asset::MaterialTextureBinding& currentBinding = Asset::GetMaterialTextureBinding(current, slot);
			const Asset::MaterialTextureBinding& prefabBinding = Asset::GetMaterialTextureBinding(prefab, slot);
			if (currentBinding.IsOverride != prefabBinding.IsOverride)
			{
				return true;
			}

			const bool currentEmbedded = currentBinding.Embedded.IsValid();
			const bool prefabEmbedded = prefabBinding.Embedded.IsValid();
			if (currentEmbedded != prefabEmbedded)
			{
				return true;
			}
			if (currentEmbedded)
			{
				return currentBinding.Embedded.Width != prefabBinding.Embedded.Width ||
					currentBinding.Embedded.Height != prefabBinding.Embedded.Height ||
					currentBinding.Embedded.Pixels.size() != prefabBinding.Embedded.Pixels.size();
			}
			return false;
		}

		[[nodiscard]] std::vector<std::string> CollectMaterialScalarOverrideLabels(
			const Asset::StaticMeshMaterial& current,
			const Asset::StaticMeshMaterial& prefab)
		{
			std::vector<std::string> labels;
			const auto addIf = [&labels](bool changed, const char* label)
			{
				if (changed)
				{
					labels.emplace_back(label);
				}
			};

			addIf(current.Name != prefab.Name, "Name");
			addIf(current.ShadingModel != prefab.ShadingModel, "Shading Model");
			addIf(!Float4NearlyEqual(current.DiffuseColor, prefab.DiffuseColor), "Base Color");
			addIf(!Float3NearlyEqual(current.SpecularColor, prefab.SpecularColor), "Specular");
			addIf(!Float3NearlyEqual(current.EmissiveColor, prefab.EmissiveColor), "Emissive");
			addIf(!NearlyEqual(current.MetallicFactor, prefab.MetallicFactor), "Metallic");
			addIf(!NearlyEqual(current.RoughnessFactor, prefab.RoughnessFactor), "Roughness");
			addIf(!NearlyEqual(current.Shininess, prefab.Shininess), "Shininess");
			addIf(!NearlyEqual(current.Opacity, prefab.Opacity), "Opacity");
			addIf(current.UseVertexColor != prefab.UseVertexColor, "Use Vertex Color");
			addIf(current.NormalYFlip != prefab.NormalYFlip, "Normal Y Flip");
			return labels;
		}

		void CopyMaterialScalarProperties(Asset::StaticMeshMaterial& target, const Asset::StaticMeshMaterial& source) noexcept
		{
			target.ShadingModel = source.ShadingModel;
			target.DiffuseColor = source.DiffuseColor;
			target.SpecularColor = source.SpecularColor;
			target.EmissiveColor = source.EmissiveColor;
			target.MetallicFactor = source.MetallicFactor;
			target.RoughnessFactor = source.RoughnessFactor;
			target.Shininess = source.Shininess;
			target.Opacity = source.Opacity;
			target.UseVertexColor = source.UseVertexColor;
			target.NormalYFlip = source.NormalYFlip;
		}

		[[nodiscard]] bool MaterialScalarPropertiesDiffer(
			const Asset::StaticMeshMaterial& lhs,
			const Asset::StaticMeshMaterial& rhs) noexcept
		{
			return lhs.ShadingModel != rhs.ShadingModel ||
				!Float4NearlyEqual(lhs.DiffuseColor, rhs.DiffuseColor) ||
				!Float3NearlyEqual(lhs.SpecularColor, rhs.SpecularColor) ||
				!Float3NearlyEqual(lhs.EmissiveColor, rhs.EmissiveColor) ||
				!NearlyEqual(lhs.MetallicFactor, rhs.MetallicFactor) ||
				!NearlyEqual(lhs.RoughnessFactor, rhs.RoughnessFactor) ||
				!NearlyEqual(lhs.Shininess, rhs.Shininess) ||
				!NearlyEqual(lhs.Opacity, rhs.Opacity) ||
				lhs.UseVertexColor != rhs.UseVertexColor ||
				lhs.NormalYFlip != rhs.NormalYFlip;
		}

		[[nodiscard]] std::vector<Asset::MaterialTextureSlot> CollectMaterialTextureOverrideSlots(
			const Asset::StaticMeshMaterial& current,
			const Asset::StaticMeshMaterial& prefab)
		{
			std::vector<Asset::MaterialTextureSlot> slots;
			for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
			{
				const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
				if (MaterialTextureSlotDiffers(current, prefab, slot))
				{
					slots.push_back(slot);
				}
			}
			return slots;
		}

		template <typename Component>
		void DrawAddComponentMenuItem(
			EditorContext& context,
			EntityId entityId,
			SceneComponentKind kind,
			const char* label,
			bool enabled = true,
			const char* disabledReason = nullptr)
		{
			if (context.ActiveScene.HasComponent<Component>(entityId))
			{
				return;
			}

			if (ImGui::MenuItem(label, nullptr, false, enabled) && context.OnComponentAdded)
			{
				context.OnComponentAdded(entityId, kind);
			}
			if (!enabled && disabledReason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			{
				ImGui::SetTooltip("%s", disabledReason);
			}
		}

		[[nodiscard]] bool ShouldShowMaterialSlot(Asset::MaterialShadingModel model, Asset::MaterialTextureSlot slot) noexcept
		{
			switch (slot)
			{
			case Asset::MaterialTextureSlot::BaseColor:
			case Asset::MaterialTextureSlot::Normal:
			case Asset::MaterialTextureSlot::Emissive:
			case Asset::MaterialTextureSlot::Opacity:
				return true;
			case Asset::MaterialTextureSlot::Metallic:
			case Asset::MaterialTextureSlot::Roughness:
			case Asset::MaterialTextureSlot::MetallicRoughness:
			case Asset::MaterialTextureSlot::AO:
				return model == Asset::MaterialShadingModel::PBR;
			case Asset::MaterialTextureSlot::Specular:
			case Asset::MaterialTextureSlot::Shininess:
				return model == Asset::MaterialShadingModel::Phong;
			case Asset::MaterialTextureSlot::Count:
			default:
				return false;
			}
		}

		[[nodiscard]] bool IsImageAssetPath(const std::filesystem::path& path)
		{
			return Asset::ClassifyAssetPath(path) == Asset::AssetFileKind::Image;
		}

		[[nodiscard]] std::filesystem::path ResolveEditorAssetPath(
			const std::filesystem::path& path,
			const std::shared_ptr<const Asset::AssetFileSnapshot>& snapshot)
		{
			if (path.empty() || path.is_absolute())
			{
				return path.lexically_normal();
			}

			if (snapshot && snapshot->RootExists)
			{
				std::error_code errorCode;
				const std::filesystem::path assetRelative = (snapshot->RootPath / path).lexically_normal();
				if (std::filesystem::exists(assetRelative, errorCode))
				{
					return assetRelative;
				}
				errorCode.clear();
				const std::filesystem::path projectRelative = (snapshot->RootPath.parent_path() / path).lexically_normal();
				if (std::filesystem::exists(projectRelative, errorCode))
				{
					return projectRelative;
				}
				return assetRelative;
			}

			return std::filesystem::absolute(path).lexically_normal();
		}

		[[nodiscard]] int TextureSlotKeywordScore(Asset::MaterialTextureSlot slot, std::string_view lowerName)
		{
			const bool hasKeyword = Asset::TextureMatching::ContainsAnyKeyword(
				lowerName,
				Asset::TextureMatching::SlotKeywords(slot));

			switch (slot)
			{
			case Asset::MaterialTextureSlot::BaseColor:
				return hasKeyword ? 28 : 0;
			case Asset::MaterialTextureSlot::Normal:
				return hasKeyword ? 28 : 0;
			case Asset::MaterialTextureSlot::Metallic:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Roughness:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::MetallicRoughness:
				return hasKeyword ? 30 : 0;
			case Asset::MaterialTextureSlot::AO:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Emissive:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Opacity:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Specular:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Shininess:
				return hasKeyword ? 26 : 0;
			case Asset::MaterialTextureSlot::Count:
			default:
				return 0;
			}
		}

		[[nodiscard]] bool ContainsAnyTextureKeyword(std::string_view text, std::span<const std::string_view> keywords)
		{
			return Asset::TextureMatching::ContainsAnyKeyword(text, keywords);
		}

		[[nodiscard]] bool IsIgnoredTextureRemapToken(std::string_view token) noexcept
		{
			return Asset::TextureMatching::IsIgnoredMatchToken(token);
		}

		void AddUniqueTextureRemapToken(std::vector<std::string>& tokens, std::string token)
		{
			if (token.empty() || IsIgnoredTextureRemapToken(token))
			{
				return;
			}
			if (std::ranges::find(tokens, token) == tokens.end())
			{
				tokens.push_back(std::move(token));
			}
		}

		void AppendTextureRemapTokens(std::vector<std::string>& tokens, std::string_view text)
		{
			std::string currentToken;
			for (const char rawCharacter : ToLower(std::string(text)))
			{
				const auto character = static_cast<unsigned char>(rawCharacter);
				if (std::isalnum(character))
				{
					currentToken.push_back(static_cast<char>(character));
					continue;
				}

				AddUniqueTextureRemapToken(tokens, std::move(currentToken));
				currentToken.clear();
			}
			AddUniqueTextureRemapToken(tokens, std::move(currentToken));
		}

		[[nodiscard]] std::vector<std::string> BuildTextureRemapMatchTokens(
			const Asset::StaticMeshMaterial& material,
			const std::filesystem::path& missingPath,
			const std::filesystem::path& sourcePath)
		{
			std::vector<std::string> tokens;
			AppendTextureRemapTokens(tokens, material.Name);
			AppendTextureRemapTokens(tokens, missingPath.stem().string());
			AppendTextureRemapTokens(tokens, missingPath.parent_path().filename().string());
			AppendTextureRemapTokens(tokens, sourcePath.stem().string());
			AppendTextureRemapTokens(tokens, sourcePath.parent_path().filename().string());
			return tokens;
		}

		void AppendTextureRemapReason(std::string& reason, std::string_view addition)
		{
			if (addition.empty())
			{
				return;
			}
			if (!reason.empty())
			{
				reason.append(", ");
			}
			reason.append(addition);
		}

		[[nodiscard]] bool IsPathInsideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory)
		{
			if (path.empty() || directory.empty())
			{
				return false;
			}
			std::error_code errorCode;
			const std::filesystem::path relative = std::filesystem::relative(path.lexically_normal(), directory.lexically_normal(), errorCode);
			if (errorCode || relative.empty())
			{
				return false;
			}
			for (const auto& part : relative)
			{
				if (part == "..")
				{
					return false;
				}
			}
			return true;
		}

		struct TextureRemapCandidate
		{
			std::filesystem::path Path;
			int Score = 0;
			std::string Reason;
		};

		enum class TextureRemapConfidence : uint8_t
		{
			High,
			Medium,
			Low,
			Ambiguous
		};

		struct TextureRemapPreviewRow
		{
			size_t MaterialIndex = static_cast<size_t>(-1);
			std::string MaterialName;
			Asset::MaterialTextureSlot Slot = Asset::MaterialTextureSlot::Count;
			std::filesystem::path MissingPath;
			TextureRemapCandidate Candidate;
			std::vector<TextureRemapCandidate> Candidates;
			std::filesystem::path AlternativePath;
			int AlternativeScore = 0;
			TextureRemapConfidence Confidence = TextureRemapConfidence::Low;
			bool CandidateOverridden = false;
		};

		[[nodiscard]] std::string BuildTextureRemapOverrideKey(
			size_t materialIndex,
			Asset::MaterialTextureSlot slot,
			const std::filesystem::path& missingPath)
		{
			return std::format(
				"{}:{}:{}",
				materialIndex,
				Asset::MaterialTextureSlotIndex(slot),
				ToLower(missingPath.lexically_normal().generic_string()));
		}

		[[nodiscard]] constexpr const char* TextureRemapConfidenceName(TextureRemapConfidence confidence) noexcept
		{
			switch (confidence)
			{
			case TextureRemapConfidence::High:
				return "High";
			case TextureRemapConfidence::Medium:
				return "Medium";
			case TextureRemapConfidence::Ambiguous:
				return "Ambiguous";
			case TextureRemapConfidence::Low:
			default:
				return "Low";
			}
		}

		[[nodiscard]] constexpr ImVec4 TextureRemapConfidenceColor(TextureRemapConfidence confidence) noexcept
		{
			switch (confidence)
			{
			case TextureRemapConfidence::High:
				return ImVec4(0.42f, 0.92f, 0.52f, 1.0f);
			case TextureRemapConfidence::Medium:
				return ImVec4(0.42f, 0.72f, 1.0f, 1.0f);
			case TextureRemapConfidence::Ambiguous:
				return ImVec4(1.0f, 0.72f, 0.22f, 1.0f);
			case TextureRemapConfidence::Low:
			default:
				return ImVec4(1.0f, 0.48f, 0.34f, 1.0f);
			}
		}

		[[nodiscard]] TextureRemapConfidence EvaluateTextureRemapConfidence(const std::vector<TextureRemapCandidate>& candidates) noexcept
		{
			if (candidates.empty())
			{
				return TextureRemapConfidence::Low;
			}
			if (candidates.size() > 1 && candidates.front().Score - candidates[1].Score <= 8)
			{
				return TextureRemapConfidence::Ambiguous;
			}
			if (candidates.front().Score >= 96)
			{
				return TextureRemapConfidence::High;
			}
			if (candidates.front().Score >= 58)
			{
				return TextureRemapConfidence::Medium;
			}
			return TextureRemapConfidence::Low;
		}

		void CollectTextureRemapCandidates(
			const Asset::AssetFileEntry& entry,
			const std::filesystem::path& missingPath,
			const Asset::StaticMeshMaterial& material,
			Asset::MaterialTextureSlot slot,
			const std::filesystem::path& sourcePath,
			std::vector<TextureRemapCandidate>& candidates)
		{
			if (entry.Kind == Asset::AssetFileKind::Image)
			{
				const std::string lowerName = ToLower(entry.Name);
				const std::string lowerStem = ToLower(entry.Path.stem().string());
				const std::string lowerSearchText = ToLower((entry.Path.parent_path().filename() / entry.Path.stem()).string());
				const std::string missingName = ToLower(missingPath.filename().string());
				const std::string missingStem = ToLower(missingPath.stem().string());
				int score = TextureSlotKeywordScore(slot, lowerSearchText);
				std::string reason;
				bool exactOrStemMatch = false;

				if (!missingName.empty() && lowerName == missingName)
				{
					score += 120;
					exactOrStemMatch = true;
					AppendTextureRemapReason(reason, "same filename");
				}
				else if (!missingStem.empty() && lowerStem == missingStem)
				{
					score += 90;
					exactOrStemMatch = true;
					const std::string extension = ToLower(entry.Path.extension().string());
					AppendTextureRemapReason(reason, extension == ".png" || extension == ".tga" ? "preferred stem variant" : "same stem");
				}
				else if (!missingStem.empty() && lowerStem.find(missingStem) != std::string::npos)
				{
					score += 48;
					AppendTextureRemapReason(reason, "contains missing stem");
				}

				if (slot == Asset::MaterialTextureSlot::BaseColor &&
					!exactOrStemMatch &&
					ContainsAnyTextureKeyword(lowerSearchText, Asset::TextureMatching::NonBaseColorKeywords()))
				{
					return;
				}
				if (slot != Asset::MaterialTextureSlot::BaseColor &&
					!exactOrStemMatch &&
					TextureSlotKeywordScore(slot, lowerSearchText) == 0)
				{
					return;
				}

				const std::string materialName = ToLower(material.Name);
				if (!materialName.empty() && lowerName.find(materialName) != std::string::npos)
				{
					score += 18;
					AppendTextureRemapReason(reason, "material name match");
				}
				bool tokenMatched = false;
				for (const std::string& token : BuildTextureRemapMatchTokens(material, missingPath, sourcePath))
				{
					if (lowerSearchText.find(token) != std::string::npos)
					{
						score += 20;
						tokenMatched = true;
					}
				}
				if (tokenMatched)
				{
					AppendTextureRemapReason(reason, "token match");
				}
				const std::string lowerParentPath = ToLower(entry.Path.parent_path().string());
				if (lowerName.find("texture") != std::string::npos || lowerParentPath.find("texture") != std::string::npos)
				{
					score += 4;
				}
				if (!missingPath.parent_path().filename().empty() &&
					ToLower(entry.Path.parent_path().filename().string()) == ToLower(missingPath.parent_path().filename().string()))
				{
					score += 12;
					AppendTextureRemapReason(reason, "same folder name");
				}
				if (!sourcePath.empty() && IsPathInsideDirectory(entry.Path, sourcePath.parent_path()))
				{
					score += 16;
					AppendTextureRemapReason(reason, "near source model");
				}
				if (score >= 24)
				{
					if (reason.empty())
					{
						reason = "slot keyword match";
					}
					candidates.push_back(TextureRemapCandidate{
						.Path = entry.Path,
						.Score = score,
						.Reason = std::move(reason)
					});
				}
			}

			for (const Asset::AssetFileEntry& child : entry.Children)
			{
				CollectTextureRemapCandidates(child, missingPath, material, slot, sourcePath, candidates);
			}
		}

		[[nodiscard]] std::vector<TextureRemapCandidate> FindTextureRemapCandidates(
			const Asset::AssetFileSnapshot& snapshot,
			const std::filesystem::path& missingPath,
			const Asset::StaticMeshMaterial& material,
			Asset::MaterialTextureSlot slot,
			const std::filesystem::path& sourcePath,
			size_t maxCandidates)
		{
			std::vector<TextureRemapCandidate> candidates;
			for (const Asset::AssetFileEntry& entry : snapshot.Children)
			{
				CollectTextureRemapCandidates(entry, missingPath, material, slot, sourcePath, candidates);
			}
			std::sort(candidates.begin(), candidates.end(), [](const TextureRemapCandidate& lhs, const TextureRemapCandidate& rhs)
				{
					if (lhs.Score != rhs.Score)
					{
						return lhs.Score > rhs.Score;
					}
					return ToLower(lhs.Path.string()) < ToLower(rhs.Path.string());
				});
			if (candidates.size() > maxCandidates)
			{
				candidates.resize(maxCandidates);
			}
			return candidates;
		}

		[[nodiscard]] bool TextureRemapCandidateMatchesFilter(const TextureRemapCandidate& candidate, std::string_view filter)
		{
			return filter.empty() ||
				ContainsCaseInsensitive(candidate.Path.filename().string(), filter) ||
				ContainsCaseInsensitive(candidate.Path.string(), filter) ||
				ContainsCaseInsensitive(candidate.Reason, filter);
		}

		[[nodiscard]] std::vector<TextureRemapCandidate> FilterTextureRemapCandidates(
			std::vector<TextureRemapCandidate> candidates,
			std::string_view filter,
			size_t maxCandidates)
		{
			if (!filter.empty())
			{
				std::erase_if(candidates, [filter](const TextureRemapCandidate& candidate)
					{
						return !TextureRemapCandidateMatchesFilter(candidate, filter);
					});
			}
			if (candidates.size() > maxCandidates)
			{
				candidates.resize(maxCandidates);
			}
			return candidates;
		}

		[[nodiscard]] bool TryGetMissingMaterialTexturePath(
			const Asset::StaticMeshMaterial& material,
			Asset::MaterialTextureSlot slot,
			const std::shared_ptr<const Asset::AssetFileSnapshot>& snapshot,
			std::filesystem::path& missingPath)
		{
			missingPath = Asset::GetMaterialTexturePath(material, slot);
			if (missingPath.empty() || Asset::GetMaterialTextureBinding(material, slot).Embedded.IsValid())
			{
				return false;
			}

			std::error_code texturePathError;
			const std::filesystem::path resolvedPath = ResolveEditorAssetPath(missingPath, snapshot);
			return !std::filesystem::exists(resolvedPath, texturePathError) && !texturePathError;
		}

		[[nodiscard]] std::vector<MaterialTextureAssignment> BuildAutoTextureRemapAssignments(
			const std::shared_ptr<const Asset::AssetFileSnapshot>& snapshot,
			const Asset::StaticMeshMaterial& material,
			const std::filesystem::path& sourcePath,
			std::string_view filter)
		{
			std::vector<MaterialTextureAssignment> assignments;
			if (!snapshot || !snapshot->RootExists)
			{
				return assignments;
			}

			for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
			{
				const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
				if (!ShouldShowMaterialSlot(material.ShadingModel, slot))
				{
					continue;
				}

				std::filesystem::path missingPath;
				if (!TryGetMissingMaterialTexturePath(material, slot, snapshot, missingPath))
				{
					continue;
				}

				std::vector<TextureRemapCandidate> candidates =
					FindTextureRemapCandidates(*snapshot, missingPath, material, slot, sourcePath, 16);
				candidates = FilterTextureRemapCandidates(std::move(candidates), filter, 1);
				if (!candidates.empty())
				{
					assignments.push_back(MaterialTextureAssignment{
						.Slot = slot,
						.Path = candidates.front().Path
					});
				}
			}
			return assignments;
		}

		[[nodiscard]] std::vector<MaterialTextureBatchAssignment> BuildMeshAutoTextureRemapAssignments(
			const std::shared_ptr<const Asset::AssetFileSnapshot>& snapshot,
			const Asset::StaticMeshAsset& mesh,
			std::string_view filter)
		{
			std::vector<MaterialTextureBatchAssignment> batchAssignments;
			if (!snapshot || !snapshot->RootExists)
			{
				return batchAssignments;
			}

			for (size_t materialIndex = 0; materialIndex < mesh.Materials.size(); ++materialIndex)
			{
				std::vector<MaterialTextureAssignment> assignments =
					BuildAutoTextureRemapAssignments(snapshot, mesh.Materials[materialIndex], mesh.SourcePath, filter);
				if (!assignments.empty())
				{
					batchAssignments.push_back(MaterialTextureBatchAssignment{
						.MaterialIndex = materialIndex,
						.Assignments = std::move(assignments)
					});
				}
			}
			return batchAssignments;
		}

		[[nodiscard]] std::vector<TextureRemapPreviewRow> BuildMeshTextureRemapPreviewRows(
			const std::shared_ptr<const Asset::AssetFileSnapshot>& snapshot,
			const Asset::StaticMeshAsset& mesh,
			std::string_view filter)
		{
			std::vector<TextureRemapPreviewRow> rows;
			if (!snapshot || !snapshot->RootExists)
			{
				return rows;
			}

			for (size_t materialIndex = 0; materialIndex < mesh.Materials.size(); ++materialIndex)
			{
				const Asset::StaticMeshMaterial& material = mesh.Materials[materialIndex];
				for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
				{
					const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
					if (!ShouldShowMaterialSlot(material.ShadingModel, slot))
					{
						continue;
					}

					std::filesystem::path missingPath;
					if (!TryGetMissingMaterialTexturePath(material, slot, snapshot, missingPath))
					{
						continue;
					}

					std::vector<TextureRemapCandidate> candidates =
						FilterTextureRemapCandidates(
							FindTextureRemapCandidates(*snapshot, missingPath, material, slot, mesh.SourcePath, 16),
							filter,
							4);
					if (candidates.empty())
					{
						continue;
					}

					TextureRemapPreviewRow row;
					row.MaterialIndex = materialIndex;
					row.MaterialName = material.Name;
					row.Slot = slot;
					row.MissingPath = missingPath;
					row.Candidate = candidates.front();
					row.Candidates = candidates;
					row.Confidence = EvaluateTextureRemapConfidence(candidates);
					if (candidates.size() > 1)
					{
						row.AlternativePath = candidates[1].Path;
						row.AlternativeScore = candidates[1].Score;
					}
					rows.push_back(std::move(row));
				}
			}
			return rows;
		}

		void ApplyTextureRemapCandidateOverrides(
			std::vector<TextureRemapPreviewRow>& previewRows,
			const std::unordered_map<std::string, std::filesystem::path>& overrides)
		{
			for (TextureRemapPreviewRow& row : previewRows)
			{
				const std::string key = BuildTextureRemapOverrideKey(row.MaterialIndex, row.Slot, row.MissingPath);
				const auto overrideIt = overrides.find(key);
				if (overrideIt == overrides.end())
				{
					continue;
				}

				const auto candidateIt = std::ranges::find_if(row.Candidates, [&overrideIt](const TextureRemapCandidate& candidate)
					{
						return SamePath(candidate.Path, overrideIt->second);
					});
				if (candidateIt == row.Candidates.end())
				{
					continue;
				}

				row.Candidate = *candidateIt;
				row.CandidateOverridden = true;
				row.Confidence = TextureRemapConfidence::Medium;
				if (!row.Candidates.empty())
				{
					const TextureRemapCandidate& automaticCandidate = row.Candidates.front();
					if (!SamePath(automaticCandidate.Path, row.Candidate.Path))
					{
						row.AlternativePath = automaticCandidate.Path;
						row.AlternativeScore = automaticCandidate.Score;
					}
				}
			}
		}

		[[nodiscard]] std::vector<MaterialTextureBatchAssignment> BuildBatchAssignmentsFromTextureRemapPreview(
			const std::vector<TextureRemapPreviewRow>& previewRows)
		{
			std::vector<MaterialTextureBatchAssignment> batchAssignments;
			for (const TextureRemapPreviewRow& row : previewRows)
			{
				auto batchIt = std::ranges::find_if(batchAssignments, [&row](const MaterialTextureBatchAssignment& batch)
					{
						return batch.MaterialIndex == row.MaterialIndex;
					});
				if (batchIt == batchAssignments.end())
				{
					batchAssignments.push_back(MaterialTextureBatchAssignment{
						.MaterialIndex = row.MaterialIndex,
						.Assignments = {}
					});
					batchIt = std::prev(batchAssignments.end());
				}
				batchIt->Assignments.push_back(MaterialTextureAssignment{
					.Slot = row.Slot,
					.Path = row.Candidate.Path
				});
			}
			return batchAssignments;
		}

		[[nodiscard]] size_t CountRiskyTextureRemapRows(const std::vector<TextureRemapPreviewRow>& previewRows) noexcept
		{
			return static_cast<size_t>(std::ranges::count_if(previewRows, [](const TextureRemapPreviewRow& row)
				{
					return row.Confidence == TextureRemapConfidence::Low ||
						row.Confidence == TextureRemapConfidence::Ambiguous;
				}));
		}

		[[nodiscard]] size_t CountMaterialTextureAssignments(const std::vector<MaterialTextureBatchAssignment>& batchAssignments) noexcept
		{
			size_t count = 0;
			for (const MaterialTextureBatchAssignment& batch : batchAssignments)
			{
				count += batch.Assignments.size();
			}
			return count;
		}

		[[nodiscard]] bool IsFavoriteProjectPath(const std::vector<std::filesystem::path>& favorites, const std::filesystem::path& path)
		{
			return std::ranges::any_of(favorites, [&path](const std::filesystem::path& favoritePath)
				{
					return SamePath(favoritePath, path);
				});
		}

		[[nodiscard]] bool ProjectEntryMatchesTextFilter(const Asset::AssetFileEntry& entry, std::string_view filter)
		{
			if (filter.empty())
			{
				return true;
			}
			const std::string searchText = entry.Name + " " + entry.Path.string() + " " + AssetKindTag(entry.Kind) + " " + ExtensionTag(entry.Path);
			return ContainsCaseInsensitive(searchText, filter);
		}

		[[nodiscard]] bool ProjectEntryMatchesQuickFilter(
			const Asset::AssetFileEntry& entry,
			ProjectQuickFilter filter,
			const std::vector<std::filesystem::path>& favorites)
		{
			const std::string extension = ToLower(entry.Path.extension().string());
			switch (filter)
			{
			case ProjectQuickFilter::All:
				return true;
			case ProjectQuickFilter::Favorites:
				return IsFavoriteProjectPath(favorites, entry.Path);
			case ProjectQuickFilter::Folders:
				return entry.Kind == Asset::AssetFileKind::Directory;
			case ProjectQuickFilter::Models:
				return entry.Kind == Asset::AssetFileKind::Model;
			case ProjectQuickFilter::Images:
				return entry.Kind == Asset::AssetFileKind::Image;
			case ProjectQuickFilter::Scenes:
				return extension == ".scene";
			case ProjectQuickFilter::Materials:
				return extension == ".material" || extension == ".skybox";
			case ProjectQuickFilter::Prefabs:
				return extension == ".prefab";
			case ProjectQuickFilter::Source:
				return entry.Kind == Asset::AssetFileKind::Source;
			case ProjectQuickFilter::Text:
				return entry.Kind == Asset::AssetFileKind::Text &&
					extension != ".scene" &&
					extension != ".material" &&
					extension != ".skybox" &&
					extension != ".prefab";
			default:
				return true;
			}
		}

		[[nodiscard]] bool ProjectEntryMatchesFilter(
			const Asset::AssetFileEntry& entry,
			std::string_view filter,
			ProjectQuickFilter quickFilter,
			const std::vector<std::filesystem::path>& favorites)
		{
			if (ProjectEntryMatchesTextFilter(entry, filter) &&
				ProjectEntryMatchesQuickFilter(entry, quickFilter, favorites))
			{
				return true;
			}
			return std::ranges::any_of(entry.Children, [filter, quickFilter, &favorites](const Asset::AssetFileEntry& child)
				{
					return ProjectEntryMatchesFilter(child, filter, quickFilter, favorites);
				});
		}

		void AddProjectEntryCommands(
			const Asset::AssetFileEntry& entry,
			const Asset::AssetFileSnapshot& snapshot,
			EditorContext& context,
			std::vector<CommandPaletteItem>& items,
			std::string_view filter,
			std::filesystem::path& selectedAssetPath)
		{
			const std::string relativePath = RelativeDisplayPath(entry.Path, snapshot.RootPath);
			const std::string searchText = entry.Name + " " + relativePath + " asset project";
			const bool matches = ContainsCaseInsensitive(searchText, filter);

			if (matches)
			{
				if (context.OnAssetOpen)
				{
					items.push_back(CommandPaletteItem{
						.Label = "Open Asset: " + entry.Name,
						.Detail = relativePath,
						.Scope = CommandPaletteScope::Assets,
						.Enabled = true,
						.Execute = [&context, &selectedAssetPath, path = entry.Path]()
						{
							selectedAssetPath = path;
							context.OnAssetOpen(path);
						}
					});
				}

				if (context.OnAssetReveal)
				{
					items.push_back(CommandPaletteItem{
						.Label = "Reveal Asset: " + entry.Name,
						.Detail = relativePath,
						.Scope = CommandPaletteScope::Assets,
						.Enabled = true,
						.Execute = [&context, &selectedAssetPath, path = entry.Path]()
						{
							selectedAssetPath = path;
							context.OnAssetReveal(path);
						}
					});
				}

				if (entry.Kind == Asset::AssetFileKind::Model && context.OnModelDrop)
				{
					items.push_back(CommandPaletteItem{
						.Label = "Load Model: " + entry.Name,
						.Detail = relativePath,
						.Scope = CommandPaletteScope::Assets,
						.Enabled = context.CanEditProjectScene,
						.Execute = [&context, &selectedAssetPath, path = entry.Path]()
						{
							selectedAssetPath = path;
							context.OnModelDrop(path, AssetDropTarget::Game);
						}
					});
				}

				if (Asset::IsSkyboxAssetPath(entry.Path) && context.OnAssetOpen)
				{
					items.push_back(CommandPaletteItem{
						.Label = "Apply Skybox: " + entry.Name,
						.Detail = relativePath,
						.Scope = CommandPaletteScope::Assets,
						.Enabled = context.CanEditProjectScene,
						.Execute = [&context, &selectedAssetPath, path = entry.Path]()
						{
							selectedAssetPath = path;
							context.OnAssetOpen(path);
						}
					});
				}

				if (entry.Kind == Asset::AssetFileKind::Model && context.OnAssetReimportRequested)
				{
					items.push_back(CommandPaletteItem{
						.Label = "Reimport Asset: " + entry.Name,
						.Detail = relativePath,
						.Scope = CommandPaletteScope::Assets,
						.Enabled = context.CanEditProjectScene,
						.Execute = [&context, &selectedAssetPath, path = entry.Path]()
						{
							selectedAssetPath = path;
							context.OnAssetReimportRequested(path);
						}
					});
				}
			}

			for (const Asset::AssetFileEntry& child : entry.Children)
			{
				AddProjectEntryCommands(child, snapshot, context, items, filter, selectedAssetPath);
			}
		}

		void DrawMaterialTextureSlotRow(
			EditorContext& context,
			EntityId entityId,
			size_t materialIndex,
			const Asset::StaticMeshMaterial& material,
			Asset::MaterialTextureSlot slot,
			bool focused,
			const std::filesystem::path& sourcePath,
			std::string_view remapFilter)
		{
			const std::filesystem::path path = Asset::GetMaterialTexturePath(material, slot);
			const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
			const std::string id = std::format("{}##mat{}_slot{}", Asset::MaterialTextureSlotName(slot), materialIndex, Asset::MaterialTextureSlotIndex(slot));
			const std::filesystem::path resolvedPath = ResolveEditorAssetPath(path, context.ProjectSnapshot);
			std::error_code texturePathError;
			const bool hasTexturePath = !path.empty();
			const bool texturePathExists = hasTexturePath && std::filesystem::exists(resolvedPath, texturePathError);
			const bool missingTexturePath = hasTexturePath && !texturePathError && !texturePathExists;

			ImGui::PushID(id.c_str());
			if (focused)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.30f, 1.0f));
			}
			ImGui::Text("%s", std::string(Asset::MaterialTextureSlotName(slot)).c_str());
			if (focused && ImGui::IsItemVisible())
			{
				ImGui::SetScrollHereY(0.5f);
			}
			ImGui::SameLine(130.0f);
			ImGui::TextWrapped("%s", path.empty() ? (binding.Embedded.IsValid() ? "<embedded>" : "<fallback>") : path.filename().string().c_str());
			if (focused)
			{
				ImGui::PopStyleColor();
			}
			if (!path.empty() && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", path.string().c_str());
			}
			if (texturePathError)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "[Error]");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", texturePathError.message().c_str());
				}
			}
			else if (missingTexturePath)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "[Missing]");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Resolved as: %s", resolvedPath.string().c_str());
				}
			}

			ImGui::SameLine();
			if (ImGui::SmallButton("Browse") && context.OnMaterialTextureBrowseRequested)
			{
				context.OnMaterialTextureBrowseRequested(entityId, materialIndex, slot);
			}
			ImGui::SameLine();
			const bool hasSource = !path.empty() || binding.Embedded.IsValid();
			ImGui::BeginDisabled(!hasSource);
			if (ImGui::SmallButton("Clear") && context.OnMaterialTextureCleared)
			{
				context.OnMaterialTextureCleared(entityId, materialIndex, slot);
			}
			ImGui::EndDisabled();

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathPayload))
				{
					if (payload->Data && payload->DataSize > 0 && context.OnMaterialTextureAssigned)
					{
						const char* pathText = static_cast<const char*>(payload->Data);
						const std::filesystem::path droppedPath(pathText);
						if (IsImageAssetPath(droppedPath))
						{
							context.OnMaterialTextureAssigned(entityId, materialIndex, slot, droppedPath);
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (missingTexturePath)
			{
				ImGui::Indent(18.0f);
				ImGui::TextColored(
					ImVec4(1.0f, 0.72f, 0.22f, 1.0f),
					"Missing texture dependency: %s",
					path.string().c_str());
				if (context.ProjectSnapshot && context.ProjectSnapshot->RootExists && context.OnMaterialTextureAssigned)
				{
					const std::vector<TextureRemapCandidate> candidates =
						FilterTextureRemapCandidates(
							FindTextureRemapCandidates(*context.ProjectSnapshot, path, material, slot, sourcePath, 16),
							remapFilter,
							4);
					if (candidates.empty())
					{
						ImGui::TextDisabled(remapFilter.empty()
							? "No image candidates found in the current Project snapshot."
							: "No image candidates match the current remap filter.");
						if (context.OnProjectRefresh)
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Refresh Project"))
							{
								context.OnProjectRefresh();
							}
						}
					}
					else if (ImGui::TreeNodeEx("Suggested Remaps", ImGuiTreeNodeFlags_DefaultOpen))
					{
						const TextureRemapConfidence confidence = EvaluateTextureRemapConfidence(candidates);
						ImGui::TextColored(
							TextureRemapConfidenceColor(confidence),
							"Best match confidence: %s",
							TextureRemapConfidenceName(confidence));
						if (confidence == TextureRemapConfidence::Ambiguous && candidates.size() > 1)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("top scores are close");
						}
						for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
						{
							const TextureRemapCandidate& candidate = candidates[candidateIndex];
							ImGui::PushID(static_cast<int>(candidateIndex));
							if (ImGui::SmallButton("Assign"))
							{
								context.OnMaterialTextureAssigned(entityId, materialIndex, slot, candidate.Path);
							}
							ImGui::SameLine();
							ImGui::TextWrapped(
								"%s  (%s, score %d)",
								RelativeDisplayPath(candidate.Path, context.ProjectSnapshot->RootPath).c_str(),
								candidate.Reason.c_str(),
								candidate.Score);
							if (ImGui::IsItemHovered())
							{
								ImGui::SetTooltip("%s", candidate.Path.string().c_str());
							}
							ImGui::PopID();
						}
						ImGui::TreePop();
					}
				}
				else
				{
					ImGui::TextDisabled("Project snapshot or material assignment callback is unavailable.");
				}
				ImGui::Unindent(18.0f);
			}
			ImGui::PopID();
		}
	}

	void EditorLayer::Draw(EditorContext& context)
	{
		EnsureProjectStateLoaded(context);
		TrackCurrentSceneInRecentScenes(context);
		DrawDockSpace();
		DrawToolbar(context);
		HandleHierarchyShortcuts(context);
		DrawHierarchy(context);
		DrawSceneView(context);
		DrawGameView(context);
		DrawInspector(context);
		DrawProject(context);
		DrawBenchmark(context);
		DrawProfiler(context);
		DrawConsole(context);
		DrawStatusBar(context);

		if (context.ShowDemoWindow)
		{
			ImGui::ShowDemoWindow(&context.ShowDemoWindow);
		}
		DrawCommandPalette(context);
		DrawContentDrawer(context);
		DrawShortcutReference(context);
	}

	void EditorLayer::DrawDockSpace()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);
		ImGui::SetNextWindowViewport(viewport->ID);

		ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNavFocus |
			ImGuiWindowFlags_NoBackground;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Editor DockSpace", nullptr, windowFlags);
		ImGui::PopStyleVar(3);

		const ImGuiID dockspaceId = ImGui::GetID("EngineEditorDockSpace");
		if (!m_DefaultLayoutBuilt)
		{
			BuildDefaultLayout(dockspaceId, viewport->Size.x, viewport->Size.y);
			m_DefaultLayoutBuilt = true;
		}

		ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
		ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
		ImGui::End();
	}

	void EditorLayer::BuildDefaultLayout(unsigned int dockspaceId, float viewportWidth, float viewportHeight)
	{
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, ImVec2(viewportWidth, viewportHeight));

		ImGuiID mainNode = dockspaceId;
		const ImGuiID hierarchyNode = ImGui::DockBuilderSplitNode(mainNode, ImGuiDir_Left, 0.18f, nullptr, &mainNode);
		const ImGuiID inspectorNode = ImGui::DockBuilderSplitNode(mainNode, ImGuiDir_Right, 0.24f, nullptr, &mainNode);
		const ImGuiID bottomNode = ImGui::DockBuilderSplitNode(mainNode, ImGuiDir_Down, 0.27f, nullptr, &mainNode);
		const ImGuiID gameNode = ImGui::DockBuilderSplitNode(mainNode, ImGuiDir_Right, 0.42f, nullptr, &mainNode);

		ImGuiID projectNode = bottomNode;
		const ImGuiID toolsNode = ImGui::DockBuilderSplitNode(projectNode, ImGuiDir_Right, 0.5f, nullptr, &projectNode);

		ImGui::DockBuilderDockWindow("Hierarchy", hierarchyNode);
		ImGui::DockBuilderDockWindow("Inspector", inspectorNode);
		ImGui::DockBuilderDockWindow("Scene", mainNode);
		ImGui::DockBuilderDockWindow("Game", gameNode);
		ImGui::DockBuilderDockWindow("Project", projectNode);
		ImGui::DockBuilderDockWindow("Benchmark", toolsNode);
		ImGui::DockBuilderDockWindow("Profiler", toolsNode);
		ImGui::DockBuilderDockWindow("Console", toolsNode);
		ImGui::DockBuilderFinish(dockspaceId);
	}

	void EditorLayer::StoreViewportState(
		ViewportPanelState& target,
		float screenLeft,
		float screenTop,
		float width,
		float height,
		bool hovered,
		bool focused) const
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		target.Left = screenLeft - viewport->Pos.x;
		target.Top = screenTop - viewport->Pos.y;
		target.Width = width;
		target.Height = height;
		target.IsVisible = width >= 1.0f && height >= 1.0f;
		target.IsHovered = hovered;
		target.IsFocused = focused;
	}

	void EditorLayer::DrawToolbar(EditorContext& context)
	{
		if (!ImGui::BeginMainMenuBar())
		{
			return;
		}

		ImGui::TextUnformatted("EnginePlatformer");
		if (context.IsSceneDirty)
		{
			ImGui::SameLine();
			ImGui::TextUnformatted("*");
		}
		ImGui::Separator();

		if (ImGui::BeginMenu("File"))
		{
			if (!context.CanEditProjectScene)
			{
				ImGui::BeginDisabled();
			}
			if (ImGui::MenuItem("Save Scene", "Ctrl+S") && context.OnSaveScene)
			{
				context.OnSaveScene();
			}
			if (ImGui::MenuItem("Save Scene As...") && context.OnSaveSceneAs)
			{
				context.OnSaveSceneAs();
			}
			if (ImGui::MenuItem("Open Scene...") && context.OnOpenSceneDialog)
			{
				context.OnOpenSceneDialog();
			}
			DrawRecentScenesMenu(context);
			ImGui::Separator();
			if (ImGui::MenuItem("Save Selected As Prefab") && context.OnSaveSelectedPrefab)
			{
				context.OnSaveSelectedPrefab();
			}
			if (ImGui::MenuItem("Export Project Package") && (context.OnExportProjectProfile || context.OnExportProject))
			{
				if (context.OnExportProjectProfile)
				{
					EnsureExportProfileDefaults(context);
					static_cast<void>(context.OnExportProjectProfile(BuildExportProfileSettings(context)));
				}
				else
				{
					context.OnExportProject();
				}
			}
			if (!context.CanEditProjectScene)
			{
				ImGui::EndDisabled();
			}
			if (ImGui::MenuItem("Reveal Project") && context.OnRevealProject)
			{
				context.OnRevealProject();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Exit") && context.OnExit)
			{
				context.OnExit();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Edit"))
		{
			if (!context.CanUndo)
			{
				ImGui::BeginDisabled();
			}
			const std::string undoLabel = context.UndoLabel.empty() ? "Undo" : std::format("Undo {}", context.UndoLabel);
			if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z") && context.OnUndo)
			{
				context.OnUndo();
			}
			if (!context.CanUndo)
			{
				ImGui::EndDisabled();
			}
			if (!context.CanRedo)
			{
				ImGui::BeginDisabled();
			}
			const std::string redoLabel = context.RedoLabel.empty() ? "Redo" : std::format("Redo {}", context.RedoLabel);
			if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y") && context.OnRedo)
			{
				context.OnRedo();
			}
			if (!context.CanRedo)
			{
				ImGui::EndDisabled();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Window"))
		{
			if (ImGui::MenuItem("Content Drawer", "Ctrl+Space"))
			{
				OpenContentDrawer();
			}
			if (ImGui::MenuItem("Save Editor Layout"))
			{
				SaveCurrentEditorLayout(context);
			}
			if (ImGui::MenuItem("Restore Saved Layout", nullptr, false, !m_ProjectEditorLayoutIni.empty()))
			{
				RestoreSavedEditorLayout();
			}
			if (ImGui::MenuItem("Reset Editor Layout"))
			{
				ResetEditorLayoutToDefault();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Help"))
		{
			if (ImGui::MenuItem("Keyboard Shortcuts", "F1"))
			{
				OpenShortcutReference();
			}
			if (ImGui::MenuItem("Command Palette", "Ctrl+P / Ctrl+K"))
			{
				OpenCommandPalette();
			}
			ImGui::EndMenu();
		}

		if (ImGui::Button("Search") || (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
		{
			OpenCommandPalette();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Open Command Palette (Ctrl+P / Ctrl+K)");
		}
		ImGui::SameLine();
		if (ImGui::Button("Content"))
		{
			OpenContentDrawer();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Open Content Drawer (Ctrl+Space)");
		}
		ImGui::Separator();

		if (!context.CanEditProjectScene)
		{
			ImGui::BeginDisabled();
		}
		if (ImGui::Button("Save") && context.OnSaveScene)
		{
			context.OnSaveScene();
		}
		if (!context.CanEditProjectScene)
		{
			ImGui::EndDisabled();
		}
		if (!context.CurrentScenePath.empty())
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(context.CurrentScenePath.filename().string().c_str());
		}
		ImGui::Separator();

		const bool isPaused = context.PlayState == EditorPlayState::Paused;
		const bool isPlaying = context.PlayState == EditorPlayState::Play || context.PlayState == EditorPlayState::EnteringPlay;
		const bool isPlaySession = isPlaying || isPaused;
		if (!context.CanControlPlayMode)
		{
			ImGui::BeginDisabled();
		}
		if (ImGui::Button(isPlaySession ? "Stop" : "Play") && context.OnPlayModeChanged)
		{
			context.OnPlayModeChanged(!isPlaySession);
		}
		if (!context.CanControlPlayMode)
		{
			ImGui::EndDisabled();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(isPlaySession ? "Restore the in-memory edit scene snapshot" : "Clone the edit scene into a runtime scene and enter Play mode");
		}
		ImGui::SameLine();
		const bool canPause = context.CanControlPlayMode && (context.PlayState == EditorPlayState::Play || isPaused) && static_cast<bool>(context.OnPlayPausedChanged);
		ImGui::BeginDisabled(!canPause);
		if (ImGui::Button(isPaused ? "Resume" : "Pause") && context.OnPlayPausedChanged)
		{
			context.OnPlayPausedChanged(!isPaused);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip(isPaused ? "Resume Play mode simulation" : "Pause Play mode simulation while keeping rendering and editor UI responsive");
		}
		ImGui::SameLine();
		const bool canStep = context.CanControlPlayMode && isPaused && static_cast<bool>(context.OnPlayStep);
		ImGui::BeginDisabled(!canStep);
		if (ImGui::Button("Step") && context.OnPlayStep)
		{
			context.OnPlayStep();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Advance the paused Play runtime clone by one frame");
		}
		ImGui::SameLine();
		const bool canResetPlayRuntime = context.CanControlPlayMode && context.ActiveSceneIsRuntimeClone && static_cast<bool>(context.OnResetPlayRuntimeScene);
		ImGui::BeginDisabled(!canResetPlayRuntime);
		if (ImGui::Button("Reset") && context.OnResetPlayRuntimeScene)
		{
			context.OnResetPlayRuntimeScene();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Rebuild the Play runtime clone from the locked edit scene snapshot");
		}
		ImGui::Separator();

		int apiIndex = context.CurrentApi == GraphicsAPI::DirectX12 ? 0 : 1;
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::Combo("##GraphicsApi", &apiIndex, "DirectX12\0Vulkan\0"))
		{
			const GraphicsAPI requestedApi = apiIndex == 0 ? GraphicsAPI::DirectX12 : GraphicsAPI::Vulkan;
			if (context.OnGraphicsApiChanged)
			{
				context.OnGraphicsApiChanged(requestedApi);
			}
		}

		int renderModeIndex = std::to_underlying(context.CurrentRenderMode);
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::Combo("##RenderMode", &renderModeIndex, "Forward\0Deferred\0Forward+\0"))
		{
			if (context.OnRenderModeChanged)
			{
				context.OnRenderModeChanged(static_cast<RenderMode>(renderModeIndex));
			}
		}

		int sampleModeIndex = std::to_underlying(context.SampleMode);
		ImGui::SetNextItemWidth(150.0f);
		if (ImGui::Combo("##SampleMode", &sampleModeIndex, "Project Scene\0Spider Sample\0ECS Benchmark\0"))
		{
			context.SampleMode = static_cast<Samples::Benchmark::SampleMode>(sampleModeIndex);
		}

		if (ImGui::Button("Frame Selected") && context.OnFrameSelected)
		{
			context.OnFrameSelected();
		}

		ImGui::SameLine();
		int gizmoModeIndex = std::to_underlying(m_TransformGizmoMode);
		ImGui::SetNextItemWidth(105.0f);
		if (ImGui::Combo("##TransformGizmoMode", &gizmoModeIndex, "Translate\0Rotate\0Scale\0"))
		{
			gizmoModeIndex = std::clamp(gizmoModeIndex, 0, 2);
			m_TransformGizmoMode = static_cast<TransformGizmoMode>(gizmoModeIndex);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Scene transform gizmo: %s", TransformGizmoModeName(m_TransformGizmoMode));
		}
		ImGui::SameLine();
		int gizmoSpaceIndex = std::to_underlying(m_TransformGizmoSpace);
		ImGui::SetNextItemWidth(84.0f);
		if (ImGui::Combo("##TransformGizmoSpace", &gizmoSpaceIndex, "World\0Local\0"))
		{
			gizmoSpaceIndex = std::clamp(gizmoSpaceIndex, 0, 1);
			m_TransformGizmoSpace = static_cast<TransformGizmoSpace>(gizmoSpaceIndex);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Transform gizmo space: %s", TransformGizmoSpaceName(m_TransformGizmoSpace));
		}
		ImGui::SameLine();
		int gizmoPivotIndex = std::to_underlying(m_TransformGizmoPivot);
		ImGui::SetNextItemWidth(84.0f);
		if (ImGui::Combo("##TransformGizmoPivot", &gizmoPivotIndex, "Pivot\0Center\0"))
		{
			gizmoPivotIndex = std::clamp(gizmoPivotIndex, 0, 1);
			m_TransformGizmoPivot = static_cast<TransformGizmoPivot>(gizmoPivotIndex);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Transform gizmo pivot: %s", TransformGizmoPivotName(m_TransformGizmoPivot));
		}

		if (ImGui::Button("Align Game Camera") && context.OnAlignGameCameraToScene)
		{
			context.OnAlignGameCameraToScene();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Copy Scene camera view to the Game Camera");
		}
		ImGui::SameLine();
		if (ImGui::Button("Align Scene to Game") && context.OnAlignSceneCameraToGame)
		{
			context.OnAlignSceneCameraToGame();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Move Scene camera to the Game Camera view");
		}

		ImGui::Separator();
		ImGui::Checkbox("Gizmos", &m_ShowSceneGizmos);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show Scene View camera gizmos");
		}
		ImGui::SameLine();
		bool viewFrustumCulling = context.ViewFrustumCullingEnabled;
		if (ImGui::Checkbox("Frustum Culling", &viewFrustumCulling) && context.OnViewFrustumCullingChanged)
		{
			context.OnViewFrustumCullingChanged(viewFrustumCulling);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Cull Scene/Game View meshes outside each camera frustum");
		}
		ImGui::SameLine();
		int debugViewIndex = std::to_underlying(context.DebugView);
		ImGui::SetNextItemWidth(130.0f);
		if (ImGui::Combo("##MaterialDebugView", &debugViewIndex, "Lit\0BaseColor\0Normal\0Metallic\0Roughness\0AO\0Emissive\0LightingOnly\0VertexColor\0Shadow\0TileLights\0"))
		{
			debugViewIndex = std::clamp(debugViewIndex, 0, static_cast<int>(std::to_underlying(MaterialDebugView::DeferredTileLights)));
			if (context.OnMaterialDebugViewChanged)
			{
				context.OnMaterialDebugViewChanged(static_cast<MaterialDebugView>(debugViewIndex));
			}
		}
		if (ImGui::IsItemHovered())
		{
			if (context.DebugView == MaterialDebugView::Shadow || context.DebugView == MaterialDebugView::DeferredTileLights)
			{
				ImGui::SetTooltip("Material Debug View: %s (Deferred only)", MaterialDebugViewName(context.DebugView));
			}
			else
			{
				ImGui::SetTooltip("Material Debug View: %s", MaterialDebugViewName(context.DebugView));
			}
		}
		ImGui::SameLine();
		if (!context.CanEditProjectScene)
		{
			ImGui::BeginDisabled();
		}
		bool simulatePhysics = context.PhysicsSimulationEnabled;
		if (ImGui::Checkbox("Simulate Physics", &simulatePhysics) && context.OnPhysicsSimulationChanged)
		{
			context.OnPhysicsSimulationChanged(simulatePhysics);
		}
		if (!context.CanEditProjectScene)
		{
			ImGui::EndDisabled();
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Run Project Scene rigid bodies with fixed timestep PhysX simulation");
		}
		ImGui::Separator();
		ImGui::Checkbox("Demo", &context.ShowDemoWindow);
		ImGui::EndMainMenuBar();
	}

	void EditorLayer::DrawHierarchy(EditorContext& context)
	{
		SetInitialWindowRect("Hierarchy", 8.0f, 32.0f, 260.0f, 560.0f);
		ImGui::Begin("Hierarchy");

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##HierarchyFilter", "Search entities...", m_HierarchyFilter.data(), m_HierarchyFilter.size());
		int hierarchyQuickFilterIndex = static_cast<int>(std::to_underlying(m_HierarchyQuickFilter));
		ImGui::SetNextItemWidth(128.0f);
		if (ImGui::Combo("##HierarchyQuickFilter", &hierarchyQuickFilterIndex, "All\0Mesh\0Camera\0Light\0Physics\0Script\0Hidden\0Locked\0Nested\0"))
		{
			const int hierarchyQuickFilterMax = static_cast<int>(std::to_underlying(HierarchyQuickFilter::Nested));
			hierarchyQuickFilterIndex = std::clamp(hierarchyQuickFilterIndex, 0, hierarchyQuickFilterMax);
			m_HierarchyQuickFilter = static_cast<HierarchyQuickFilter>(hierarchyQuickFilterIndex);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Hierarchy quick filter: %s", HierarchyQuickFilterName(m_HierarchyQuickFilter));
		}
		ImGui::SameLine();
		const bool canClearHierarchyFilter = !TextFilter(m_HierarchyFilter).empty() || m_HierarchyQuickFilter != HierarchyQuickFilter::All;
		ImGui::BeginDisabled(!canClearHierarchyFilter);
		if (ImGui::SmallButton("Clear"))
		{
			m_HierarchyFilter.fill('\0');
			m_HierarchyQuickFilter = HierarchyQuickFilter::All;
		}
		ImGui::EndDisabled();
		if (context.ActiveSceneIsRuntimeClone)
		{
			ImGui::TextColored(ImVec4(0.38f, 0.72f, 1.0f, 1.0f), "Play Runtime Clone");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Hierarchy is showing the Play-mode runtime Scene clone. Stop returns to the edit Scene.");
			}
		}
		const std::string_view hierarchyFilter = TextFilter(m_HierarchyFilter);
		const bool hasHierarchyFilter = !hierarchyFilter.empty() || m_HierarchyQuickFilter != HierarchyQuickFilter::All;
		const size_t totalEntityCount = context.ActiveScene.GetEntities().size();
		size_t visibleEntityCount = 0;

		const auto setAllHierarchyExpanded = [&context](bool expanded)
		{
			for (const SceneEntity& entity : context.ActiveScene.GetEntities())
			{
				context.ActiveScene.EnsureHierarchyComponent(entity.Id).Expanded = expanded;
			}
		};

		const auto setHierarchyBranchExpanded = [&context](EntityId rootEntityId, bool expanded)
		{
			std::vector<EntityId> stack;
			std::unordered_set<EntityId> visited;
			stack.push_back(rootEntityId);
			while (!stack.empty())
			{
				const EntityId entityId = stack.back();
				stack.pop_back();
				if (!context.ActiveScene.ContainsEntity(entityId) || !visited.insert(entityId).second)
				{
					continue;
				}

				context.ActiveScene.EnsureHierarchyComponent(entityId).Expanded = expanded;
				for (const EntityId childEntity : context.ActiveScene.GetChildEntities(entityId))
				{
					stack.push_back(childEntity);
				}
			}
		};

		if (ImGui::SmallButton("Expand All"))
		{
			setAllHierarchyExpanded(true);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Expand every Hierarchy branch.");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Collapse All"))
		{
			setAllHierarchyExpanded(false);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Collapse every Hierarchy branch.");
		}

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		std::vector<EntityId> pendingDuplicateEntities;
		std::vector<EntityId> pendingDeleteEntities;
		EntityId pendingVisibilityEntity = InvalidEntityId;
		bool pendingVisibilityValue = true;
		EntityId pendingPickabilityEntity = InvalidEntityId;
		bool pendingPickabilityValue = true;
		EntityId pendingMoveEntity = InvalidEntityId;
		std::vector<EntityId> pendingMoveEntities;
		EntityId pendingMoveTarget = InvalidEntityId;
		EntityDropPlacement pendingMovePlacement = EntityDropPlacement::After;
		EntityId pendingSelectEntity = InvalidEntityId;
		EntityId pendingMakeLocalEntity = InvalidEntityId;
		EntityId pendingReloadSceneReference = InvalidEntityId;
		EntityId pendingUnloadSceneReference = InvalidEntityId;
		std::filesystem::path pendingOpenScenePath;
		EntityId pendingCreateParentEntity = InvalidEntityId;
		EntityId pendingCreateEmptyParentForEntity = InvalidEntityId;
		Asset::PrimitiveMeshKind pendingPrimitiveKind = Asset::PrimitiveMeshKind::None;
		bool createEmptyEntity = false;
		bool createCameraEntity = false;
		bool createLightEntity = false;
		if (m_DefaultParentEntity != InvalidEntityId && !context.ActiveScene.ContainsEntity(m_DefaultParentEntity))
		{
			m_DefaultParentEntity = InvalidEntityId;
		}
		std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
			{
				return !context.ActiveScene.ContainsEntity(entityId);
			});
		if (m_LastHierarchyClickedEntity != InvalidEntityId && !context.ActiveScene.ContainsEntity(m_LastHierarchyClickedEntity))
		{
			m_LastHierarchyClickedEntity = InvalidEntityId;
		}

		const auto isHierarchySelected = [this](EntityId entityId) -> bool
		{
			return std::ranges::find(m_HierarchySelection, entityId) != m_HierarchySelection.end();
		};

		const auto addHierarchySelection = [this](EntityId entityId)
		{
			if (entityId != InvalidEntityId && std::ranges::find(m_HierarchySelection, entityId) == m_HierarchySelection.end())
			{
				m_HierarchySelection.push_back(entityId);
			}
		};

		const auto setSingleHierarchySelection = [this, &context](EntityId entityId)
		{
			m_HierarchySelection.clear();
			if (entityId != InvalidEntityId)
			{
				m_HierarchySelection.push_back(entityId);
			}
			context.ActiveScene.SetSelectedEntity(entityId);
			m_LastHierarchyClickedEntity = entityId;
		};

		const auto selectHierarchyRange = [this, &context, &addHierarchySelection](EntityId rangeEnd)
		{
			if (m_LastHierarchyClickedEntity == InvalidEntityId || !context.ActiveScene.ContainsEntity(m_LastHierarchyClickedEntity))
			{
				m_HierarchySelection.clear();
				addHierarchySelection(rangeEnd);
				context.ActiveScene.SetSelectedEntity(rangeEnd);
				m_LastHierarchyClickedEntity = rangeEnd;
				return;
			}

			const size_t beginIndex = context.ActiveScene.GetEntityIndex(m_LastHierarchyClickedEntity);
			const size_t endIndex = context.ActiveScene.GetEntityIndex(rangeEnd);
			if (beginIndex == static_cast<size_t>(-1) || endIndex == static_cast<size_t>(-1))
			{
				m_HierarchySelection.clear();
				addHierarchySelection(rangeEnd);
				context.ActiveScene.SetSelectedEntity(rangeEnd);
				m_LastHierarchyClickedEntity = rangeEnd;
				return;
			}

			const size_t firstIndex = (std::min)(beginIndex, endIndex);
			const size_t lastIndex = (std::max)(beginIndex, endIndex);
			m_HierarchySelection.clear();
			const std::vector<SceneEntity>& entities = context.ActiveScene.GetEntities();
			for (size_t entityIndex = firstIndex; entityIndex <= lastIndex && entityIndex < entities.size(); ++entityIndex)
			{
				addHierarchySelection(entities[entityIndex].Id);
			}
			context.ActiveScene.SetSelectedEntity(rangeEnd);
		};

		const auto toggleHierarchySelection = [this, &context, &isHierarchySelected, &addHierarchySelection](EntityId entityId)
		{
			if (isHierarchySelected(entityId))
			{
				std::erase(m_HierarchySelection, entityId);
				context.ActiveScene.SetSelectedEntity(m_HierarchySelection.empty() ? InvalidEntityId : m_HierarchySelection.back());
			}
			else
			{
				addHierarchySelection(entityId);
				context.ActiveScene.SetSelectedEntity(entityId);
				m_LastHierarchyClickedEntity = entityId;
			}
		};

		const auto hierarchyActionEntities = [this, &context, &isHierarchySelected](EntityId fallbackEntity) -> std::vector<EntityId>
		{
			std::vector<EntityId> entities;
			if (isHierarchySelected(fallbackEntity))
			{
				for (EntityId entityId : m_HierarchySelection)
				{
					if (context.ActiveScene.ContainsEntity(entityId))
					{
						entities.push_back(entityId);
					}
				}
			}
			if (entities.empty() && fallbackEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(fallbackEntity))
			{
				entities.push_back(fallbackEntity);
			}
			return entities;
		};

		const auto entityDisplayName = [&context](EntityId entityId) -> std::string
		{
			const std::string* entityName = context.ActiveScene.GetEntityName(entityId);
			return entityName && !entityName->empty() ? *entityName : "<unnamed>";
		};

		const auto nestedSceneChildStatus = [&context](EntityId entityId) -> NestedSceneChildStatus
		{
			return context.OnGetNestedSceneChildStatus ? context.OnGetNestedSceneChildStatus(entityId) : NestedSceneChildStatus{};
		};

		const auto entityLabel = [&context](EntityId entityId, std::string displayName, bool sceneVisible, bool scenePickable) -> std::string
		{
			std::string label = std::move(displayName);
			if (context.ActiveScene.GetCameraComponent(entityId))
			{
				label.append(" [Camera]");
			}
			else if (context.ActiveScene.GetLightComponent(entityId))
			{
				label.append(" [Light]");
			}
			if (!sceneVisible)
			{
				label.append(" [Hidden]");
			}
			if (!scenePickable)
			{
				label.append(" [Locked]");
			}
			return label;
		};

		const auto entitySearchText = [&context, &entityLabel, &entityDisplayName, &nestedSceneChildStatus](EntityId entityId) -> std::string
		{
			const bool sceneVisible = context.ActiveScene.IsEntityVisibleInScene(entityId);
			const bool scenePickable = context.ActiveScene.IsEntityPickableInScene(entityId);
			std::string searchText = entityLabel(entityId, entityDisplayName(entityId), sceneVisible, scenePickable);
			if (context.ActiveScene.GetMeshComponent(entityId))
			{
				searchText.append(" Mesh");
			}
			if (context.ActiveScene.GetRigidBodyComponent(entityId))
			{
				searchText.append(" Rigidbody Physics");
			}
			if (context.ActiveScene.GetColliderComponent(entityId))
			{
				searchText.append(" Collider Physics");
			}
			if (context.ActiveScene.GetScriptComponent(entityId))
			{
				searchText.append(" Script");
			}
			if (context.ActiveScene.GetAnimatorComponent(entityId))
			{
				searchText.append(" Animator Animation");
			}
			if (context.ActiveScene.GetPrefabInstanceComponent(entityId))
			{
				searchText.append(" Prefab Instance");
			}
			if (context.ActiveScene.GetSceneReferenceComponent(entityId))
			{
				searchText.append(" SceneReference Scene Reference");
			}
			if (context.ActiveScene.GetSprite2DComponent(entityId))
			{
				searchText.append(" Sprite 2D");
			}
			if (context.ActiveScene.GetUiElementComponent(entityId))
			{
				searchText.append(" UI Element");
			}
			if (context.ActiveScene.GetAudioSourceComponent(entityId))
			{
				searchText.append(" Audio Source");
			}
			if (context.ActiveScene.GetNavigationAgentComponent(entityId))
			{
				searchText.append(" Navigation Agent");
			}
			if (context.ActiveScene.GetNetworkIdentityComponent(entityId))
			{
				searchText.append(" Network Identity");
			}
			if (nestedSceneChildStatus(entityId).IsNestedSceneChild)
			{
				searchText.append(" Nested SceneReference Runtime Child");
			}
			return searchText;
		};

		const auto matchesQuickFilterSelf = [&](EntityId entityId) -> bool
		{
			switch (m_HierarchyQuickFilter)
			{
			case HierarchyQuickFilter::All:
				return true;
			case HierarchyQuickFilter::Mesh:
				return context.ActiveScene.GetMeshComponent(entityId) != nullptr;
			case HierarchyQuickFilter::Camera:
				return context.ActiveScene.GetCameraComponent(entityId) != nullptr;
			case HierarchyQuickFilter::Light:
				return context.ActiveScene.GetLightComponent(entityId) != nullptr;
			case HierarchyQuickFilter::Physics:
				return context.ActiveScene.GetRigidBodyComponent(entityId) != nullptr ||
					context.ActiveScene.GetColliderComponent(entityId) != nullptr ||
					context.ActiveScene.GetPhysicsMaterialComponent(entityId) != nullptr;
			case HierarchyQuickFilter::Script:
				return context.ActiveScene.GetScriptComponent(entityId) != nullptr;
			case HierarchyQuickFilter::Hidden:
				return !context.ActiveScene.IsEntityVisibleInScene(entityId);
			case HierarchyQuickFilter::Locked:
				return !context.ActiveScene.IsEntityPickableInScene(entityId);
			case HierarchyQuickFilter::Nested:
				return nestedSceneChildStatus(entityId).IsNestedSceneChild;
			default:
				return true;
			}
		};

		const auto matchesSelf = [&](EntityId entityId) -> bool
		{
			return matchesQuickFilterSelf(entityId) &&
				ContainsCaseInsensitive(entitySearchText(entityId), hierarchyFilter);
		};

		std::function<bool(EntityId, std::unordered_set<EntityId>&)> matchesEntityOrDescendant =
			[&](EntityId entityId, std::unordered_set<EntityId>& visited) -> bool
		{
			if (!context.ActiveScene.ContainsEntity(entityId) || !visited.insert(entityId).second)
			{
				return false;
			}
			if (matchesSelf(entityId))
			{
				return true;
			}
			for (const EntityId childEntity : context.ActiveScene.GetChildEntities(entityId))
			{
				if (matchesEntityOrDescendant(childEntity, visited))
				{
					return true;
				}
			}
			return false;
		};

		const auto matchesHierarchyFilter = [&](EntityId entityId) -> bool
		{
			std::unordered_set<EntityId> visited;
			return matchesEntityOrDescendant(entityId, visited);
		};

		std::unordered_set<EntityId> drawnEntities;
		std::function<void(EntityId)> drawEntity = [&](EntityId entityId)
		{
			if (!context.ActiveScene.ContainsEntity(entityId) || !matchesHierarchyFilter(entityId))
			{
				return;
			}
			if (!drawnEntities.insert(entityId).second)
			{
				return;
			}
			++visibleEntityCount;

			const bool selected = entityId == selectedEntity || isHierarchySelected(entityId);
			const std::string displayName = entityDisplayName(entityId);
			const bool sceneVisible = context.ActiveScene.IsEntityVisibleInScene(entityId);
			const bool scenePickable = context.ActiveScene.IsEntityPickableInScene(entityId);
			const NestedSceneChildStatus nestedStatus = nestedSceneChildStatus(entityId);
			std::string label = entityLabel(entityId, displayName, sceneVisible, scenePickable);
			const bool showAllChildren = !hasHierarchyFilter || matchesSelf(entityId);
			std::vector<EntityId> visibleChildren;
			for (const EntityId childEntity : context.ActiveScene.GetChildEntities(entityId))
			{
				if (showAllChildren || matchesHierarchyFilter(childEntity))
				{
					visibleChildren.push_back(childEntity);
				}
			}

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (selected)
			{
				flags |= ImGuiTreeNodeFlags_Selected;
			}
			if (visibleChildren.empty())
			{
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			}
			if (!visibleChildren.empty())
			{
				const SceneHierarchyComponent* hierarchy = context.ActiveScene.GetHierarchyComponent(entityId);
				ImGui::SetNextItemOpen(hasHierarchyFilter || !hierarchy || hierarchy->Expanded, ImGuiCond_Always);
			}

			ImGui::PushID(static_cast<int>(entityId));
			if (ImGui::SmallButton(sceneVisible ? "V" : "-"))
			{
				if (context.OnEntitySceneVisibilityChanged)
				{
					context.OnEntitySceneVisibilityChanged(entityId, !sceneVisible);
				}
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(sceneVisible ? "Visible in Scene View. Click to hide from Scene View only." : "Hidden in Scene View. Game View is unaffected.");
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(scenePickable ? "P" : "L"))
			{
				if (context.OnEntityScenePickabilityChanged)
				{
					context.OnEntityScenePickabilityChanged(entityId, !scenePickable);
				}
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(scenePickable ? "Pickable in Scene View. Click to lock selection." : "Scene picking locked. Select from Hierarchy to edit.");
			}
			ImGui::SameLine();

			label.append("##");
			label.append(std::to_string(entityId));
			if (!sceneVisible)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			}
			const bool nodeOpen = ImGui::TreeNodeEx(label.c_str(), flags);
			if (!sceneVisible)
			{
				ImGui::PopStyleColor();
			}
			if (!hasHierarchyFilter && !visibleChildren.empty() && ImGui::IsItemToggledOpen())
			{
				context.ActiveScene.EnsureHierarchyComponent(entityId).Expanded = nodeOpen;
			}
			if (ImGui::IsItemClicked())
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (io.KeyShift)
				{
					selectHierarchyRange(entityId);
				}
				else if (io.KeyCtrl)
				{
					toggleHierarchySelection(entityId);
				}
				else
				{
					setSingleHierarchySelection(entityId);
				}
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{
				if (!isHierarchySelected(entityId))
				{
					setSingleHierarchySelection(entityId);
				}
				else
				{
					context.ActiveScene.SetSelectedEntity(entityId);
				}
			}

			const std::string popupId = "HierarchyEntityContext##" + std::to_string(entityId);
			if (ImGui::BeginPopupContextItem(popupId.c_str()))
			{
				context.ActiveScene.SetSelectedEntity(entityId);
				if (ImGui::MenuItem("Rename"))
				{
					OpenRenamePopup(entityId, displayName);
				}
				const std::vector<EntityId> actionEntities = hierarchyActionEntities(entityId);
				const bool hasMultiSelection = actionEntities.size() > 1;
				if (ImGui::MenuItem(hasMultiSelection ? "Duplicate Selected" : "Duplicate"))
				{
					pendingDuplicateEntities = actionEntities;
				}
				if (ImGui::MenuItem(hasMultiSelection ? "Expand Selected Branches" : "Expand Branch"))
				{
					for (const EntityId actionEntity : actionEntities)
					{
						setHierarchyBranchExpanded(actionEntity, true);
					}
				}
				if (ImGui::MenuItem(hasMultiSelection ? "Collapse Selected Branches" : "Collapse Branch"))
				{
					for (const EntityId actionEntity : actionEntities)
					{
						setHierarchyBranchExpanded(actionEntity, false);
					}
				}
				if (ImGui::BeginMenu("Create Child"))
				{
					if (ImGui::MenuItem("Empty Entity"))
					{
						createEmptyEntity = true;
						pendingCreateParentEntity = entityId;
					}
					if (ImGui::MenuItem("Camera"))
					{
						createCameraEntity = true;
						pendingCreateParentEntity = entityId;
					}
					if (ImGui::MenuItem("Light"))
					{
						createLightEntity = true;
						pendingCreateParentEntity = entityId;
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Cube"))
					{
						pendingPrimitiveKind = Asset::PrimitiveMeshKind::Cube;
						pendingCreateParentEntity = entityId;
					}
					if (ImGui::MenuItem("Sphere"))
					{
						pendingPrimitiveKind = Asset::PrimitiveMeshKind::Sphere;
						pendingCreateParentEntity = entityId;
					}
					if (ImGui::MenuItem("Capsule"))
					{
						pendingPrimitiveKind = Asset::PrimitiveMeshKind::Capsule;
						pendingCreateParentEntity = entityId;
					}
					if (ImGui::MenuItem("Plane"))
					{
						pendingPrimitiveKind = Asset::PrimitiveMeshKind::Plane;
						pendingCreateParentEntity = entityId;
					}
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Create Empty Parent"))
				{
					pendingCreateEmptyParentForEntity = entityId;
				}
				if (m_DefaultParentEntity == entityId)
				{
					if (ImGui::MenuItem("Clear Default Parent"))
					{
						m_DefaultParentEntity = InvalidEntityId;
					}
				}
				else if (ImGui::MenuItem("Set As Default Parent"))
				{
					m_DefaultParentEntity = entityId;
				}
				if (ImGui::MenuItem(sceneVisible ? "Hide In Scene View" : "Show In Scene View"))
				{
					pendingVisibilityEntity = entityId;
					pendingVisibilityValue = !sceneVisible;
				}
				if (ImGui::MenuItem(scenePickable ? "Lock Scene Picking" : "Unlock Scene Picking"))
				{
					pendingPickabilityEntity = entityId;
					pendingPickabilityValue = !scenePickable;
				}
				if (hasMultiSelection && ImGui::MenuItem("Clear Selection"))
				{
					m_HierarchySelection.clear();
					m_LastHierarchyClickedEntity = InvalidEntityId;
				}
				if (nestedStatus.IsNestedSceneChild)
				{
					ImGui::Separator();
					ImGui::TextDisabled("Nested Scene");
					if (ImGui::MenuItem(
						"Select SceneReference Owner",
						nullptr,
						false,
						nestedStatus.OwnerEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(nestedStatus.OwnerEntity)))
					{
						pendingSelectEntity = nestedStatus.OwnerEntity;
					}
					if (ImGui::MenuItem(
						"Open Source Scene",
						nullptr,
						false,
						!nestedStatus.SourceScenePath.empty() && static_cast<bool>(context.OnOpenScene)))
					{
						pendingOpenScenePath = nestedStatus.SourceScenePath;
					}
					if (ImGui::MenuItem(
						"Make Local",
						nullptr,
						false,
						static_cast<bool>(context.OnMakeNestedSceneChildLocal)))
					{
						pendingMakeLocalEntity = entityId;
					}
					if (ImGui::MenuItem(
						"Reload Source Scene",
						nullptr,
						false,
						nestedStatus.OwnerEntity != InvalidEntityId && static_cast<bool>(context.OnLoadSceneReference)))
					{
						pendingReloadSceneReference = nestedStatus.OwnerEntity;
					}
					if (ImGui::MenuItem(
						"Unload Source Scene",
						nullptr,
						false,
						nestedStatus.OwnerEntity != InvalidEntityId && static_cast<bool>(context.OnUnloadSceneReference)))
					{
						pendingUnloadSceneReference = nestedStatus.OwnerEntity;
					}
				}
				if (ImGui::MenuItem(hasMultiSelection ? "Delete Selected" : "Delete"))
				{
					pendingDeleteEntities = actionEntities;
				}
				ImGui::EndPopup();
			}

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				ImGui::SetDragDropPayload(kHierarchyEntityPayload, &entityId, sizeof(EntityId));
				ImGui::Text("Move %s", displayName.c_str());
				ImGui::EndDragDropSource();
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kHierarchyEntityPayload))
				{
					if (payload->Data && payload->DataSize == sizeof(EntityId))
					{
						const EntityId movedEntity = *static_cast<const EntityId*>(payload->Data);
						if (movedEntity != entityId)
						{
							const ImVec2 itemMin = ImGui::GetItemRectMin();
							const ImVec2 itemMax = ImGui::GetItemRectMax();
							const float itemHeight = (std::max)(1.0f, itemMax.y - itemMin.y);
							const float itemThird = itemHeight / 3.0f;
							const float mouseY = ImGui::GetMousePos().y;
							pendingMoveEntity = movedEntity;
							pendingMoveEntities = hierarchyActionEntities(movedEntity);
							pendingMoveTarget = entityId;
							if (mouseY < itemMin.y + itemThird)
							{
								pendingMovePlacement = EntityDropPlacement::Before;
							}
							else if (mouseY > itemMax.y - itemThird)
							{
								pendingMovePlacement = EntityDropPlacement::After;
							}
							else
							{
								pendingMovePlacement = EntityDropPlacement::AsChild;
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (nestedStatus.IsNestedSceneChild)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("[Nested]");
				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Text("Runtime child of SceneReference entity %u.", nestedStatus.OwnerEntity);
					if (!nestedStatus.SourceScenePath.empty())
					{
						ImGui::TextWrapped("Source: %s", nestedStatus.SourceScenePath.string().c_str());
					}
					ImGui::Text("Loaded siblings: %zu", nestedStatus.SiblingCount);
					ImGui::TextUnformatted("Excluded from scene save; reload or edit the source scene to persist changes.");
					ImGui::EndTooltip();
				}
			}

			if (nodeOpen && !visibleChildren.empty())
			{
				for (const EntityId childEntity : visibleChildren)
				{
					drawEntity(childEntity);
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		};

		for (const EntityId rootEntity : context.ActiveScene.GetRootEntities())
		{
			drawEntity(rootEntity);
		}
		for (const SceneEntity& entity : context.ActiveScene.GetEntities())
		{
			drawEntity(entity.Id);
		}

		if (hasHierarchyFilter)
		{
			ImGui::TextDisabled("%zu / %zu entities (%s)", visibleEntityCount, totalEntityCount, HierarchyQuickFilterName(m_HierarchyQuickFilter));
		}

		if (ImGui::BeginPopupContextWindow("HierarchyBlankContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			if (m_DefaultParentEntity != InvalidEntityId)
			{
				const std::string defaultParentName = entityDisplayName(m_DefaultParentEntity);
				ImGui::TextDisabled("Default Parent: %s", defaultParentName.c_str());
				if (ImGui::MenuItem("Clear Default Parent"))
				{
					m_DefaultParentEntity = InvalidEntityId;
				}
				ImGui::Separator();
			}
			if (ImGui::BeginMenu("Create"))
			{
				if (ImGui::MenuItem("Empty Entity"))
				{
					createEmptyEntity = true;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				if (ImGui::MenuItem("Camera"))
				{
					createCameraEntity = true;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				if (ImGui::MenuItem("Light"))
				{
					createLightEntity = true;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Cube"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Cube;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				if (ImGui::MenuItem("Sphere"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Sphere;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				if (ImGui::MenuItem("Capsule"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Capsule;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				if (ImGui::MenuItem("Plane"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Plane;
					pendingCreateParentEntity = m_DefaultParentEntity;
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}

		DrawRenamePopup(context);

		if (!pendingDuplicateEntities.empty())
		{
			if (pendingDuplicateEntities.size() > 1 && context.OnDuplicateEntities)
			{
				context.OnDuplicateEntities(pendingDuplicateEntities);
			}
			else if (context.OnDuplicateEntity)
			{
				for (EntityId entityId : pendingDuplicateEntities)
				{
					if (context.ActiveScene.ContainsEntity(entityId))
					{
						context.OnDuplicateEntity(entityId);
					}
				}
			}
		}
		if (!pendingDeleteEntities.empty())
		{
			if (pendingDeleteEntities.size() > 1 && context.OnDeleteEntities)
			{
				context.OnDeleteEntities(pendingDeleteEntities);
			}
			else if (context.OnDeleteEntity)
			{
				for (EntityId entityId : pendingDeleteEntities)
				{
					if (context.ActiveScene.ContainsEntity(entityId))
					{
						context.OnDeleteEntity(entityId);
					}
				}
			}
			std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
				{
					return !context.ActiveScene.ContainsEntity(entityId);
				});
			if (m_HierarchySelection.empty())
			{
				m_LastHierarchyClickedEntity = InvalidEntityId;
			}
		}
		if (pendingVisibilityEntity != InvalidEntityId && context.OnEntitySceneVisibilityChanged)
		{
			context.OnEntitySceneVisibilityChanged(pendingVisibilityEntity, pendingVisibilityValue);
		}
		if (pendingPickabilityEntity != InvalidEntityId && context.OnEntityScenePickabilityChanged)
		{
			context.OnEntityScenePickabilityChanged(pendingPickabilityEntity, pendingPickabilityValue);
		}
		if (pendingMoveEntity != InvalidEntityId && pendingMoveTarget != InvalidEntityId && context.OnMoveEntity)
		{
			if (pendingMoveEntities.size() > 1 && context.OnMoveEntities)
			{
				context.OnMoveEntities(pendingMoveEntities, pendingMoveTarget, pendingMovePlacement);
			}
			else
			{
				context.OnMoveEntity(pendingMoveEntity, pendingMoveTarget, pendingMovePlacement);
			}
		}
		if (pendingSelectEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(pendingSelectEntity))
		{
			setSingleHierarchySelection(pendingSelectEntity);
		}
		if (pendingMakeLocalEntity != InvalidEntityId && context.OnMakeNestedSceneChildLocal)
		{
			if (context.OnMakeNestedSceneChildLocal(pendingMakeLocalEntity) && context.ActiveScene.ContainsEntity(pendingMakeLocalEntity))
			{
				setSingleHierarchySelection(pendingMakeLocalEntity);
			}
		}
		if (!pendingOpenScenePath.empty() && context.OnOpenScene)
		{
			context.OnOpenScene(pendingOpenScenePath);
		}
		if (pendingReloadSceneReference != InvalidEntityId && context.OnLoadSceneReference)
		{
			static_cast<void>(context.OnLoadSceneReference(pendingReloadSceneReference));
			if (context.ActiveScene.ContainsEntity(pendingReloadSceneReference))
			{
				setSingleHierarchySelection(pendingReloadSceneReference);
			}
		}
		if (pendingUnloadSceneReference != InvalidEntityId && context.OnUnloadSceneReference)
		{
			static_cast<void>(context.OnUnloadSceneReference(pendingUnloadSceneReference));
			if (context.ActiveScene.ContainsEntity(pendingUnloadSceneReference))
			{
				setSingleHierarchySelection(pendingUnloadSceneReference);
			}
		}
		if (pendingCreateEmptyParentForEntity != InvalidEntityId && context.OnCreateEmptyParentForEntity)
		{
			context.OnCreateEmptyParentForEntity(pendingCreateEmptyParentForEntity);
		}
		if (pendingPrimitiveKind != Asset::PrimitiveMeshKind::None && context.OnCreatePrimitive)
		{
			context.OnCreatePrimitive(pendingPrimitiveKind, pendingCreateParentEntity);
		}
		if (createEmptyEntity && context.OnCreateEmptyEntity)
		{
			context.OnCreateEmptyEntity(pendingCreateParentEntity);
		}
		if (createCameraEntity && context.OnCreateCameraEntity)
		{
			context.OnCreateCameraEntity(pendingCreateParentEntity);
		}
		if (createLightEntity && context.OnCreateLightEntity)
		{
			context.OnCreateLightEntity(pendingCreateParentEntity);
		}

		ImGui::End();
	}

	void EditorLayer::HandleHierarchyShortcuts(EditorContext& context)
	{
		if (m_RenamingEntity != InvalidEntityId || m_ShouldOpenRenamePopup)
		{
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput || ImGui::IsAnyItemActive())
		{
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
		{
			OpenShortcutReference();
			return;
		}

		if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_P, false) || ImGui::IsKeyPressed(ImGuiKey_K, false)))
		{
			OpenCommandPalette();
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false))
		{
			OpenContentDrawer();
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
		{
			if (context.CanControlPlayMode && context.OnPlayModeChanged)
			{
				const bool isPlaySession =
					context.PlayState == EditorPlayState::Play ||
					context.PlayState == EditorPlayState::Paused ||
					context.PlayState == EditorPlayState::EnteringPlay;
				context.OnPlayModeChanged(!isPlaySession);
			}
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
		{
			if (context.CanControlPlayMode && context.OnPlayPausedChanged)
			{
				const bool isPaused = context.PlayState == EditorPlayState::Paused;
				if (context.PlayState == EditorPlayState::Play || isPaused)
				{
					context.OnPlayPausedChanged(!isPaused);
				}
			}
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F10, false))
		{
			if (context.CanControlPlayMode && context.PlayState == EditorPlayState::Paused && context.OnPlayStep)
			{
				context.OnPlayStep();
			}
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
		{
			if (context.CanEditProjectScene && context.OnSaveScene)
			{
				context.OnSaveScene();
			}
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
		{
			if (context.CanUndo && context.OnUndo)
			{
				context.OnUndo();
			}
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
		{
			if (context.CanRedo && context.OnRedo)
			{
				context.OnRedo();
			}
			return;
		}

		if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			if (context.OnAlignGameCameraToScene)
			{
				context.OnAlignGameCameraToScene();
			}
			return;
		}

		if (!io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			if (context.OnAlignSceneCameraToGame)
			{
				context.OnAlignSceneCameraToGame();
			}
			return;
		}

		std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
			{
				return !context.ActiveScene.ContainsEntity(entityId);
			});
		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		std::vector<EntityId> actionEntities;
		for (EntityId entityId : m_HierarchySelection)
		{
			if (context.ActiveScene.ContainsEntity(entityId))
			{
				actionEntities.push_back(entityId);
			}
		}
		if (actionEntities.empty() && selectedEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(selectedEntity))
		{
			actionEntities.push_back(selectedEntity);
		}
		if (selectedEntity == InvalidEntityId && actionEntities.empty())
		{
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
		{
			if (selectedEntity != InvalidEntityId)
			{
				if (const std::string* entityName = context.ActiveScene.GetEntityName(selectedEntity))
				{
					OpenRenamePopup(selectedEntity, *entityName);
				}
			}
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		{
			if (actionEntities.size() > 1 && context.OnDuplicateEntities)
			{
				context.OnDuplicateEntities(actionEntities);
			}
			else if (context.OnDuplicateEntity)
			{
				for (EntityId entityId : actionEntities)
				{
					if (context.ActiveScene.ContainsEntity(entityId))
					{
						context.OnDuplicateEntity(entityId);
					}
				}
			}
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			if (actionEntities.size() > 1 && context.OnDeleteEntities)
			{
				context.OnDeleteEntities(actionEntities);
				std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
					{
						return !context.ActiveScene.ContainsEntity(entityId);
					});
				if (m_HierarchySelection.empty())
				{
					m_LastHierarchyClickedEntity = InvalidEntityId;
				}
			}
			else if (context.OnDeleteEntity)
			{
				for (EntityId entityId : actionEntities)
				{
					if (context.ActiveScene.ContainsEntity(entityId))
					{
						context.OnDeleteEntity(entityId);
					}
				}
				std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
					{
						return !context.ActiveScene.ContainsEntity(entityId);
					});
				if (m_HierarchySelection.empty())
				{
					m_LastHierarchyClickedEntity = InvalidEntityId;
				}
			}
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F, false))
		{
			if (context.OnFrameSelected)
			{
				context.OnFrameSelected();
			}
		}
	}

	void EditorLayer::OpenCommandPalette()
	{
		m_CommandPaletteFilter.fill('\0');
		m_CommandPaletteLastFilter.clear();
		m_CommandPaletteSelectedIndex = 0;
		m_ShouldOpenCommandPalette = true;
		m_ShouldFocusCommandPalette = true;
	}

	void EditorLayer::OpenContentDrawer()
	{
		m_ContentDrawerFilter.fill('\0');
		m_ShouldOpenContentDrawer = true;
		m_ShouldFocusContentDrawer = true;
	}

	void EditorLayer::OpenShortcutReference()
	{
		m_ShouldOpenShortcutReference = true;
	}

	void EditorLayer::DrawCommandPalette(EditorContext& context)
	{
		if (m_ShouldOpenCommandPalette)
		{
			ImGui::OpenPopup("Command Palette");
			m_ShouldOpenCommandPalette = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowSize(ImVec2((std::min)(viewport->Size.x * 0.72f, 720.0f), 460.0f), ImGuiCond_Appearing);
		bool paletteOpen = true;
		if (!ImGui::BeginPopupModal("Command Palette", &paletteOpen, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		if (!paletteOpen || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		if (m_ShouldFocusCommandPalette)
		{
			ImGui::SetKeyboardFocusHere();
			m_ShouldFocusCommandPalette = false;
		}
		ImGui::SetNextItemWidth(-142.0f);
		const bool executeFromInput = ImGui::InputTextWithHint(
			"##CommandPaletteFilter",
			"Search commands, entities, and assets...",
			m_CommandPaletteFilter.data(),
			m_CommandPaletteFilter.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		const bool commandPaletteFilterActive = ImGui::IsItemActive();
		ImGui::SameLine();
		int commandPaletteScopeIndex = static_cast<int>(std::to_underlying(m_CommandPaletteScope));
		ImGui::SetNextItemWidth(132.0f);
		if (ImGui::Combo("##CommandPaletteScope", &commandPaletteScopeIndex, "All\0Commands\0Entities\0Assets\0"))
		{
			const int commandPaletteScopeMax = static_cast<int>(std::to_underlying(CommandPaletteScope::Assets));
			commandPaletteScopeIndex = std::clamp(commandPaletteScopeIndex, 0, commandPaletteScopeMax);
			m_CommandPaletteScope = static_cast<CommandPaletteScope>(commandPaletteScopeIndex);
			m_CommandPaletteSelectedIndex = 0;
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Command Palette scope: %s", CommandPaletteScopeName(m_CommandPaletteScope));
		}
		const std::string_view rawFilter = TextFilter(m_CommandPaletteFilter);
		CommandPaletteScope effectiveCommandPaletteScope = m_CommandPaletteScope;
		std::string commandPaletteSearch(rawFilter);
		std::string commandPalettePrefixHint;
		const auto useCommandPalettePrefix = [&](CommandPaletteScope scope, size_t prefixLength, std::string_view hint)
			{
				effectiveCommandPaletteScope = scope;
				commandPaletteSearch = TrimCopy(rawFilter.substr(prefixLength));
				commandPalettePrefixHint = std::string(hint);
			};
		if (!rawFilter.empty())
		{
			const std::string lowerRawFilter = ToLower(std::string(rawFilter));
			if (rawFilter.front() == '>')
			{
				useCommandPalettePrefix(CommandPaletteScope::Commands, 1, "> Commands");
			}
			else if (rawFilter.front() == '@')
			{
				useCommandPalettePrefix(CommandPaletteScope::Entities, 1, "@ Entities");
			}
			else if (rawFilter.front() == '/')
			{
				useCommandPalettePrefix(CommandPaletteScope::Assets, 1, "/ Assets");
			}
			else if (lowerRawFilter.starts_with("cmd:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Commands, 4, "cmd: Commands");
			}
			else if (lowerRawFilter.starts_with("command:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Commands, 8, "command: Commands");
			}
			else if (lowerRawFilter.starts_with("commands:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Commands, 9, "commands: Commands");
			}
			else if (lowerRawFilter.starts_with("entity:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Entities, 7, "entity: Entities");
			}
			else if (lowerRawFilter.starts_with("entities:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Entities, 9, "entities: Entities");
			}
			else if (lowerRawFilter.starts_with("asset:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Assets, 6, "asset: Assets");
			}
			else if (lowerRawFilter.starts_with("assets:"))
			{
				useCommandPalettePrefix(CommandPaletteScope::Assets, 7, "assets: Assets");
			}
		}
		const std::string_view filter(commandPaletteSearch);
		const std::string currentFilter = std::format(
			"{}|{}",
			std::string(rawFilter),
			std::string(CommandPaletteScopeToken(effectiveCommandPaletteScope)));
		if (currentFilter != m_CommandPaletteLastFilter)
		{
			m_CommandPaletteLastFilter = currentFilter;
			m_CommandPaletteSelectedIndex = 0;
		}
		ImGui::TextDisabled("Ctrl+P / Ctrl+K opens this palette. Prefix: > commands, @ entities, / assets.");
		if (!commandPalettePrefixHint.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("[%s]", commandPalettePrefixHint.c_str());
		}
		if (!m_CommandPalettePinnedCommands.empty())
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear Pins##CommandPalettePins"))
			{
				m_CommandPalettePinnedCommands.clear();
				SaveProjectState(context);
			}
		}
		if (!m_CommandPaletteRecentCommands.empty())
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear Recent##CommandPaletteRecent"))
			{
				m_CommandPaletteRecentCommands.clear();
				SaveProjectState(context);
			}
		}
		ImGui::Separator();

		std::vector<CommandPaletteItem> items;
		items.reserve(96);
		const auto addScopedItem = [&](std::string label, std::string detail, CommandPaletteScope scope, bool enabled, std::function<void()> execute)
			{
				const std::string searchText = label + " " + detail;
				if (!ContainsCaseInsensitive(searchText, filter))
				{
					return;
				}
				items.push_back(CommandPaletteItem{
					.Label = std::move(label),
					.Detail = std::move(detail),
					.Scope = scope,
					.Enabled = enabled,
					.Execute = std::move(execute)
				});
			};
		const auto addItem = [&](std::string label, std::string detail, bool enabled, std::function<void()> execute)
			{
				addScopedItem(std::move(label), std::move(detail), CommandPaletteScope::Commands, enabled, std::move(execute));
			};

		const bool isPaused = context.PlayState == EditorPlayState::Paused;
		const bool isPlaying = context.PlayState == EditorPlayState::Play || context.PlayState == EditorPlayState::EnteringPlay;
		const bool isPlaySession = isPlaying || isPaused;
		addItem("Save Scene", "File Ctrl+S", context.CanEditProjectScene && static_cast<bool>(context.OnSaveScene), [&context]() { context.OnSaveScene(); });
		addItem("Save Scene As", "File", context.CanEditProjectScene && static_cast<bool>(context.OnSaveSceneAs), [&context]() { context.OnSaveSceneAs(); });
		addItem("Open Scene", "File", context.CanEditProjectScene && static_cast<bool>(context.OnOpenSceneDialog), [&context]() { context.OnOpenSceneDialog(); });
		for (const std::filesystem::path& recentScenePath : m_RecentScenePaths)
		{
			std::error_code errorCode;
			const bool exists = std::filesystem::is_regular_file(recentScenePath, errorCode);
			const std::string label = std::format("Open Recent: {}", recentScenePath.filename().string());
			const std::string detail = std::format("File {}", ToStoredProjectPath(recentScenePath, context.ProjectRootPath));
			addItem(
				label,
				detail,
				context.CanEditProjectScene && exists && static_cast<bool>(context.OnOpenScene),
				[&context, recentScenePath]() { context.OnOpenScene(recentScenePath); });
		}
		addItem("Save Selected As Prefab", "File Prefab", context.CanEditProjectScene && static_cast<bool>(context.OnSaveSelectedPrefab), [&context]() { context.OnSaveSelectedPrefab(); });
		addItem(
			"Export Project Package",
			"File Build Runtime",
			context.CanEditProjectScene && (static_cast<bool>(context.OnExportProjectProfile) || static_cast<bool>(context.OnExportProject)),
			[this, &context]()
			{
				if (context.OnExportProjectProfile)
				{
					EnsureExportProfileDefaults(context);
					static_cast<void>(context.OnExportProjectProfile(BuildExportProfileSettings(context)));
				}
				else
				{
					context.OnExportProject();
				}
			});
		addItem("Reveal Project", "File Explorer", static_cast<bool>(context.OnRevealProject), [&context]() { context.OnRevealProject(); });
		addItem(isPlaySession ? "Stop Play Mode" : "Play Project Scene", "Toolbar Play F5", context.CanControlPlayMode && static_cast<bool>(context.OnPlayModeChanged), [&context, isPlaySession]() { context.OnPlayModeChanged(!isPlaySession); });
		addItem(isPaused ? "Resume Play Mode" : "Pause Play Mode", "Toolbar Play F6", context.CanControlPlayMode && (context.PlayState == EditorPlayState::Play || isPaused) && static_cast<bool>(context.OnPlayPausedChanged), [&context, isPaused]() { context.OnPlayPausedChanged(!isPaused); });
		addItem("Step Play Mode", "Toolbar Play F10", context.CanControlPlayMode && isPaused && static_cast<bool>(context.OnPlayStep), [&context]() { context.OnPlayStep(); });
		addItem("Reset Play Runtime", "Toolbar Play", context.CanControlPlayMode && context.ActiveSceneIsRuntimeClone && static_cast<bool>(context.OnResetPlayRuntimeScene), [&context]() { context.OnResetPlayRuntimeScene(); });
		addItem("Undo", "Edit Ctrl+Z", context.CanUndo && static_cast<bool>(context.OnUndo), [&context]() { context.OnUndo(); });
		addItem("Redo", "Edit Ctrl+Y", context.CanRedo && static_cast<bool>(context.OnRedo), [&context]() { context.OnRedo(); });
		addItem("Save Editor Layout", "Window Docking", true, [this, &context]() { SaveCurrentEditorLayout(context); });
		addItem("Restore Saved Layout", "Window Docking", !m_ProjectEditorLayoutIni.empty(), [this]() { RestoreSavedEditorLayout(); });
		addItem("Reset Editor Layout", "Window Docking", true, [this]() { ResetEditorLayoutToDefault(); });
		addItem("Open Content Drawer", "Window Project Ctrl+Space", true, [this]() { OpenContentDrawer(); });
		addItem("Keyboard Shortcuts", "Help F1", true, [this]() { OpenShortcutReference(); });
		addItem("Frame Selected", "Scene View F", context.ActiveScene.GetSelectedEntity() != InvalidEntityId && static_cast<bool>(context.OnFrameSelected), [&context]() { context.OnFrameSelected(); });
		addItem("Align Game Camera To Scene", "Camera Ctrl+Shift+F", context.CanEditProjectScene && static_cast<bool>(context.OnAlignGameCameraToScene), [&context]() { context.OnAlignGameCameraToScene(); });
		addItem("Align Scene Camera To Game", "Camera Shift+F", static_cast<bool>(context.OnAlignSceneCameraToGame), [&context]() { context.OnAlignSceneCameraToGame(); });
		addItem("Refresh Project Assets", "Project", static_cast<bool>(context.OnProjectRefresh), [&context]() { context.OnProjectRefresh(); });
		if (context.ProjectSnapshot && context.ProjectSnapshot->RootExists && context.OnCreateProjectAsset)
		{
			const std::filesystem::path commandTargetDirectory = [&]() -> std::filesystem::path
			{
				std::error_code errorCode;
				if (!m_SelectedAssetPath.empty())
				{
					if (std::filesystem::is_directory(m_SelectedAssetPath, errorCode))
					{
						return m_SelectedAssetPath;
					}
					if (std::filesystem::is_regular_file(m_SelectedAssetPath, errorCode))
					{
						return m_SelectedAssetPath.parent_path();
					}
				}
				return context.ProjectSnapshot->RootPath;
			}();
			addItem("Create Asset: Folder", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Folder, commandTargetDirectory); });
			addItem("Create Asset: Scene", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Scene, commandTargetDirectory); });
			addItem("Create Asset: Material", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Material, commandTargetDirectory); });
			addItem("Create Asset: Skybox", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Skybox, commandTargetDirectory); });
			addItem("Create Asset: Script", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Script, commandTargetDirectory); });
			addItem("Create Asset: Prefab", "Project Create", context.CanEditProjectScene, [&context, commandTargetDirectory]() { context.OnCreateProjectAsset(ProjectCreateAssetKind::Prefab, commandTargetDirectory); });
		}

		addItem("Switch Graphics API: DirectX12", "Rendering", static_cast<bool>(context.OnGraphicsApiChanged), [&context]() { context.OnGraphicsApiChanged(GraphicsAPI::DirectX12); });
		addItem("Switch Graphics API: Vulkan", "Rendering", static_cast<bool>(context.OnGraphicsApiChanged), [&context]() { context.OnGraphicsApiChanged(GraphicsAPI::Vulkan); });
		addItem("Render Mode: Forward", "Rendering", static_cast<bool>(context.OnRenderModeChanged), [&context]() { context.OnRenderModeChanged(RenderMode::Forward); });
		addItem("Render Mode: Deferred", "Rendering", static_cast<bool>(context.OnRenderModeChanged), [&context]() { context.OnRenderModeChanged(RenderMode::Deferred); });
		addItem("Render Mode: Forward+", "Rendering", static_cast<bool>(context.OnRenderModeChanged), [&context]() { context.OnRenderModeChanged(RenderMode::ForwardPlus); });

		if (m_DefaultParentEntity != InvalidEntityId && !context.ActiveScene.ContainsEntity(m_DefaultParentEntity))
		{
			m_DefaultParentEntity = InvalidEntityId;
		}
		const EntityId defaultParentEntity = m_DefaultParentEntity;
		addItem("Create Empty Entity", "Hierarchy Create", context.CanEditProjectScene && static_cast<bool>(context.OnCreateEmptyEntity), [&context, defaultParentEntity]() { context.OnCreateEmptyEntity(defaultParentEntity); });
		addItem("Create Camera", "Hierarchy Create", context.CanEditProjectScene && static_cast<bool>(context.OnCreateCameraEntity), [&context, defaultParentEntity]() { context.OnCreateCameraEntity(defaultParentEntity); });
		addItem("Create Light", "Hierarchy Create", context.CanEditProjectScene && static_cast<bool>(context.OnCreateLightEntity), [&context, defaultParentEntity]() { context.OnCreateLightEntity(defaultParentEntity); });
		addItem("Create Cube", "Hierarchy Primitive", context.CanEditProjectScene && static_cast<bool>(context.OnCreatePrimitive), [&context, defaultParentEntity]() { context.OnCreatePrimitive(Asset::PrimitiveMeshKind::Cube, defaultParentEntity); });
		addItem("Create Sphere", "Hierarchy Primitive", context.CanEditProjectScene && static_cast<bool>(context.OnCreatePrimitive), [&context, defaultParentEntity]() { context.OnCreatePrimitive(Asset::PrimitiveMeshKind::Sphere, defaultParentEntity); });
		addItem("Create Capsule", "Hierarchy Primitive", context.CanEditProjectScene && static_cast<bool>(context.OnCreatePrimitive), [&context, defaultParentEntity]() { context.OnCreatePrimitive(Asset::PrimitiveMeshKind::Capsule, defaultParentEntity); });
		addItem("Create Plane", "Hierarchy Primitive", context.CanEditProjectScene && static_cast<bool>(context.OnCreatePrimitive), [&context, defaultParentEntity]() { context.OnCreatePrimitive(Asset::PrimitiveMeshKind::Plane, defaultParentEntity); });

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (selectedEntity != InvalidEntityId)
		{
			const std::string selectedName = context.ActiveScene.GetEntityName(selectedEntity)
				? *context.ActiveScene.GetEntityName(selectedEntity)
				: std::string("<unnamed>");
			const bool selectedSceneVisible = context.ActiveScene.IsEntityVisibleInScene(selectedEntity);
			const bool selectedScenePickable = context.ActiveScene.IsEntityPickableInScene(selectedEntity);
			addItem(
				"Create Empty Child",
				std::format("Hierarchy {}", selectedName),
				context.CanEditProjectScene && static_cast<bool>(context.OnCreateEmptyEntity),
				[&context, selectedEntity]() { context.OnCreateEmptyEntity(selectedEntity); });
			addItem(
				"Create Empty Parent",
				std::format("Hierarchy {}", selectedName),
				context.CanEditProjectScene && static_cast<bool>(context.OnCreateEmptyParentForEntity),
				[&context, selectedEntity]() { context.OnCreateEmptyParentForEntity(selectedEntity); });
			addItem(
				"Set Selected As Default Parent",
				std::format("Hierarchy {}", selectedName),
				context.CanEditProjectScene,
				[this, selectedEntity]() { m_DefaultParentEntity = selectedEntity; });
			addItem(
				"Clear Default Parent",
				"Hierarchy",
				m_DefaultParentEntity != InvalidEntityId,
				[this]() { m_DefaultParentEntity = InvalidEntityId; });
			addItem(
				selectedSceneVisible ? "Hide Selected In Scene View" : "Show Selected In Scene View",
				"Hierarchy Visibility",
				context.CanEditProjectScene && static_cast<bool>(context.OnEntitySceneVisibilityChanged),
				[&context, selectedEntity, selectedSceneVisible]() { context.OnEntitySceneVisibilityChanged(selectedEntity, !selectedSceneVisible); });
			addItem(
				selectedScenePickable ? "Lock Selected Scene Picking" : "Unlock Selected Scene Picking",
				"Hierarchy Pickability",
				context.CanEditProjectScene && static_cast<bool>(context.OnEntityScenePickabilityChanged),
				[&context, selectedEntity, selectedScenePickable]() { context.OnEntityScenePickabilityChanged(selectedEntity, !selectedScenePickable); });

			const auto addComponentCommand = [&](SceneComponentKind kind, const char* name, bool alreadyExists)
				{
					addItem(
						std::format("Add Component: {}", name),
						std::format("Inspector {}", selectedName),
						context.CanEditProjectScene && !alreadyExists && static_cast<bool>(context.OnComponentAdded),
						[&context, selectedEntity, kind]() { context.OnComponentAdded(selectedEntity, kind); });
				};
			addComponentCommand(SceneComponentKind::Mesh, "Mesh", context.ActiveScene.GetMeshComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::Camera, "Camera", context.ActiveScene.GetCameraComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::Light, "Light", context.ActiveScene.GetLightComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::Script, "Script", context.ActiveScene.GetScriptComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::RigidBody, "Rigidbody", context.ActiveScene.GetRigidBodyComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::Collider, "Collider", context.ActiveScene.GetColliderComponent(selectedEntity) != nullptr);
			addComponentCommand(SceneComponentKind::SceneReference, "Scene Reference", context.ActiveScene.GetSceneReferenceComponent(selectedEntity) != nullptr);
			if (const SceneReferenceComponent* sceneReference = context.ActiveScene.GetSceneReferenceComponent(selectedEntity))
			{
				const bool hasScenePath = !sceneReference->ScenePath.empty();
				const SceneReferenceRuntimeStatus runtimeStatus = context.OnGetSceneReferenceStatus
					? context.OnGetSceneReferenceStatus(selectedEntity)
					: SceneReferenceRuntimeStatus{};
				addItem(
					"Open Referenced Scene",
					std::format("Scene Reference {}", selectedName),
					context.CanEditProjectScene && hasScenePath && static_cast<bool>(context.OnOpenScene),
					[&context, scenePath = sceneReference->ScenePath]() { context.OnOpenScene(scenePath); });
				addItem(
					"Load Referenced Scene As Children",
					std::format("Scene Reference {}", selectedName),
					context.CanEditProjectScene && hasScenePath && sceneReference->LoadAdditively && static_cast<bool>(context.OnLoadSceneReference),
					[&context, selectedEntity]() { static_cast<void>(context.OnLoadSceneReference(selectedEntity)); });
				addItem(
					"Reload Referenced Scene Children",
					std::format("Scene Reference {}", selectedName),
					context.CanEditProjectScene && runtimeStatus.Loaded && hasScenePath && sceneReference->LoadAdditively && static_cast<bool>(context.OnLoadSceneReference),
					[&context, selectedEntity]() { static_cast<void>(context.OnLoadSceneReference(selectedEntity)); });
				addItem(
					"Unload Referenced Scene Children",
					std::format("Scene Reference {}", selectedName),
					context.CanEditProjectScene && runtimeStatus.Loaded && static_cast<bool>(context.OnUnloadSceneReference),
					[&context, selectedEntity]() { static_cast<void>(context.OnUnloadSceneReference(selectedEntity)); });
			}
			const NestedSceneChildStatus nestedStatus = context.OnGetNestedSceneChildStatus
				? context.OnGetNestedSceneChildStatus(selectedEntity)
				: NestedSceneChildStatus{};
			if (nestedStatus.IsNestedSceneChild)
			{
				addItem(
					"Make Nested Scene Child Local",
					std::format("Nested Scene {}", selectedName),
					context.CanEditProjectScene && static_cast<bool>(context.OnMakeNestedSceneChildLocal),
					[&context, selectedEntity]() { static_cast<void>(context.OnMakeNestedSceneChildLocal(selectedEntity)); });
				addItem(
					"Select Nested Scene Owner",
					std::format("Nested Scene {}", selectedName),
					nestedStatus.OwnerEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(nestedStatus.OwnerEntity),
					[&context, ownerEntity = nestedStatus.OwnerEntity]() { context.ActiveScene.SetSelectedEntity(ownerEntity); });
				addItem(
					"Open Nested Source Scene",
					std::format("Nested Scene {}", selectedName),
					context.CanEditProjectScene && !nestedStatus.SourceScenePath.empty() && static_cast<bool>(context.OnOpenScene),
					[&context, scenePath = nestedStatus.SourceScenePath]() { context.OnOpenScene(scenePath); });
			}
		}

		if (!filter.empty() || effectiveCommandPaletteScope == CommandPaletteScope::Entities)
		{
			for (const SceneEntity& entity : context.ActiveScene.GetEntities())
			{
				const std::string entityName = context.ActiveScene.GetEntityName(entity.Id)
					? *context.ActiveScene.GetEntityName(entity.Id)
					: std::string("<unnamed>");
				std::string detail = std::format("Entity {}", entity.Id);
				if (context.ActiveScene.GetCameraComponent(entity.Id))
				{
					detail.append(" Camera");
				}
				if (context.ActiveScene.GetLightComponent(entity.Id))
				{
					detail.append(" Light");
				}
				if (context.ActiveScene.GetMeshComponent(entity.Id))
				{
					detail.append(" Mesh");
				}
				addScopedItem("Select Entity: " + entityName, detail, CommandPaletteScope::Entities, true, [&context, entityId = entity.Id]() { context.ActiveScene.SetSelectedEntity(entityId); });
				addScopedItem("Frame Entity: " + entityName, detail, CommandPaletteScope::Entities, static_cast<bool>(context.OnFrameSelected), [&context, entityId = entity.Id]()
					{
						context.ActiveScene.SetSelectedEntity(entityId);
						context.OnFrameSelected();
					});
			}
		}

		if (!filter.empty())
		{
			if (context.ProjectSnapshot && context.ProjectSnapshot->RootExists)
			{
				for (const Asset::AssetFileEntry& entry : context.ProjectSnapshot->Children)
				{
					AddProjectEntryCommands(entry, *context.ProjectSnapshot, context, items, filter, m_SelectedAssetPath);
				}
			}
		}

		if (effectiveCommandPaletteScope != CommandPaletteScope::All)
		{
			std::erase_if(items, [effectiveCommandPaletteScope](const CommandPaletteItem& item)
				{
					return item.Scope != effectiveCommandPaletteScope;
				});
		}

		bool commandExecuted = false;
		const auto executeItem = [&](CommandPaletteItem& item)
			{
				if (!item.Enabled || !item.Execute)
				{
					return;
				}
				std::erase(m_CommandPaletteRecentCommands, item.Label);
				m_CommandPaletteRecentCommands.insert(m_CommandPaletteRecentCommands.begin(), item.Label);
				constexpr size_t maxRecentCommandCount = 12;
				if (m_CommandPaletteRecentCommands.size() > maxRecentCommandCount)
				{
					m_CommandPaletteRecentCommands.resize(maxRecentCommandCount);
				}
				SaveProjectState(context);
				item.Execute();
				commandExecuted = true;
				ImGui::CloseCurrentPopup();
			};

		constexpr size_t maxCommandPaletteRows = 96;
		constexpr size_t invalidCommandPaletteIndex = static_cast<size_t>(-1);
		if ((!m_CommandPalettePinnedCommands.empty() || !m_CommandPaletteRecentCommands.empty()) && !items.empty())
		{
			std::vector<CommandPaletteItem> orderedItems;
			orderedItems.reserve(items.size());
			std::unordered_set<std::string> promotedLabels;
			const auto promoteCommand = [&items, &orderedItems, &promotedLabels](const std::string& commandLabel, const char* prefix)
				{
					auto itemIt = std::ranges::find_if(items, [&commandLabel, &promotedLabels](const CommandPaletteItem& item)
					{
						return item.Label == commandLabel && !promotedLabels.contains(item.Label);
					});
					if (itemIt == items.end())
					{
						return;
					}

					if (!itemIt->Detail.empty() &&
						!itemIt->Detail.starts_with("Pinned ") &&
						!itemIt->Detail.starts_with("Recent "))
					{
						itemIt->Detail = std::string(prefix) + itemIt->Detail;
					}
					promotedLabels.insert(itemIt->Label);
					orderedItems.push_back(*itemIt);
				};
			for (const std::string& pinnedLabel : m_CommandPalettePinnedCommands)
			{
				promoteCommand(pinnedLabel, "Pinned ");
			}
			for (const std::string& recentLabel : m_CommandPaletteRecentCommands)
			{
				promoteCommand(recentLabel, "Recent ");
			}

			for (CommandPaletteItem& item : items)
			{
				if (!promotedLabels.contains(item.Label))
				{
					orderedItems.push_back(std::move(item));
				}
			}
			items = std::move(orderedItems);
		}

		const size_t shownItemCount = (std::min)(items.size(), maxCommandPaletteRows);
		const auto commandCanExecute = [](const CommandPaletteItem& item)
			{
				return item.Enabled && static_cast<bool>(item.Execute);
			};
		std::vector<size_t> enabledCommandIndexes;
		enabledCommandIndexes.reserve(shownItemCount);
		for (size_t itemIndex = 0; itemIndex < shownItemCount; ++itemIndex)
		{
			if (commandCanExecute(items[itemIndex]))
			{
				enabledCommandIndexes.push_back(itemIndex);
			}
		}
		if (enabledCommandIndexes.empty())
		{
			m_CommandPaletteSelectedIndex = 0;
		}
		else if (std::ranges::find(enabledCommandIndexes, m_CommandPaletteSelectedIndex) == enabledCommandIndexes.end())
		{
			m_CommandPaletteSelectedIndex = enabledCommandIndexes.front();
		}

		bool scrollCommandPaletteSelectionIntoView = false;
		const auto selectEnabledCommandAt = [&](size_t enabledPosition)
			{
				if (enabledCommandIndexes.empty())
				{
					return;
				}
				enabledPosition = std::clamp(enabledPosition, size_t{ 0 }, enabledCommandIndexes.size() - 1);
				m_CommandPaletteSelectedIndex = enabledCommandIndexes[enabledPosition];
				scrollCommandPaletteSelectionIntoView = true;
			};
		const auto selectedEnabledPosition = [&]() -> size_t
			{
				const auto it = std::ranges::find(enabledCommandIndexes, m_CommandPaletteSelectedIndex);
				if (it == enabledCommandIndexes.end())
				{
					return invalidCommandPaletteIndex;
				}
				return static_cast<size_t>(std::distance(enabledCommandIndexes.begin(), it));
			};
		const bool commandPaletteKeyboardFocused =
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			(!ImGui::IsAnyItemActive() || commandPaletteFilterActive);
		if (commandPaletteKeyboardFocused && !enabledCommandIndexes.empty())
		{
			const size_t currentPosition = selectedEnabledPosition() == invalidCommandPaletteIndex ? 0 : selectedEnabledPosition();
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
			{
				selectEnabledCommandAt((std::min)(currentPosition + 1, enabledCommandIndexes.size() - 1));
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
			{
				selectEnabledCommandAt(currentPosition == 0 ? 0 : currentPosition - 1);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
			{
				selectEnabledCommandAt((std::min)(currentPosition + 10, enabledCommandIndexes.size() - 1));
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
			{
				selectEnabledCommandAt(currentPosition > 10 ? currentPosition - 10 : 0);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
			{
				selectEnabledCommandAt(0);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_End, false))
			{
				selectEnabledCommandAt(enabledCommandIndexes.size() - 1);
			}
		}

		const auto executeSelectedCommand = [&]() -> bool
			{
				if (m_CommandPaletteSelectedIndex >= shownItemCount || !commandCanExecute(items[m_CommandPaletteSelectedIndex]))
				{
					return false;
				}
				executeItem(items[m_CommandPaletteSelectedIndex]);
				return commandExecuted;
			};
		if ((executeFromInput ||
			(!commandPaletteFilterActive && commandPaletteKeyboardFocused &&
				(ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))) &&
			executeSelectedCommand())
		{
			ImGui::EndPopup();
			return;
		}

		ImGui::BeginChild("CommandPaletteResults", ImVec2(0.0f, 330.0f), true);
		size_t shownCount = 0;
		for (CommandPaletteItem& item : items)
		{
			if (shownCount >= maxCommandPaletteRows)
			{
				break;
			}
			ImGui::PushID(static_cast<int>(shownCount));
			if (!item.Enabled)
			{
				ImGui::BeginDisabled();
			}
			const bool selected = shownCount == m_CommandPaletteSelectedIndex && commandCanExecute(item);
			if (ImGui::Selectable(item.Label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				m_CommandPaletteSelectedIndex = shownCount;
				executeItem(item);
			}
			if (!item.Enabled)
			{
				ImGui::EndDisabled();
			}
			if (selected && scrollCommandPaletteSelectionIntoView)
			{
				ImGui::SetScrollHereY(0.5f);
			}
			if (!item.Detail.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("%s", item.Detail.c_str());
			}
			ImGui::SameLine();
			const bool commandPinned = std::ranges::find(m_CommandPalettePinnedCommands, item.Label) != m_CommandPalettePinnedCommands.end();
			if (ImGui::SmallButton(commandPinned ? "Unpin" : "Pin"))
			{
				if (commandPinned)
				{
					std::erase(m_CommandPalettePinnedCommands, item.Label);
				}
				else
				{
					std::erase(m_CommandPalettePinnedCommands, item.Label);
					m_CommandPalettePinnedCommands.insert(m_CommandPalettePinnedCommands.begin(), item.Label);
					constexpr size_t maxPinnedCommandCount = 16;
					if (m_CommandPalettePinnedCommands.size() > maxPinnedCommandCount)
					{
						m_CommandPalettePinnedCommands.resize(maxPinnedCommandCount);
					}
				}
				SaveProjectState(context);
			}
			ImGui::PopID();
			++shownCount;
			if (commandExecuted)
			{
				break;
			}
		}
		if (items.empty())
		{
			if (effectiveCommandPaletteScope == CommandPaletteScope::Assets && filter.empty())
			{
				ImGui::TextDisabled("Type a search term to find project assets.");
			}
			else
			{
				ImGui::TextDisabled("No matching %s result.", CommandPaletteScopeName(effectiveCommandPaletteScope));
			}
		}
		else if (items.size() > shownItemCount)
		{
			ImGui::TextDisabled("Showing first %zu of %zu result(s). Refine the search to narrow the palette.", shownItemCount, items.size());
		}
		ImGui::EndChild();
		if (commandExecuted)
		{
			ImGui::EndPopup();
			return;
		}
		ImGui::TextDisabled("%zu %s result(s)", items.size(), CommandPaletteScopeName(effectiveCommandPaletteScope));
		ImGui::EndPopup();
	}

	void EditorLayer::OpenRenamePopup(EntityId entityId, std::string_view currentName)
	{
		m_RenamingEntity = entityId;
		m_RenameBuffer.fill('\0');
		const size_t copyLength = (std::min)(currentName.size(), m_RenameBuffer.size() - 1);
		std::copy_n(currentName.data(), copyLength, m_RenameBuffer.data());
		m_ShouldOpenRenamePopup = true;
		m_ShouldFocusRenameInput = true;
	}

	void EditorLayer::DrawRenamePopup(EditorContext& context)
	{
		if (m_ShouldOpenRenamePopup)
		{
			ImGui::OpenPopup("Rename Entity");
			m_ShouldOpenRenamePopup = false;
		}

		if (!ImGui::BeginPopupModal("Rename Entity", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			return;
		}

		if (!context.ActiveScene.ContainsEntity(m_RenamingEntity))
		{
			m_RenamingEntity = InvalidEntityId;
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		if (m_ShouldFocusRenameInput)
		{
			ImGui::SetKeyboardFocusHere();
			m_ShouldFocusRenameInput = false;
		}
		bool applyRename = ImGui::InputText("Name", m_RenameBuffer.data(), m_RenameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue);
		const bool hasName = m_RenameBuffer[0] != '\0';
		if (!hasName)
		{
			ImGui::BeginDisabled();
		}
		applyRename |= ImGui::Button("Apply");
		if (!hasName)
		{
			ImGui::EndDisabled();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_RenamingEntity = InvalidEntityId;
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			m_RenamingEntity = InvalidEntityId;
			ImGui::CloseCurrentPopup();
		}

		if (applyRename && hasName && m_RenamingEntity != InvalidEntityId && context.OnRenameEntity)
		{
			context.OnRenameEntity(m_RenamingEntity, std::string_view(m_RenameBuffer.data()));
			m_RenamingEntity = InvalidEntityId;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	void EditorLayer::DrawContentDrawer(EditorContext& context)
	{
		if (m_ShouldOpenContentDrawer)
		{
			ImGui::OpenPopup("Content Drawer");
			m_ShouldOpenContentDrawer = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowSize(ImVec2((std::min)(viewport->Size.x * 0.82f, 980.0f), (std::min)(viewport->Size.y * 0.64f, 560.0f)), ImGuiCond_Appearing);
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.82f), ImGuiCond_Appearing, ImVec2(0.5f, 1.0f));
		bool drawerOpen = true;
		if (!ImGui::BeginPopupModal("Content Drawer", &drawerOpen, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		if (!drawerOpen || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		if (m_ShouldFocusContentDrawer)
		{
			ImGui::SetKeyboardFocusHere();
			m_ShouldFocusContentDrawer = false;
		}

		const bool openFirstResult = ImGui::InputTextWithHint(
			"##ContentDrawerFilter",
			"Search project assets...",
			m_ContentDrawerFilter.data(),
			m_ContentDrawerFilter.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		const bool contentDrawerFilterActive = ImGui::IsItemActive();
		ImGui::SameLine();
		int drawerQuickFilterIndex = static_cast<int>(std::to_underlying(m_ProjectQuickFilter));
		ImGui::SetNextItemWidth(132.0f);
		if (ImGui::Combo("##ContentDrawerQuickFilter", &drawerQuickFilterIndex, "All\0Favorites\0Folders\0Models\0Images\0Scenes\0Materials\0Prefabs\0Source\0Text\0"))
		{
			const int drawerQuickFilterMax = static_cast<int>(std::to_underlying(ProjectQuickFilter::Text));
			drawerQuickFilterIndex = std::clamp(drawerQuickFilterIndex, 0, drawerQuickFilterMax);
			m_ProjectQuickFilter = static_cast<ProjectQuickFilter>(drawerQuickFilterIndex);
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Content Drawer asset type filter: %s", ProjectQuickFilterName(m_ProjectQuickFilter));
		}
		ImGui::SameLine();
		int drawerSortModeIndex = static_cast<int>(std::to_underlying(m_ContentDrawerSortMode));
		ImGui::SetNextItemWidth(116.0f);
		if (ImGui::Combo("##ContentDrawerSortMode", &drawerSortModeIndex, "Path\0Name\0Type\0Size\0Modified\0"))
		{
			const ContentDrawerSortMode previousSortMode = m_ContentDrawerSortMode;
			const int drawerSortModeMax = static_cast<int>(std::to_underlying(ContentDrawerSortMode::ModifiedDescending));
			drawerSortModeIndex = std::clamp(drawerSortModeIndex, 0, drawerSortModeMax);
			m_ContentDrawerSortMode = static_cast<ContentDrawerSortMode>(drawerSortModeIndex);
			if (m_ContentDrawerSortMode != previousSortMode)
			{
				m_ContentDrawerSortDescending =
					m_ContentDrawerSortMode == ContentDrawerSortMode::SizeDescending ||
					m_ContentDrawerSortMode == ContentDrawerSortMode::ModifiedDescending;
			}
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Content Drawer sort mode: %s", ContentDrawerSortModeName(m_ContentDrawerSortMode));
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("Desc##ContentDrawerSortDirection", &m_ContentDrawerSortDescending))
		{
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Toggle descending sort order for the Content Drawer.");
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("Details##ContentDrawerDetails", &m_ContentDrawerDetailsVisible))
		{
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show the selected asset details and preview inside the Content Drawer.");
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Search"))
		{
			const std::string search(TextFilter(m_ContentDrawerFilter));
			if (!search.empty() && std::ranges::find(m_ProjectSavedSearches, search) == m_ProjectSavedSearches.end())
			{
				m_ProjectSavedSearches.push_back(search);
				SaveProjectState(context);
			}
		}
		if (!m_ProjectSavedSearches.empty())
		{
			ImGui::SameLine();
			if (ImGui::BeginCombo("##ContentDrawerSavedSearches", "Saved Searches"))
			{
				for (size_t searchIndex = 0; searchIndex < m_ProjectSavedSearches.size(); ++searchIndex)
				{
					const std::string& search = m_ProjectSavedSearches[searchIndex];
					ImGui::PushID(static_cast<int>(searchIndex));
					if (ImGui::Selectable(search.c_str()))
					{
						SetTextBuffer(m_ContentDrawerFilter, search);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("x"))
					{
						m_ProjectSavedSearches.erase(m_ProjectSavedSearches.begin() + static_cast<std::ptrdiff_t>(searchIndex));
						SaveProjectState(context);
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Filters"))
		{
			m_ContentDrawerFilter.fill('\0');
			m_ProjectQuickFilter = ProjectQuickFilter::All;
			SaveProjectState(context);
		}
		ImGui::SameLine();
		if (ImGui::Button("Refresh") && context.OnProjectRefresh)
		{
			context.OnProjectRefresh();
		}
		ImGui::SameLine();
		if (ImGui::Button("Close"))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const auto drawContentDrawerActiveFilters = [&]()
		{
			const std::string_view drawerFilter = TextFilter(m_ContentDrawerFilter);
			const bool hasQuickFilter = m_ProjectQuickFilter != ProjectQuickFilter::All;
			const bool hasTextFilter = !drawerFilter.empty();
			if (!hasQuickFilter && !hasTextFilter)
			{
				return;
			}

			ImGui::TextDisabled("Active");
			if (hasQuickFilter)
			{
				const std::string typeLabel = std::format("Type: {}", ProjectQuickFilterName(m_ProjectQuickFilter));
				ImGui::SameLine();
				ImGui::TextUnformatted(typeLabel.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("x##ClearContentDrawerTypeFilter"))
				{
					m_ProjectQuickFilter = ProjectQuickFilter::All;
					SaveProjectState(context);
				}
			}
			if (hasTextFilter)
			{
				const std::string textLabel = std::format("Text: {}", std::string(drawerFilter));
				ImGui::SameLine();
				ImGui::TextUnformatted(textLabel.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("x##ClearContentDrawerTextFilter"))
				{
					m_ContentDrawerFilter.fill('\0');
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear All##ClearAllContentDrawerActiveFilters"))
			{
				m_ProjectQuickFilter = ProjectQuickFilter::All;
				m_ContentDrawerFilter.fill('\0');
				SaveProjectState(context);
			}
		};
		drawContentDrawerActiveFilters();
		ImGui::TextDisabled("Ctrl+Space opens this drawer. Up/Down selects, Enter opens, Ctrl+Enter loads a model into the scene.");
		ImGui::Separator();

		const auto snapshot = context.ProjectSnapshot;
		if (!snapshot)
		{
			ImGui::TextUnformatted("Project snapshot is loading.");
			ImGui::EndPopup();
			return;
		}
		if (!snapshot->RootExists)
		{
			ImGui::TextWrapped("%s", snapshot->Status.c_str());
			ImGui::EndPopup();
			return;
		}

		const std::string_view filter = TextFilter(m_ContentDrawerFilter);
		struct DrawerAssetRow
		{
			const Asset::AssetFileEntry* Entry = nullptr;
			std::string RelativePath;
		};
		std::vector<DrawerAssetRow> rows;
		rows.reserve(128);
		const auto collectRows = [&](const auto& self, const Asset::AssetFileEntry& entry) -> void
			{
				const std::string relativePath = RelativeDisplayPath(entry.Path, snapshot->RootPath);
				const std::string searchText = entry.Name + " " + relativePath + " " + AssetKindTag(entry.Kind);
				const bool matches = ContainsCaseInsensitive(searchText, filter) &&
					ProjectEntryMatchesQuickFilter(entry, m_ProjectQuickFilter, m_ProjectFavoritePaths);
				const bool includeDirectory =
					entry.Kind == Asset::AssetFileKind::Directory &&
					(m_ProjectQuickFilter == ProjectQuickFilter::Folders ||
						(m_ProjectQuickFilter == ProjectQuickFilter::Favorites && IsFavoriteProjectPath(m_ProjectFavoritePaths, entry.Path)));
				if (matches && (entry.Kind != Asset::AssetFileKind::Directory || includeDirectory))
				{
					rows.push_back(DrawerAssetRow{ .Entry = &entry, .RelativePath = relativePath });
				}
				for (const Asset::AssetFileEntry& child : entry.Children)
				{
					self(self, child);
				}
			};
		for (const Asset::AssetFileEntry& entry : snapshot->Children)
		{
			collectRows(collectRows, entry);
		}

		std::ranges::sort(rows, [this](const DrawerAssetRow& lhs, const DrawerAssetRow& rhs)
			{
				const Asset::AssetFileEntry& lhsEntry = *lhs.Entry;
				const Asset::AssetFileEntry& rhsEntry = *rhs.Entry;
				int comparison = 0;
				const auto compareLower = [](const std::string& left, const std::string& right)
				{
					return ToLower(left).compare(ToLower(right));
				};
				switch (m_ContentDrawerSortMode)
				{
				case ContentDrawerSortMode::Name:
					comparison = compareLower(lhsEntry.Name, rhsEntry.Name);
					break;
				case ContentDrawerSortMode::Type:
					if (lhsEntry.Kind != rhsEntry.Kind)
					{
						comparison = std::to_underlying(lhsEntry.Kind) < std::to_underlying(rhsEntry.Kind) ? -1 : 1;
						break;
					}
					comparison = compareLower(lhsEntry.Extension, rhsEntry.Extension);
					break;
				case ContentDrawerSortMode::SizeDescending:
					if (lhsEntry.SizeBytes != rhsEntry.SizeBytes)
					{
						comparison = lhsEntry.SizeBytes < rhsEntry.SizeBytes ? -1 : 1;
					}
					break;
				case ContentDrawerSortMode::ModifiedDescending:
					if (lhsEntry.LastWriteTimeTicks != rhsEntry.LastWriteTimeTicks)
					{
						comparison = lhsEntry.LastWriteTimeTicks < rhsEntry.LastWriteTimeTicks ? -1 : 1;
					}
					break;
				case ContentDrawerSortMode::Path:
				default:
					break;
				}

				if (comparison == 0)
				{
					comparison = lhs.RelativePath.compare(rhs.RelativePath);
				}
				if (comparison == 0)
				{
					return false;
				}
				return m_ContentDrawerSortDescending ? comparison > 0 : comparison < 0;
			});

		constexpr size_t maxContentDrawerRows = 256;
		const size_t visibleRowCount = (std::min)(rows.size(), maxContentDrawerRows);
		constexpr size_t invalidDrawerRowIndex = static_cast<size_t>(-1);
		const auto findSelectedDrawerRowIndex = [&]() -> size_t
			{
				if (m_SelectedAssetPath.empty())
				{
					return invalidDrawerRowIndex;
				}
				for (size_t rowIndex = 0; rowIndex < visibleRowCount; ++rowIndex)
				{
					if (rows[rowIndex].Entry && SamePath(m_SelectedAssetPath, rows[rowIndex].Entry->Path))
					{
						return rowIndex;
					}
				}
				return invalidDrawerRowIndex;
			};
		auto selectedDrawerRowIndex = findSelectedDrawerRowIndex();
		bool scrollSelectedDrawerRowIntoView = false;
		const auto selectDrawerRow = [&](size_t rowIndex)
			{
				if (rowIndex >= visibleRowCount || !rows[rowIndex].Entry)
				{
					return;
				}
				m_SelectedAssetPath = rows[rowIndex].Entry->Path;
				selectedDrawerRowIndex = rowIndex;
				scrollSelectedDrawerRowIntoView = true;
			};
		const auto activateDrawerRow = [&](size_t rowIndex, bool loadModelInsteadOfOpen) -> bool
			{
				if (rowIndex >= visibleRowCount || !rows[rowIndex].Entry)
				{
					return false;
				}

				const Asset::AssetFileEntry& entry = *rows[rowIndex].Entry;
				m_SelectedAssetPath = entry.Path;
				AddRecentProjectAssetPath(entry.Path, context, true);
				if (loadModelInsteadOfOpen && entry.Kind == Asset::AssetFileKind::Model && context.CanEditProjectScene && context.OnModelDrop)
				{
					context.OnModelDrop(entry.Path, AssetDropTarget::Game);
					ImGui::CloseCurrentPopup();
					return true;
				}
				if (context.OnAssetOpen)
				{
					context.OnAssetOpen(entry.Path);
					ImGui::CloseCurrentPopup();
					return true;
				}
				return false;
			};

		const bool drawerKeyboardFocused =
			ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
			(!ImGui::IsAnyItemActive() || contentDrawerFilterActive);
		if (drawerKeyboardFocused && visibleRowCount > 0)
		{
			const size_t currentIndex = selectedDrawerRowIndex == invalidDrawerRowIndex ? 0 : selectedDrawerRowIndex;
			const auto moveSelection = [&](size_t nextIndex)
				{
					selectDrawerRow(std::clamp(nextIndex, size_t{ 0 }, visibleRowCount - 1));
				};
			if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
			{
				moveSelection((std::min)(currentIndex + 1, visibleRowCount - 1));
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
			{
				moveSelection(currentIndex == 0 ? 0 : currentIndex - 1);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
			{
				moveSelection((std::min)(currentIndex + 10, visibleRowCount - 1));
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
			{
				moveSelection(currentIndex > 10 ? currentIndex - 10 : 0);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
			{
				moveSelection(0);
			}
			else if (ImGui::IsKeyPressed(ImGuiKey_End, false))
			{
				moveSelection(visibleRowCount - 1);
			}
		}

		if (openFirstResult && visibleRowCount > 0)
		{
			const size_t rowToOpen = selectedDrawerRowIndex == invalidDrawerRowIndex ? 0 : selectedDrawerRowIndex;
			if (activateDrawerRow(rowToOpen, ImGui::GetIO().KeyCtrl))
			{
				ImGui::EndPopup();
				return;
			}
		}

		if (!openFirstResult && drawerKeyboardFocused && visibleRowCount > 0 &&
			(ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
		{
			const size_t rowToOpen = selectedDrawerRowIndex == invalidDrawerRowIndex ? 0 : selectedDrawerRowIndex;
			if (activateDrawerRow(rowToOpen, ImGui::GetIO().KeyCtrl))
			{
				ImGui::EndPopup();
				return;
			}
		}

		ImGui::Text(
			"%zu matching asset(s), sorted by %s %s",
			rows.size(),
			ContentDrawerSortModeName(m_ContentDrawerSortMode),
			m_ContentDrawerSortDescending ? "desc" : "asc");
		std::error_code drawerDetailsExistsError;
		const bool selectedAssetExists = !m_SelectedAssetPath.empty() && std::filesystem::exists(m_SelectedAssetPath, drawerDetailsExistsError);
		const bool showDrawerDetails =
			m_ContentDrawerDetailsVisible &&
			selectedAssetExists;
		const float drawerDetailsWidth = showDrawerDetails
			? std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, 280.0f, 430.0f)
			: 0.0f;
		const float drawerResultsWidth = showDrawerDetails
			? (std::max)(240.0f, ImGui::GetContentRegionAvail().x - drawerDetailsWidth - ImGui::GetStyle().ItemSpacing.x)
			: 0.0f;
		if (ImGui::BeginChild("ContentDrawerResults", ImVec2(drawerResultsWidth, 0.0f), true))
		{
			if (rows.empty())
			{
				ImGui::TextDisabled(filter.empty() ? "No project assets found." : "No assets match the current search.");
			}

			for (size_t rowIndex = 0; rowIndex < visibleRowCount; ++rowIndex)
			{
				const DrawerAssetRow& row = rows[rowIndex];
				const Asset::AssetFileEntry& entry = *row.Entry;
				ImGui::PushID(static_cast<int>(rowIndex));
				const bool selected = SamePath(m_SelectedAssetPath, entry.Path);
				const bool favorite = IsProjectFavorite(entry.Path);
				const std::string label = std::format("{}{} {}##content_drawer_{}", favorite ? "* " : "", AssetKindTag(entry.Kind), row.RelativePath, rowIndex);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
				{
					m_SelectedAssetPath = entry.Path;
					AddRecentProjectAssetPath(entry.Path, context, true);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
					{
						context.OnAssetOpen(entry.Path);
						ImGui::CloseCurrentPopup();
						ImGui::PopID();
						break;
					}
				}
				if (selected && scrollSelectedDrawerRowIntoView)
				{
					ImGui::SetScrollHereY(0.5f);
				}
				if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
				{
					const std::string payloadPath = entry.Path.string();
					ImGui::SetDragDropPayload(kAssetPathPayload, payloadPath.c_str(), payloadPath.size() + 1);
					ImGui::TextUnformatted(payloadPath.c_str());
					ImGui::EndDragDropSource();
				}
				if (ImGui::BeginPopupContextItem())
				{
					if (ImGui::MenuItem(favorite ? "Remove From Favorites" : "Add To Favorites"))
					{
						ToggleProjectFavorite(entry.Path, context);
					}
					ImGui::Separator();
					if (ImGui::MenuItem("Open") && context.OnAssetOpen)
					{
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnAssetOpen(entry.Path);
					}
					if (ImGui::MenuItem("Reveal") && context.OnAssetReveal)
					{
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnAssetReveal(entry.Path);
					}
					ImGui::Separator();
					DrawProjectPathCopyMenuItems(entry.Path, snapshot->RootPath);
					if (entry.Kind == Asset::AssetFileKind::Model && ImGui::MenuItem("Load Model", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnModelDrop)))
					{
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnModelDrop(entry.Path, AssetDropTarget::Game);
					}
					if (Asset::IsSkyboxAssetPath(entry.Path) && ImGui::MenuItem("Apply Skybox", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetOpen)))
					{
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnAssetOpen(entry.Path);
					}
					if (entry.Kind == Asset::AssetFileKind::Model && ImGui::MenuItem("Reimport", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetReimportRequested)))
					{
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnAssetReimportRequested(entry.Path);
					}
					ImGui::EndPopup();
				}

				ImGui::SameLine();
				if (ImGui::SmallButton(favorite ? "Unfav" : "Fav"))
				{
					ToggleProjectFavorite(entry.Path, context);
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip(favorite ? "Remove from Project favorites." : "Add to Project favorites.");
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Open") && context.OnAssetOpen)
				{
					m_SelectedAssetPath = entry.Path;
					AddRecentProjectAssetPath(entry.Path, context, true);
					context.OnAssetOpen(entry.Path);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Reveal") && context.OnAssetReveal)
				{
					m_SelectedAssetPath = entry.Path;
					AddRecentProjectAssetPath(entry.Path, context, true);
					context.OnAssetReveal(entry.Path);
				}
				if (entry.Kind == Asset::AssetFileKind::Model)
				{
					ImGui::SameLine();
					ImGui::BeginDisabled(!context.CanEditProjectScene || !context.OnModelDrop);
					if (ImGui::SmallButton("Load"))
					{
						m_SelectedAssetPath = entry.Path;
						AddRecentProjectAssetPath(entry.Path, context, true);
						context.OnModelDrop(entry.Path, AssetDropTarget::Game);
					}
					ImGui::EndDisabled();
				}
				ImGui::PopID();
			}
			if (rows.size() > visibleRowCount)
			{
				ImGui::TextDisabled("Showing first %zu of %zu result(s). Refine the search to narrow the drawer.", visibleRowCount, rows.size());
			}
		}
		ImGui::EndChild();
		if (showDrawerDetails)
		{
			ImGui::SameLine();
			if (ImGui::BeginChild("ContentDrawerDetailsPane", ImVec2(0.0f, 0.0f), true))
			{
				DrawSelectedAssetDetails(*snapshot, context);
			}
			ImGui::EndChild();
		}
		ImGui::EndPopup();
	}

	void EditorLayer::DrawShortcutReference(EditorContext& context)
	{
		if (m_ShouldOpenShortcutReference)
		{
			ImGui::OpenPopup("Keyboard Shortcuts");
			m_ShouldOpenShortcutReference = false;
		}

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowSize(ImVec2((std::min)(viewport->Size.x * 0.72f, 760.0f), (std::min)(viewport->Size.y * 0.78f, 620.0f)), ImGuiCond_Appearing);
		bool shortcutsOpen = true;
		if (!ImGui::BeginPopupModal("Keyboard Shortcuts", &shortcutsOpen, ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		if (!shortcutsOpen || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted("Keyboard Shortcut Reference");
		ImGui::TextDisabled(
			"Project Scene editing: %s. Text inputs capture keys while active.",
			context.CanEditProjectScene ? "enabled" : "disabled");
		ImGui::SameLine();
		if (ImGui::Button("Close"))
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::Separator();

		const auto drawSection = [](const char* sectionName, const std::initializer_list<std::array<const char*, 3>>& rows)
			{
				ImGui::Spacing();
				ImGui::TextUnformatted(sectionName);
				if (ImGui::BeginTable(sectionName, 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.44f);
					ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthStretch, 0.24f);
					ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch, 0.32f);
					ImGui::TableHeadersRow();
					for (const std::array<const char*, 3>& row : rows)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextWrapped("%s", row[0]);
						ImGui::TableSetColumnIndex(1);
						ImGui::TextWrapped("%s", row[1]);
						ImGui::TableSetColumnIndex(2);
						ImGui::TextWrapped("%s", row[2]);
					}
					ImGui::EndTable();
				}
			};

		drawSection("Global", {
			{ "Open Command Palette", "Ctrl+P / Ctrl+K", "Search commands, entities, and project assets." },
			{ "Command Palette navigation", "Up/Down, Page, Home/End, Enter", "Move through executable results and run the selected result." },
			{ "Open Content Drawer", "Ctrl+Space", "Quick asset browser and drag source." },
			{ "Keyboard Shortcut Reference", "F1", "This window." },
			{ "Save Scene", "Ctrl+S", "Project Scene mode only." },
			{ "Play / Stop", "F5", "Clone edit scene into Play runtime scene, or restore edit scene." },
			{ "Pause / Resume Play", "F6", "Pauses simulation phases while editor and rendering keep updating." },
			{ "Step Play", "F10", "Advance one frame while Play mode is paused." },
			{ "Reset Play Runtime", "Toolbar / Command Palette", "Rebuilds the runtime clone from the locked edit scene snapshot." },
			{ "Undo / Redo", "Ctrl+Z / Ctrl+Y", "Editor command stack; Play mode uses runtime Transform/Material/component undo." }
		});

		drawSection("Hierarchy And Selection", {
			{ "Rename selected Entity", "F2", "Opens the rename popup." },
			{ "Delete selected Entity", "Delete", "Works with multi-select." },
			{ "Duplicate selected Entity", "Ctrl+D", "Works with multi-select." },
			{ "Frame selected Entity", "F", "Frames the Scene Camera." },
			{ "Scene marquee select", "Left-drag in Scene View", "Drag a rectangle; Ctrl/Shift keeps existing selection." }
		});

		drawSection("Scene View And Cameras", {
			{ "Move Scene Camera", "WASD / QE", "While Scene View camera input is active." },
			{ "Mouse look", "Right Mouse", "Hold to look around with the Scene Camera." },
			{ "Align Game Camera to Scene", "Ctrl+Shift+F", "Copies Scene Camera pose to the Game Camera." },
			{ "Align Scene Camera to Game", "Shift+F", "Moves Scene Camera to the Game Camera." },
			{ "Transform Gizmo", "Toolbar combo", "Translate plane handles and Scale uniform center handle support snapping." }
		});

		drawSection("Project And Console", {
			{ "Open Recent Scene", "File > Open Recent", "Also searchable in the Command Palette." },
			{ "Content Drawer result navigation", "Up/Down, Enter, Ctrl+Enter", "Move through results, open the selected asset, or load selected model." },
			{ "Console command input", "Console panel", "Use help, shortcuts, status, save, create, load, git, layout." },
			{ "Source Control refresh", "git status / scm refresh", "Console command or Source Control panel button." },
			{ "Stage / Commit", "Source Control panel", "Stage All, file Stage/Unstage, Commit Staged." },
			{ "Push / Resolve", "git push / git resolve <path>", "Push confirmation and conflict mark-resolved tools." }
		});

		ImGui::EndPopup();
	}

	void EditorLayer::DrawSceneView(EditorContext& context)
	{
		const float width = static_cast<float>((std::max)(context.ViewportWidth, 1));
		const float height = static_cast<float>((std::max)(context.ViewportHeight, 1));
		SetInitialWindowRect("Scene", 276.0f, 32.0f, width * 0.38f, height * 0.58f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.05f, 0.02f));
		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		const bool visible = ImGui::Begin("Scene", nullptr, windowFlags);
		if (!visible)
		{
			m_SceneViewport = {};
			ImGui::End();
			ImGui::PopStyleColor();
			return;
		}

		ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
		ImVec2 canvasSize = ImGui::GetContentRegionAvail();
		canvasSize.x = (std::max)(canvasSize.x, 64.0f);
		canvasSize.y = (std::max)(canvasSize.y, 64.0f);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRect(canvasPosition, ImVec2(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y), IM_COL32(120, 160, 220, 140));
		DrawSceneGrid(context, drawList, canvasPosition, canvasSize);

		ImGui::InvisibleButton("SceneCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		StoreViewportState(m_SceneViewport, canvasPosition.x, canvasPosition.y, canvasSize.x, canvasSize.y, hovered, focused);
		const bool gizmoConsumedMouse = DrawSceneGizmos(context, drawList, canvasPosition, canvasSize);
		DrawSceneMeasurement(context, drawList, canvasPosition, canvasSize);
		const ImVec2 mousePosition = ImGui::GetMousePos();
		const ImVec2 overlayMin(canvasPosition.x + 8.0f, canvasPosition.y + 8.0f);
		const bool viewCubeVisible = canvasSize.x >= 260.0f && canvasSize.y >= 170.0f;
		const float overlayWidth = (std::min)(
			(std::max)(viewCubeVisible ? canvasSize.x - 162.0f : canvasSize.x - 20.0f, 0.0f),
			820.0f);
		const ImVec2 overlayMax(canvasPosition.x + 10.0f + overlayWidth, canvasPosition.y + 116.0f);
		const ImVec2 viewCubeMin(canvasPosition.x + canvasSize.x - 152.0f, canvasPosition.y + 10.0f);
		const ImVec2 viewCubeMax(canvasPosition.x + canvasSize.x - 10.0f, canvasPosition.y + 138.0f);
		const bool mouseOverOverlay =
			PointInRect(mousePosition, overlayMin, overlayMax) ||
			(viewCubeVisible && PointInRect(mousePosition, viewCubeMin, viewCubeMax));
		const bool measureConsumedMouse = HandleSceneMeasureTool(context, canvasPosition, canvasSize, gizmoConsumedMouse || mouseOverOverlay);
		const bool orbitConsumedMouse = HandleSceneFocusOrbit(context, gizmoConsumedMouse || mouseOverOverlay || measureConsumedMouse);
		const bool altLeftMouse = m_SceneViewport.IsHovered && ImGui::GetIO().KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
		const bool marqueeConsumedMouse = HandleSceneMarqueeSelection(
			context,
			drawList,
			canvasPosition,
			canvasSize,
			gizmoConsumedMouse || mouseOverOverlay || measureConsumedMouse || orbitConsumedMouse || altLeftMouse);
		if (!gizmoConsumedMouse && !mouseOverOverlay && !measureConsumedMouse && !orbitConsumedMouse && !altLeftMouse && !marqueeConsumedMouse && ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.OnScenePick)
		{
			context.OnScenePick(
				mousePosition.x - canvasPosition.x,
				mousePosition.y - canvasPosition.y,
				canvasSize.x,
				canvasSize.y);
		}
		AcceptModelDrop(context, AssetDropTarget::Scene);

		const DirectX::XMFLOAT3 cameraPosition = context.SceneCamera.GetPosition();
		std::string overlay = "Scene";
		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (const std::string* entityName = context.ActiveScene.GetEntityName(selectedEntity))
		{
			overlay.append(" | ");
			overlay.append(*entityName);
		}
		drawList->AddText(ImVec2(canvasPosition.x + 10.0f, canvasPosition.y + 10.0f), IM_COL32(230, 235, 245, 220), overlay.c_str());
		const std::string cameraText = std::format("Camera {:.1f}, {:.1f}, {:.1f}", cameraPosition.x, cameraPosition.y, cameraPosition.z);
		drawList->AddText(ImVec2(canvasPosition.x + 10.0f, canvasPosition.y + 30.0f), IM_COL32(190, 205, 220, 210), cameraText.c_str());
		DrawSceneViewOverlay(context, canvasPosition, canvasSize);
		DrawSceneViewCube(context, canvasPosition, canvasSize);

		ImGui::End();
		ImGui::PopStyleColor();
	}

	void EditorLayer::DrawSceneViewOverlay(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize)
	{
		if (canvasSize.x < 160.0f || canvasSize.y < 90.0f)
		{
			return;
		}

		const bool viewCubeVisible = canvasSize.x >= 260.0f && canvasSize.y >= 170.0f;
		const float availableOverlayWidth = viewCubeVisible ? canvasSize.x - 162.0f : canvasSize.x - 20.0f;
		if (availableOverlayWidth < 260.0f)
		{
			return;
		}

		const ImVec2 overlayPosition(canvasPosition.x + 10.0f, canvasPosition.y + 54.0f);
		const ImVec2 overlaySize((std::min)(availableOverlayWidth, 820.0f), 52.0f);
		const ImVec2 overlayEnd(overlayPosition.x + overlaySize.x, overlayPosition.y + overlaySize.y);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(overlayPosition, overlayEnd, IM_COL32(14, 18, 24, 214), 5.0f);
		drawList->AddRect(overlayPosition, overlayEnd, IM_COL32(92, 112, 142, 160), 5.0f);

		ImGui::PushID("SceneViewOverlay");
		ImGui::SetCursorScreenPos(ImVec2(overlayPosition.x + 8.0f, overlayPosition.y + 7.0f));
		ImGui::BeginGroup();
		ImGui::Checkbox("Grid", &m_ShowSceneGrid);
		ImGui::SameLine();
		ImGui::Checkbox("Gizmos", &m_ShowSceneGizmos);
		ImGui::SameLine();
		ImGui::Checkbox("Sel", &m_ShowSelectionOutline);
		ImGui::SameLine();

		const char* currentMode = TransformGizmoModeName(m_TransformGizmoMode);
		ImGui::SetNextItemWidth(104.0f);
		if (ImGui::BeginCombo("##SceneGizmoMode", currentMode))
		{
			for (const TransformGizmoMode mode : { TransformGizmoMode::Translate, TransformGizmoMode::Rotate, TransformGizmoMode::Scale })
			{
				const bool selected = m_TransformGizmoMode == mode;
				if (ImGui::Selectable(TransformGizmoModeName(mode), selected))
				{
					m_TransformGizmoMode = mode;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		const char* currentSpace = TransformGizmoSpaceName(m_TransformGizmoSpace);
		ImGui::SetNextItemWidth(82.0f);
		if (ImGui::BeginCombo("##SceneGizmoSpace", currentSpace))
		{
			for (const TransformGizmoSpace space : { TransformGizmoSpace::World, TransformGizmoSpace::Local })
			{
				const bool selected = m_TransformGizmoSpace == space;
				if (ImGui::Selectable(TransformGizmoSpaceName(space), selected))
				{
					m_TransformGizmoSpace = space;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		const char* currentPivot = TransformGizmoPivotName(m_TransformGizmoPivot);
		ImGui::SetNextItemWidth(82.0f);
		if (ImGui::BeginCombo("##SceneGizmoPivot", currentPivot))
		{
			for (const TransformGizmoPivot pivot : { TransformGizmoPivot::Pivot, TransformGizmoPivot::Center })
			{
				const bool selected = m_TransformGizmoPivot == pivot;
				if (ImGui::Selectable(TransformGizmoPivotName(pivot), selected))
				{
					m_TransformGizmoPivot = pivot;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		ImGui::Checkbox("Orbit", &m_FocusOrbitEnabled);
		ImGui::SameLine();
		ImGui::Checkbox("Measure", &m_MeasureToolEnabled);
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_MeasureToolEnabled);
		ImGui::SetNextItemWidth(72.0f);
		if (ImGui::BeginCombo("##MeasureTarget", SceneMeasureTargetName(m_MeasureTarget)))
		{
			for (const SceneMeasureTarget target : { SceneMeasureTarget::Ground, SceneMeasureTarget::ViewPlane, SceneMeasureTarget::SelectionBounds, SceneMeasureTarget::MeshSurface })
			{
				const bool selected = m_MeasureTarget == target;
				if (ImGui::Selectable(SceneMeasureTargetName(target), selected))
				{
					m_MeasureTarget = target;
					m_MeasureToolHasStart = false;
					m_MeasureToolDragging = false;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Checkbox("Snap", &m_TransformSnappingEnabled);
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_TransformSnappingEnabled);
		ImGui::SetNextItemWidth(74.0f);
		switch (m_TransformGizmoMode)
		{
		case TransformGizmoMode::Rotate:
			if (ImGui::DragFloat("##SnapRotate", &m_RotateSnapDegrees, 1.0f, 1.0f, 180.0f, "%.0f deg"))
			{
				m_RotateSnapDegrees = std::clamp(m_RotateSnapDegrees, 1.0f, 180.0f);
			}
			break;
		case TransformGizmoMode::Scale:
			if (ImGui::DragFloat("##SnapScale", &m_ScaleSnap, 0.01f, 0.01f, 100.0f, "%.2f"))
			{
				m_ScaleSnap = std::clamp(m_ScaleSnap, 0.01f, 100.0f);
			}
			break;
		case TransformGizmoMode::Translate:
		default:
			if (ImGui::DragFloat("##SnapTranslate", &m_TranslateSnap, 0.05f, 0.01f, 1000.0f, "%.2f"))
			{
				m_TranslateSnap = std::clamp(m_TranslateSnap, 0.01f, 1000.0f);
			}
			break;
		}
		ImGui::EndDisabled();

		ImGui::SetCursorScreenPos(ImVec2(overlayPosition.x + 8.0f, overlayPosition.y + 30.0f));
		ImGui::TextUnformatted("Camera");
		ImGui::SameLine();
		float moveSpeed = context.SceneCamera.GetMoveSpeed();
		ImGui::SetNextItemWidth((std::min)(overlaySize.x - 72.0f, 280.0f));
		if (ImGui::SliderFloat("##SceneCameraSpeed", &moveSpeed, 1.0f, 1200.0f, "%.0f"))
		{
			context.SceneCamera.SetMoveSpeed(moveSpeed);
		}
		if (m_MeasureToolHasStart)
		{
			ImGui::SameLine();
			ImGui::Text("%s %.2fm", SceneMeasureTargetName(m_MeasureTarget), Distance(m_MeasureStart, m_MeasureEnd));
		}
		else if (m_MeasureToolEnabled && m_MeasureTarget == SceneMeasureTarget::MeshSurface && m_LastMeshMeasureTriangleCount > 0)
		{
			ImGui::SameLine();
			const char* accelerationLabel = m_LastMeshMeasureUsedDynamicAcceleration
				? " dyn-accel"
				: (m_LastMeshMeasureUsedAcceleration ? " accel" : "");
			ImGui::TextDisabled(
				"Mesh %s %zu/%zu%s%s %.2fms%s",
				m_LastMeshMeasureBoundsRejected ? "bounds" : (m_LastMeshMeasureHit ? "hit" : "miss"),
				m_LastMeshMeasureTrianglesTested,
				m_LastMeshMeasureTriangleCount,
				accelerationLabel,
				m_LastMeshMeasureUsedBudget ? " budget" : "",
				m_LastMeshMeasureRaycastMs,
				m_LastMeshMeasureCacheRebuilt ? " rebuild" : "");
		}
		ImGui::EndGroup();
		ImGui::PopID();
	}

	bool EditorLayer::HandleSceneMarqueeSelection(
		EditorContext& context,
		ImDrawList* drawList,
		const ImVec2& canvasPosition,
		const ImVec2& canvasSize,
		bool mouseBlockedByUi)
	{
		if (!drawList || canvasSize.x <= 1.0f || canvasSize.y <= 1.0f)
		{
			return false;
		}

		const ImGuiIO& io = ImGui::GetIO();
		const ImVec2 mousePosition = ImGui::GetMousePos();
		const ImVec2 canvasMax(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
		const bool mouseInsideCanvas = PointInRect(mousePosition, canvasPosition, canvasMax);
		if (!m_SceneMarqueeTracking &&
			!mouseBlockedByUi &&
			!io.WantTextInput &&
			m_SceneViewport.IsHovered &&
			mouseInsideCanvas &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			m_SceneMarqueeTracking = true;
			m_SceneMarqueeSelecting = false;
			m_SceneMarqueeAdditive = io.KeyCtrl || io.KeyShift;
			m_SceneMarqueeStart = { mousePosition.x, mousePosition.y };
			m_SceneMarqueeEnd = m_SceneMarqueeStart;
		}

		if (!m_SceneMarqueeTracking)
		{
			return false;
		}

		const auto clampToCanvas = [&](const ImVec2& point) noexcept
		{
			return ImVec2(
				std::clamp(point.x, canvasPosition.x, canvasMax.x),
				std::clamp(point.y, canvasPosition.y, canvasMax.y));
		};
		const ImVec2 currentMouse = clampToCanvas(mousePosition);
		m_SceneMarqueeEnd = { currentMouse.x, currentMouse.y };

		const ImVec2 start(m_SceneMarqueeStart.x, m_SceneMarqueeStart.y);
		const ImVec2 end(m_SceneMarqueeEnd.x, m_SceneMarqueeEnd.y);
		const float dragDeltaX = end.x - start.x;
		const float dragDeltaY = end.y - start.y;
		constexpr float kMarqueeThresholdPixels = 7.0f;
		const bool pastDragThreshold =
			dragDeltaX * dragDeltaX + dragDeltaY * dragDeltaY >= kMarqueeThresholdPixels * kMarqueeThresholdPixels;
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (pastDragThreshold)
			{
				m_SceneMarqueeSelecting = true;
			}
			if (m_SceneMarqueeSelecting)
			{
				const ImVec2 rectMin((std::min)(start.x, end.x), (std::min)(start.y, end.y));
				const ImVec2 rectMax((std::max)(start.x, end.x), (std::max)(start.y, end.y));
				drawList->PushClipRect(canvasPosition, canvasMax, true);
				drawList->AddRectFilled(rectMin, rectMax, IM_COL32(80, 164, 255, 42), 2.0f);
				drawList->AddRect(rectMin, rectMax, IM_COL32(118, 196, 255, 210), 2.0f, 0, 1.4f);
				const char* label = m_SceneMarqueeAdditive ? "Add Select" : "Select";
				const ImVec2 labelSize = ImGui::CalcTextSize(label);
				const ImVec2 labelMin(rectMin.x, (std::max)(canvasPosition.y, rectMin.y - labelSize.y - 6.0f));
				const ImVec2 labelMax(labelMin.x + labelSize.x + 10.0f, labelMin.y + labelSize.y + 5.0f);
				drawList->AddRectFilled(labelMin, labelMax, IM_COL32(12, 18, 28, 220), 3.0f);
				drawList->AddText(ImVec2(labelMin.x + 5.0f, labelMin.y + 2.0f), IM_COL32(218, 236, 255, 235), label);
				drawList->PopClipRect();
			}
			return m_SceneMarqueeSelecting;
		}

		const bool consumed = m_SceneMarqueeSelecting && pastDragThreshold;
		if (consumed)
		{
			struct ScreenRect
			{
				ImVec2 Min;
				ImVec2 Max;
			};

			const ScreenRect selectionRect{
				.Min = ImVec2((std::min)(start.x, end.x), (std::min)(start.y, end.y)),
				.Max = ImVec2((std::max)(start.x, end.x), (std::max)(start.y, end.y))
			};
			const auto rectsIntersect = [](const ScreenRect& lhs, const ScreenRect& rhs) noexcept
			{
				return lhs.Min.x <= rhs.Max.x && lhs.Max.x >= rhs.Min.x &&
					lhs.Min.y <= rhs.Max.y && lhs.Max.y >= rhs.Min.y;
			};
			const auto expandScreenRect = [](ScreenRect& rect, const ImVec2& point) noexcept
			{
				rect.Min.x = (std::min)(rect.Min.x, point.x);
				rect.Min.y = (std::min)(rect.Min.y, point.y);
				rect.Max.x = (std::max)(rect.Max.x, point.x);
				rect.Max.y = (std::max)(rect.Max.y, point.y);
			};
			const auto entityScreenRect = [&](EntityId entityId, ScreenRect& rect) -> bool
			{
				const TransformComponent* transform = context.ActiveScene.GetTransformComponent(entityId);
				if (!transform)
				{
					return false;
				}

				rect.Min = ImVec2(
					(std::numeric_limits<float>::max)(),
					(std::numeric_limits<float>::max)());
				rect.Max = ImVec2(
					(std::numeric_limits<float>::lowest)(),
					(std::numeric_limits<float>::lowest)());
				bool hasProjection = false;
				const auto projectPoint = [&](const DirectX::XMFLOAT3& worldPoint)
				{
					ImVec2 projected = {};
					if (ProjectWorldToSceneCanvas(context.SceneCamera, worldPoint, canvasPosition, canvasSize, projected))
					{
						expandScreenRect(rect, projected);
						hasProjection = true;
					}
				};

				if (const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(entityId))
				{
					const std::array<DirectX::XMFLOAT3, 8> localCorners = {{
						{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMin.z },
						{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMin.z },
						{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMin.z },
						{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMin.z },
						{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMax.z },
						{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMax.z },
						{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMax.z },
						{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMax.z }
					}};
					const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
					for (const DirectX::XMFLOAT3& localCorner : localCorners)
					{
						DirectX::XMFLOAT3 worldCorner = {};
						DirectX::XMStoreFloat3(
							&worldCorner,
							DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorner), worldMatrix));
						projectPoint(worldCorner);
					}
				}

				if (!hasProjection)
				{
					projectPoint(transform->WorldTransform.Translation);
				}
				if (!hasProjection)
				{
					return false;
				}

				constexpr float kMinimumSelectablePixels = 6.0f;
				if (rect.Max.x - rect.Min.x < kMinimumSelectablePixels)
				{
					const float centerX = (rect.Min.x + rect.Max.x) * 0.5f;
					rect.Min.x = centerX - kMinimumSelectablePixels * 0.5f;
					rect.Max.x = centerX + kMinimumSelectablePixels * 0.5f;
				}
				if (rect.Max.y - rect.Min.y < kMinimumSelectablePixels)
				{
					const float centerY = (rect.Min.y + rect.Max.y) * 0.5f;
					rect.Min.y = centerY - kMinimumSelectablePixels * 0.5f;
					rect.Max.y = centerY + kMinimumSelectablePixels * 0.5f;
				}
				return true;
			};

			std::vector<EntityId> marqueeHits;
			for (const SceneEntity& entity : context.ActiveScene.GetEntities())
			{
				if (!context.ActiveScene.IsEntityVisibleInScene(entity.Id) ||
					!context.ActiveScene.IsEntityPickableInScene(entity.Id))
				{
					continue;
				}

				ScreenRect rect = {};
				if (entityScreenRect(entity.Id, rect) && rectsIntersect(selectionRect, rect))
				{
					marqueeHits.push_back(entity.Id);
				}
			}

			std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
				{
					return !context.ActiveScene.ContainsEntity(entityId);
				});
			if (!m_SceneMarqueeAdditive)
			{
				m_HierarchySelection.clear();
			}
			for (EntityId entityId : marqueeHits)
			{
				if (std::ranges::find(m_HierarchySelection, entityId) == m_HierarchySelection.end())
				{
					m_HierarchySelection.push_back(entityId);
				}
			}

			if (!marqueeHits.empty())
			{
				const EntityId activeEntity = marqueeHits.back();
				context.ActiveScene.SetSelectedEntity(activeEntity);
				m_LastHierarchyClickedEntity = activeEntity;
			}
			else if (!m_SceneMarqueeAdditive)
			{
				context.ActiveScene.SetSelectedEntity(InvalidEntityId);
				m_LastHierarchyClickedEntity = InvalidEntityId;
			}
		}

		m_SceneMarqueeTracking = false;
		m_SceneMarqueeSelecting = false;
		m_SceneMarqueeAdditive = false;
		return consumed;
	}

	void EditorLayer::DrawSceneViewCube(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize)
	{
		if (canvasSize.x < 260.0f || canvasSize.y < 170.0f)
		{
			return;
		}

		const ImVec2 cubePosition(canvasPosition.x + canvasSize.x - 152.0f, canvasPosition.y + 10.0f);
		const ImVec2 cubeSize(142.0f, 128.0f);
		const ImVec2 cubeEnd(cubePosition.x + cubeSize.x, cubePosition.y + cubeSize.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(cubePosition, cubeEnd, IM_COL32(13, 17, 24, 214), 6.0f);
		drawList->AddRect(cubePosition, cubeEnd, IM_COL32(96, 118, 150, 170), 6.0f);
		drawList->AddText(ImVec2(cubePosition.x + 9.0f, cubePosition.y + 7.0f), IM_COL32(214, 226, 242, 230), "View Cube");

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		ViewFocus focus = selectedEntity != InvalidEntityId
			? ComputeEntityViewFocus(context.ActiveScene, selectedEntity)
			: ViewFocus{};
		if (selectedEntity == InvalidEntityId)
		{
			focus.Center = { 0.0f, 0.0f, 0.0f };
			focus.Radius = 8.0f;
		}

		const DirectX::XMFLOAT3 cameraPosition = context.SceneCamera.GetPosition();
		float cameraDistance = Distance(cameraPosition, focus.Center);
		const float minimumDistance = (std::max)(focus.Radius * 2.35f, 3.0f);
		if (cameraDistance < minimumDistance || !std::isfinite(cameraDistance))
		{
			cameraDistance = (std::max)(focus.Radius * 3.2f, 8.0f);
		}

		const auto moveCamera = [&](const DirectX::XMFLOAT3& offsetDirection, const DirectX::XMFLOAT3& upDirection)
			{
				const DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&focus.Center);
				const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&offsetDirection));
				DirectX::XMFLOAT3 eye = {};
				DirectX::XMStoreFloat3(&eye, DirectX::XMVectorAdd(target, DirectX::XMVectorScale(direction, cameraDistance)));
				context.SceneCamera.LookAt(eye, focus.Center, upDirection);
			};
		const auto normalizedDirection = [](const DirectX::XMFLOAT3& direction)
			{
				DirectX::XMFLOAT3 normalized = {};
				DirectX::XMStoreFloat3(&normalized, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&direction)));
				return normalized;
			};
		const auto viewUpForDirection = [](const DirectX::XMFLOAT3& direction)
			{
				const DirectX::XMVECTOR directionVector = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&direction));
				const DirectX::XMVECTOR worldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
				const float upDot = std::fabs(DirectX::XMVectorGetX(DirectX::XMVector3Dot(directionVector, worldUp)));
				if (upDot > 0.92f)
				{
					return DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f };
				}
				return DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f };
			};
		const auto moveCameraPreset = [&](const DirectX::XMFLOAT3& direction)
			{
				const DirectX::XMFLOAT3 normalized = normalizedDirection(direction);
				moveCamera(normalized, viewUpForDirection(normalized));
			};

		bool presetUiWantsMouse = false;
		ImGui::PushID("ViewCubePresets");
		ImGui::SetCursorScreenPos(ImVec2(cubeEnd.x - 58.0f, cubePosition.y + 5.0f));
		if (ImGui::SmallButton("Views"))
		{
			ImGui::OpenPopup("ViewCubePresetPopup");
		}
		presetUiWantsMouse = presetUiWantsMouse || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) || ImGui::IsItemActive();
		if (ImGui::BeginPopup("ViewCubePresetPopup"))
		{
			presetUiWantsMouse = true;
			const auto drawPreset = [&](const char* label, const DirectX::XMFLOAT3& direction)
				{
					if (ImGui::SmallButton(label))
					{
						moveCameraPreset(direction);
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("SceneCamera view: %s", label);
					}
				};
			const auto nextColumn = []()
				{
					ImGui::TableNextColumn();
				};

			ImGui::TextDisabled("Faces");
			if (ImGui::BeginTable("ViewCubeFacePresets", 3, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Front", { 0.0f, 0.0f, 1.0f });
				nextColumn(); drawPreset("Back", { 0.0f, 0.0f, -1.0f });
				nextColumn(); drawPreset("Top", { 0.0f, 1.0f, 0.0f });
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Bottom", { 0.0f, -1.0f, 0.0f });
				nextColumn(); drawPreset("Right", { 1.0f, 0.0f, 0.0f });
				nextColumn(); drawPreset("Left", { -1.0f, 0.0f, 0.0f });
				ImGui::EndTable();
			}

			ImGui::SeparatorText("Edges");
			if (ImGui::BeginTable("ViewCubeEdgePresets", 4, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Top Front", { 0.0f, 1.0f, 1.0f });
				nextColumn(); drawPreset("Top Back", { 0.0f, 1.0f, -1.0f });
				nextColumn(); drawPreset("Top Right", { 1.0f, 1.0f, 0.0f });
				nextColumn(); drawPreset("Top Left", { -1.0f, 1.0f, 0.0f });
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Bottom Front", { 0.0f, -1.0f, 1.0f });
				nextColumn(); drawPreset("Bottom Back", { 0.0f, -1.0f, -1.0f });
				nextColumn(); drawPreset("Bottom Right", { 1.0f, -1.0f, 0.0f });
				nextColumn(); drawPreset("Bottom Left", { -1.0f, -1.0f, 0.0f });
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Front Right", { 1.0f, 0.0f, 1.0f });
				nextColumn(); drawPreset("Front Left", { -1.0f, 0.0f, 1.0f });
				nextColumn(); drawPreset("Back Right", { 1.0f, 0.0f, -1.0f });
				nextColumn(); drawPreset("Back Left", { -1.0f, 0.0f, -1.0f });
				ImGui::EndTable();
			}

			ImGui::SeparatorText("Corners");
			if (ImGui::BeginTable("ViewCubeCornerPresets", 4, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Top Front Right", { 1.0f, 1.0f, 1.0f });
				nextColumn(); drawPreset("Top Front Left", { -1.0f, 1.0f, 1.0f });
				nextColumn(); drawPreset("Top Back Right", { 1.0f, 1.0f, -1.0f });
				nextColumn(); drawPreset("Top Back Left", { -1.0f, 1.0f, -1.0f });
				ImGui::TableNextRow();
				nextColumn(); drawPreset("Bottom Front Right", { 1.0f, -1.0f, 1.0f });
				nextColumn(); drawPreset("Bottom Front Left", { -1.0f, -1.0f, 1.0f });
				nextColumn(); drawPreset("Bottom Back Right", { 1.0f, -1.0f, -1.0f });
				nextColumn(); drawPreset("Bottom Back Left", { -1.0f, -1.0f, -1.0f });
				ImGui::EndTable();
			}
			ImGui::EndPopup();
		}
		presetUiWantsMouse = presetUiWantsMouse || ImGui::IsPopupOpen("ViewCubePresetPopup");
		ImGui::PopID();

		struct AxisEndpoint
		{
			const char* Label = "";
			DirectX::XMFLOAT3 Direction = {};
			DirectX::XMFLOAT3 Up = { 0.0f, 1.0f, 0.0f };
			ImU32 Color = IM_COL32_WHITE;
			ImVec2 Screen = {};
			float Depth = 0.0f;
			bool Hovered = false;
		};

		std::array<AxisEndpoint, 6> endpoints = {{
			{ "+X", { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, IM_COL32(236, 91, 91, 255) },
			{ "-X", { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, IM_COL32(158, 61, 61, 245) },
			{ "+Y", { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, IM_COL32(103, 220, 122, 255) },
			{ "-Y", { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, IM_COL32(69, 151, 84, 245) },
			{ "+Z", { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, IM_COL32(91, 145, 255, 255) },
			{ "-Z", { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, IM_COL32(61, 94, 174, 245) }
		}};

		const ImVec2 widgetCenter(cubePosition.x + cubeSize.x * 0.5f, cubePosition.y + cubeSize.y * 0.58f);
		constexpr float kAxisLength = 38.0f;
		const DirectX::XMMATRIX view = context.SceneCamera.GetViewMatrix();
		const ImVec2 mousePosition = ImGui::GetMousePos();
		for (AxisEndpoint& endpoint : endpoints)
		{
			const DirectX::XMVECTOR direction = DirectX::XMLoadFloat3(&endpoint.Direction);
			const DirectX::XMVECTOR viewDirection = DirectX::XMVector3TransformNormal(direction, view);
			const float viewX = DirectX::XMVectorGetX(viewDirection);
			const float viewY = DirectX::XMVectorGetY(viewDirection);
			endpoint.Depth = DirectX::XMVectorGetZ(viewDirection);
			endpoint.Screen = ImVec2(widgetCenter.x + viewX * kAxisLength, widgetCenter.y - viewY * kAxisLength);
			const float dx = mousePosition.x - endpoint.Screen.x;
			const float dy = mousePosition.y - endpoint.Screen.y;
			endpoint.Hovered = dx * dx + dy * dy <= 14.0f * 14.0f;
		}

		enum class ViewCubeHotspotKind : uint8_t
		{
			Face,
			Edge,
			Corner
		};

		struct ViewCubeHotspot
		{
			ViewCubeHotspotKind Kind = ViewCubeHotspotKind::Face;
			std::string Label;
			DirectX::XMFLOAT3 Direction = {};
			ImVec2 Screen = {};
			float Depth = 0.0f;
			float Radius = 5.0f;
			bool Hovered = false;
		};

		const auto axisLabel = [](int axis, int value) noexcept -> const char*
		{
			if (axis == 1)
			{
				return value > 0 ? "Top" : "Bottom";
			}
			if (axis == 2)
			{
				return value > 0 ? "Front" : "Back";
			}
			return value > 0 ? "Right" : "Left";
		};
		const auto hotspotLabel = [&](int x, int y, int z)
		{
			std::string label;
			const auto append = [&label](const char* part)
			{
				if (!label.empty())
				{
					label.push_back(' ');
				}
				label.append(part);
			};
			if (y != 0)
			{
				append(axisLabel(1, y));
			}
			if (z != 0)
			{
				append(axisLabel(2, z));
			}
			if (x != 0)
			{
				append(axisLabel(0, x));
			}
			return label;
		};
		const auto hotspotColor = [](const DirectX::XMFLOAT3& direction, bool hovered, float depth) noexcept
		{
			const int alpha = hovered ? 245 : (depth >= 0.0f ? 210 : 92);
			if (std::fabs(direction.x) >= std::fabs(direction.y) && std::fabs(direction.x) >= std::fabs(direction.z))
			{
				return direction.x >= 0.0f ? IM_COL32(236, 91, 91, alpha) : IM_COL32(158, 61, 61, alpha);
			}
			if (std::fabs(direction.y) >= std::fabs(direction.x) && std::fabs(direction.y) >= std::fabs(direction.z))
			{
				return direction.y >= 0.0f ? IM_COL32(103, 220, 122, alpha) : IM_COL32(69, 151, 84, alpha);
			}
			return direction.z >= 0.0f ? IM_COL32(91, 145, 255, alpha) : IM_COL32(61, 94, 174, alpha);
		};

		constexpr float kCubeBodyHalfSize = 23.0f;
		std::vector<ViewCubeHotspot> hotspots;
		hotspots.reserve(26);
		for (int x = -1; x <= 1; ++x)
		{
			for (int y = -1; y <= 1; ++y)
			{
				for (int z = -1; z <= 1; ++z)
				{
					const int nonZeroCount = (x != 0 ? 1 : 0) + (y != 0 ? 1 : 0) + (z != 0 ? 1 : 0);
					if (nonZeroCount == 0)
					{
						continue;
					}

					ViewCubeHotspot hotspot;
					hotspot.Kind = nonZeroCount == 1
						? ViewCubeHotspotKind::Face
						: (nonZeroCount == 2 ? ViewCubeHotspotKind::Edge : ViewCubeHotspotKind::Corner);
					hotspot.Label = hotspotLabel(x, y, z);
					hotspot.Direction = {
						static_cast<float>(x),
						static_cast<float>(y),
						static_cast<float>(z)
					};
					const DirectX::XMVECTOR viewDirection = DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&hotspot.Direction), view);
					const float viewX = DirectX::XMVectorGetX(viewDirection);
					const float viewY = DirectX::XMVectorGetY(viewDirection);
					hotspot.Depth = DirectX::XMVectorGetZ(viewDirection);
					hotspot.Screen = ImVec2(widgetCenter.x + viewX * kCubeBodyHalfSize, widgetCenter.y - viewY * kCubeBodyHalfSize);
					hotspot.Radius = hotspot.Kind == ViewCubeHotspotKind::Face
						? 8.0f
						: (hotspot.Kind == ViewCubeHotspotKind::Edge ? 6.0f : 5.0f);
					const float dx = mousePosition.x - hotspot.Screen.x;
					const float dy = mousePosition.y - hotspot.Screen.y;
					hotspot.Hovered = dx * dx + dy * dy <= hotspot.Radius * hotspot.Radius;
					hotspots.push_back(std::move(hotspot));
				}
			}
		}
		std::ranges::sort(hotspots, [](const ViewCubeHotspot& lhs, const ViewCubeHotspot& rhs)
			{
				return lhs.Depth < rhs.Depth;
			});

		const auto projectedCubePoint = [&](int x, int y, int z)
		{
			const DirectX::XMFLOAT3 direction{
				static_cast<float>(x),
				static_cast<float>(y),
				static_cast<float>(z)
			};
			const DirectX::XMVECTOR viewDirection = DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&direction), view);
			return ImVec2(
				widgetCenter.x + DirectX::XMVectorGetX(viewDirection) * kCubeBodyHalfSize,
				widgetCenter.y - DirectX::XMVectorGetY(viewDirection) * kCubeBodyHalfSize);
		};
		struct ViewCubeEdge
		{
			int Ax = 0;
			int Ay = 0;
			int Az = 0;
			int Bx = 0;
			int By = 0;
			int Bz = 0;
		};
		constexpr std::array<ViewCubeEdge, 12> cubeEdges = {{
			{ -1, -1, -1,  1, -1, -1 },
			{ -1,  1, -1,  1,  1, -1 },
			{ -1, -1,  1,  1, -1,  1 },
			{ -1,  1,  1,  1,  1,  1 },
			{ -1, -1, -1, -1,  1, -1 },
			{  1, -1, -1,  1,  1, -1 },
			{ -1, -1,  1, -1,  1,  1 },
			{  1, -1,  1,  1,  1,  1 },
			{ -1, -1, -1, -1, -1,  1 },
			{  1, -1, -1,  1, -1,  1 },
			{ -1,  1, -1, -1,  1,  1 },
			{  1,  1, -1,  1,  1,  1 }
		}};
		for (const ViewCubeEdge& edge : cubeEdges)
		{
			const DirectX::XMFLOAT3 midpoint{
				static_cast<float>(edge.Ax + edge.Bx) * 0.5f,
				static_cast<float>(edge.Ay + edge.By) * 0.5f,
				static_cast<float>(edge.Az + edge.Bz) * 0.5f
			};
			const float depth = DirectX::XMVectorGetZ(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&midpoint), view));
			drawList->AddLine(
				projectedCubePoint(edge.Ax, edge.Ay, edge.Az),
				projectedCubePoint(edge.Bx, edge.By, edge.Bz),
				depth >= 0.0f ? IM_COL32(154, 171, 196, 150) : IM_COL32(90, 101, 122, 82),
				depth >= 0.0f ? 1.6f : 1.0f);
		}

		ViewCubeHotspot* hoveredHotspot = nullptr;
		float hoveredHotspotDepth = -(std::numeric_limits<float>::max)();
		for (ViewCubeHotspot& hotspot : hotspots)
		{
			if (hotspot.Hovered && hotspot.Depth >= hoveredHotspotDepth)
			{
				hoveredHotspot = &hotspot;
				hoveredHotspotDepth = hotspot.Depth;
			}
		}
		AxisEndpoint* hoveredEndpoint = nullptr;
		float hoveredEndpointDepth = -(std::numeric_limits<float>::max)();
		for (AxisEndpoint& endpoint : endpoints)
		{
			if (endpoint.Hovered && endpoint.Depth >= hoveredEndpointDepth)
			{
				hoveredEndpoint = &endpoint;
				hoveredEndpointDepth = endpoint.Depth;
			}
		}

		for (const ViewCubeHotspot& hotspot : hotspots)
		{
			const bool hovered = hoveredHotspot == &hotspot;
			const ImU32 fillColor = hotspotColor(hotspot.Direction, hovered, hotspot.Depth);
			const ImU32 strokeColor = hovered ? IM_COL32(255, 246, 196, 245) : IM_COL32(10, 14, 20, hotspot.Depth >= 0.0f ? 200 : 135);
			switch (hotspot.Kind)
			{
			case ViewCubeHotspotKind::Face:
				drawList->AddRectFilled(
					ImVec2(hotspot.Screen.x - hotspot.Radius, hotspot.Screen.y - hotspot.Radius),
					ImVec2(hotspot.Screen.x + hotspot.Radius, hotspot.Screen.y + hotspot.Radius),
					fillColor,
					2.0f);
				drawList->AddRect(
					ImVec2(hotspot.Screen.x - hotspot.Radius, hotspot.Screen.y - hotspot.Radius),
					ImVec2(hotspot.Screen.x + hotspot.Radius, hotspot.Screen.y + hotspot.Radius),
					strokeColor,
					2.0f,
					0,
					hovered ? 1.8f : 1.0f);
				break;
			case ViewCubeHotspotKind::Edge:
			{
				const std::array<ImVec2, 4> diamond = {{
					ImVec2(hotspot.Screen.x, hotspot.Screen.y - hotspot.Radius),
					ImVec2(hotspot.Screen.x + hotspot.Radius, hotspot.Screen.y),
					ImVec2(hotspot.Screen.x, hotspot.Screen.y + hotspot.Radius),
					ImVec2(hotspot.Screen.x - hotspot.Radius, hotspot.Screen.y)
				}};
				drawList->AddConvexPolyFilled(diamond.data(), static_cast<int>(diamond.size()), fillColor);
				drawList->AddPolyline(diamond.data(), static_cast<int>(diamond.size()), strokeColor, ImDrawFlags_Closed, hovered ? 1.8f : 1.0f);
				break;
			}
			case ViewCubeHotspotKind::Corner:
				drawList->AddCircleFilled(hotspot.Screen, hotspot.Radius, fillColor, 12);
				drawList->AddCircle(hotspot.Screen, hotspot.Radius + 1.0f, strokeColor, 12, hovered ? 1.8f : 1.0f);
				break;
			}
		}
		if (hoveredEndpoint && !presetUiWantsMouse)
		{
			ImGui::SetTooltip("SceneCamera axis view: %s", hoveredEndpoint->Label);
		}
		else if (hoveredHotspot && !presetUiWantsMouse)
		{
			ImGui::SetTooltip("SceneCamera view: %s", hoveredHotspot->Label.c_str());
		}

		const auto drawEndpoint = [&](const AxisEndpoint& endpoint)
			{
				const bool frontFacing = endpoint.Depth >= 0.0f;
				const float radius = endpoint.Hovered ? 11.0f : (frontFacing ? 9.0f : 7.0f);
				const float thickness = endpoint.Hovered ? 2.4f : 1.5f;
				const ImU32 lineColor = frontFacing ? endpoint.Color : IM_COL32(106, 117, 136, 110);
				drawList->AddLine(widgetCenter, endpoint.Screen, lineColor, frontFacing ? 2.0f : 1.1f);
				drawList->AddCircleFilled(endpoint.Screen, radius, frontFacing ? endpoint.Color : IM_COL32(57, 65, 78, 230), 18);
				drawList->AddCircle(endpoint.Screen, radius + 1.5f, endpoint.Hovered ? IM_COL32(255, 246, 196, 245) : IM_COL32(11, 15, 22, 220), 18, thickness);
				const ImVec2 textSize = ImGui::CalcTextSize(endpoint.Label);
				drawList->AddText(
					ImVec2(endpoint.Screen.x - textSize.x * 0.5f, endpoint.Screen.y - textSize.y * 0.5f),
					frontFacing ? IM_COL32(255, 255, 255, 245) : IM_COL32(188, 196, 210, 210),
					endpoint.Label);
			};

		for (const AxisEndpoint& endpoint : endpoints)
		{
			if (endpoint.Depth < 0.0f)
			{
				drawEndpoint(endpoint);
			}
		}
		drawList->AddCircleFilled(widgetCenter, 7.0f, IM_COL32(230, 235, 244, 235), 18);
		drawList->AddCircle(widgetCenter, 10.0f, IM_COL32(28, 34, 44, 230), 18, 1.7f);
		for (const AxisEndpoint& endpoint : endpoints)
		{
			if (endpoint.Depth >= 0.0f)
			{
				drawEndpoint(endpoint);
			}
		}

		const bool mouseInsideCube = PointInRect(mousePosition, cubePosition, cubeEnd);
		const bool hasClickableTarget = (hoveredEndpoint != nullptr || hoveredHotspot != nullptr) && mouseInsideCube && !presetUiWantsMouse;
		if (hasClickableTarget)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}

		std::string actionLabel = "Drag orbit";
		if (hasClickableTarget)
		{
			actionLabel = hoveredEndpoint != nullptr
				? std::format("Axis {}", hoveredEndpoint->Label)
				: hoveredHotspot->Label;
		}
		else if (m_ViewCubeDragging)
		{
			actionLabel = "Orbiting";
		}
		const ImVec2 actionTextSize = ImGui::CalcTextSize(actionLabel.c_str());
		const ImVec2 actionPosition(cubePosition.x + 8.0f, cubeEnd.y - 23.0f);
		const ImVec2 actionMin(actionPosition.x - 4.0f, actionPosition.y - 2.0f);
		const ImVec2 actionMax(
			(std::min)(cubeEnd.x - 7.0f, actionPosition.x + actionTextSize.x + 8.0f),
			actionPosition.y + actionTextSize.y + 4.0f);
		drawList->AddRectFilled(
			actionMin,
			actionMax,
			hasClickableTarget ? IM_COL32(38, 47, 62, 226) : IM_COL32(19, 24, 33, 174),
			4.0f);
		drawList->AddRect(
			actionMin,
			actionMax,
			hasClickableTarget ? IM_COL32(255, 232, 154, 160) : IM_COL32(96, 118, 150, 82),
			4.0f);
		drawList->AddText(
			actionPosition,
			hasClickableTarget ? IM_COL32(255, 239, 184, 245) : IM_COL32(172, 188, 208, 225),
			actionLabel.c_str());

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseInsideCube && !presetUiWantsMouse)
		{
			if (hoveredEndpoint)
			{
				moveCamera(hoveredEndpoint->Direction, hoveredEndpoint->Up);
				m_ViewCubeDragging = false;
			}
			else if (hoveredHotspot)
			{
				moveCameraPreset(hoveredHotspot->Direction);
				m_ViewCubeDragging = false;
			}
			else
			{
				m_ViewCubeDragging = true;
			}
		}

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			m_ViewCubeDragging = false;
		}
		else if (m_ViewCubeDragging)
		{
			constexpr float kViewCubeOrbitSensitivity = 0.010f;
			OrbitCameraAroundFocus(context.SceneCamera, focus, ImGui::GetIO().MouseDelta, kViewCubeOrbitSensitivity);
		}
	}

	bool EditorLayer::HandleSceneMeasureTool(EditorContext& context, const ImVec2& canvasPosition, const ImVec2& canvasSize, bool mouseBlockedByUi)
	{
		if (!m_MeasureToolEnabled || !m_SceneViewport.IsHovered || mouseBlockedByUi || ImGui::GetIO().WantTextInput)
		{
			return false;
		}

		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyAlt)
		{
			return false;
		}

		const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		const bool leftClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		const bool leftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
		const bool rightClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Right);

		if (rightClicked)
		{
			m_MeasureToolHasStart = false;
			m_MeasureToolDragging = false;
			m_MeasureStart = {};
			m_MeasureEnd = {};
			return true;
		}

		DirectX::XMFLOAT3 measurePoint = {};
		const bool hasMeasurePoint = ProjectSceneMouseToMeasureTarget(context, ImGui::GetMousePos(), canvasPosition, canvasSize, measurePoint);
		if (leftClicked && hasMeasurePoint)
		{
			m_MeasureStart = measurePoint;
			m_MeasureEnd = measurePoint;
			m_MeasureToolHasStart = true;
			m_MeasureToolDragging = true;
			return true;
		}

		if (m_MeasureToolDragging && leftDown && hasMeasurePoint)
		{
			m_MeasureEnd = measurePoint;
			return true;
		}

		if (m_MeasureToolDragging && leftReleased)
		{
			if (hasMeasurePoint)
			{
				m_MeasureEnd = measurePoint;
			}
			m_MeasureToolDragging = false;
			return true;
		}

		return m_MeasureToolEnabled && (leftDown || m_MeasureToolDragging);
	}

	void EditorLayer::DrawSceneMeasurement(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		if (!m_MeasureToolEnabled || !m_MeasureToolHasStart || !drawList)
		{
			return;
		}

		ImVec2 startScreen = {};
		ImVec2 endScreen = {};
		if (!ProjectWorldToSceneCanvas(context.SceneCamera, m_MeasureStart, canvasPosition, canvasSize, startScreen) ||
			!ProjectWorldToSceneCanvas(context.SceneCamera, m_MeasureEnd, canvasPosition, canvasSize, endScreen))
		{
			return;
		}

		const ImVec2 canvasMax(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
		drawList->PushClipRect(canvasPosition, canvasMax, true);
		const ImU32 shadowColor = IM_COL32(5, 8, 12, 180);
		const ImU32 lineColor = IM_COL32(255, 218, 96, 245);
		const ImU32 pointColor = IM_COL32(255, 244, 180, 255);
		drawList->AddLine(startScreen, endScreen, shadowColor, 5.0f);
		drawList->AddLine(startScreen, endScreen, lineColor, 2.0f);
		drawList->AddCircleFilled(startScreen, 4.0f, pointColor, 16);
		drawList->AddCircleFilled(endScreen, 4.0f, pointColor, 16);

		const float distanceMeters = Distance(m_MeasureStart, m_MeasureEnd);
		const std::string label = std::format("{} {:.2f} m", SceneMeasureTargetName(m_MeasureTarget), distanceMeters);
		const ImVec2 midpoint((startScreen.x + endScreen.x) * 0.5f, (startScreen.y + endScreen.y) * 0.5f);
		const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
		const ImVec2 labelMin(midpoint.x - textSize.x * 0.5f - 6.0f, midpoint.y - textSize.y - 10.0f);
		const ImVec2 labelMax(labelMin.x + textSize.x + 12.0f, labelMin.y + textSize.y + 6.0f);
		drawList->AddRectFilled(labelMin, labelMax, IM_COL32(12, 16, 22, 220), 4.0f);
		drawList->AddRect(labelMin, labelMax, IM_COL32(255, 218, 96, 150), 4.0f);
		drawList->AddText(ImVec2(labelMin.x + 6.0f, labelMin.y + 3.0f), IM_COL32(255, 246, 205, 255), label.c_str());
		drawList->PopClipRect();
	}

	bool EditorLayer::HandleSceneFocusOrbit(EditorContext& context, bool mouseBlockedByUi)
	{
		if (!m_FocusOrbitEnabled || mouseBlockedByUi || !m_SceneViewport.IsHovered || ImGui::GetIO().WantTextInput)
		{
			return false;
		}

		ImGuiIO& io = ImGui::GetIO();
		const bool wantsOrbit = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
		if (!wantsOrbit)
		{
			return false;
		}

		const ImVec2 mouseDelta = io.MouseDelta;
		if (std::fabs(mouseDelta.x) <= 0.001f && std::fabs(mouseDelta.y) <= 0.001f)
		{
			return true;
		}

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		ViewFocus focus = selectedEntity != InvalidEntityId
			? ComputeEntityViewFocus(context.ActiveScene, selectedEntity)
			: ViewFocus{};
		if (selectedEntity == InvalidEntityId)
		{
			focus.Center = { 0.0f, 0.0f, 0.0f };
			focus.Radius = 8.0f;
		}

		constexpr float kOrbitSensitivity = 0.008f;
		OrbitCameraAroundFocus(context.SceneCamera, focus, mouseDelta, kOrbitSensitivity);
		return true;
	}

	void EditorLayer::DrawSceneGrid(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		if (!m_ShowSceneGrid || !drawList || canvasSize.x < 1.0f || canvasSize.y < 1.0f)
		{
			return;
		}

		const ImVec2 canvasMax(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
		drawList->PushClipRect(canvasPosition, canvasMax, true);

		constexpr int kGridHalfExtent = 50;
		constexpr int kMajorLineEvery = 5;
		constexpr float kGridStep = 1.0f;
		const ImU32 minorColor = IM_COL32(94, 111, 133, 42);
		const ImU32 majorColor = IM_COL32(118, 139, 166, 70);
		const ImU32 xAxisColor = IM_COL32(230, 94, 94, 130);
		const ImU32 zAxisColor = IM_COL32(88, 142, 240, 130);

		for (int index = -kGridHalfExtent; index <= kGridHalfExtent; ++index)
		{
			const float coordinate = static_cast<float>(index) * kGridStep;
			const bool major = index % kMajorLineEvery == 0;
			const float thickness = index == 0 ? 1.6f : (major ? 1.15f : 0.85f);
			ImVec2 start = {};
			ImVec2 end = {};

			if (ProjectWorldToSceneCanvas(context.SceneCamera, { coordinate, 0.0f, -static_cast<float>(kGridHalfExtent) }, canvasPosition, canvasSize, start) &&
				ProjectWorldToSceneCanvas(context.SceneCamera, { coordinate, 0.0f, static_cast<float>(kGridHalfExtent) }, canvasPosition, canvasSize, end))
			{
				const ImU32 color = index == 0 ? xAxisColor : (major ? majorColor : minorColor);
				drawList->AddLine(start, end, color, thickness);
			}

			if (ProjectWorldToSceneCanvas(context.SceneCamera, { -static_cast<float>(kGridHalfExtent), 0.0f, coordinate }, canvasPosition, canvasSize, start) &&
				ProjectWorldToSceneCanvas(context.SceneCamera, { static_cast<float>(kGridHalfExtent), 0.0f, coordinate }, canvasPosition, canvasSize, end))
			{
				const ImU32 color = index == 0 ? zAxisColor : (major ? majorColor : minorColor);
				drawList->AddLine(start, end, color, thickness);
			}
		}

		drawList->PopClipRect();
	}

	bool EditorLayer::DrawSceneGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize)
	{
		if (!m_ShowSceneGizmos || !drawList || canvasSize.x < 1.0f || canvasSize.y < 1.0f)
		{
			return false;
		}

		const ImVec2 canvasMax(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
		drawList->PushClipRect(canvasPosition, canvasMax, true);
		DrawGameCameraFrustumGizmo(context, drawList, canvasPosition, canvasSize);
		DrawColliderGizmos(context, drawList, canvasPosition, canvasSize);
		DrawSelectionBoundsGizmo(context, drawList, canvasPosition, canvasSize);
		const bool consumedMouse = DrawTransformGizmo(context, drawList, canvasPosition, canvasSize);
		drawList->PopClipRect();
		return consumedMouse;
	}

	bool EditorLayer::DrawTransformGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize)
	{
		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity);
		if (selectedEntity == InvalidEntityId || !transform)
		{
			m_TransformGizmoActiveEntity = InvalidEntityId;
			m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
			m_TransformGizmoActivePlane = TransformGizmoPlane::None;
			m_TransformGizmoUniformScaleActive = false;
			return false;
		}

		DirectX::XMFLOAT3 originWorld = transform->WorldTransform.Translation;
		if (m_TransformGizmoPivot == TransformGizmoPivot::Center)
		{
			originWorld = ComputeEntityViewFocus(context.ActiveScene, selectedEntity).Center;
		}
		ImVec2 originScreen = {};
		if (!ProjectWorldToSceneCanvas(context.SceneCamera, originWorld, canvasPosition, canvasSize, originScreen))
		{
			m_TransformGizmoActiveEntity = InvalidEntityId;
			m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
			m_TransformGizmoActivePlane = TransformGizmoPlane::None;
			m_TransformGizmoUniformScaleActive = false;
			return false;
		}

		const DirectX::XMFLOAT3 cameraPosition = context.SceneCamera.GetPosition();
		const float distanceToCamera = std::sqrt(
			(originWorld.x - cameraPosition.x) * (originWorld.x - cameraPosition.x) +
			(originWorld.y - cameraPosition.y) * (originWorld.y - cameraPosition.y) +
			(originWorld.z - cameraPosition.z) * (originWorld.z - cameraPosition.z));
		const float gizmoScale = std::clamp(distanceToCamera * 0.18f, 0.65f, 12.0f);

		struct AxisProjection
		{
			TransformGizmoAxis Axis = TransformGizmoAxis::None;
			DirectX::XMFLOAT3 Direction = {};
			ImVec2 EndScreen = {};
			bool Projected = false;
		};

		std::array<AxisProjection, 3> axes = {{
			{ TransformGizmoAxis::X, TransformGizmoAxisDirection(TransformGizmoAxis::X, m_TransformGizmoSpace, transform->WorldTransform) },
			{ TransformGizmoAxis::Y, TransformGizmoAxisDirection(TransformGizmoAxis::Y, m_TransformGizmoSpace, transform->WorldTransform) },
			{ TransformGizmoAxis::Z, TransformGizmoAxisDirection(TransformGizmoAxis::Z, m_TransformGizmoSpace, transform->WorldTransform) }
		}};

		for (AxisProjection& axis : axes)
		{
			const DirectX::XMFLOAT3 endpoint = {
				originWorld.x + axis.Direction.x * gizmoScale,
				originWorld.y + axis.Direction.y * gizmoScale,
				originWorld.z + axis.Direction.z * gizmoScale
			};
			axis.Projected = ProjectWorldToSceneCanvas(context.SceneCamera, endpoint, canvasPosition, canvasSize, axis.EndScreen);
		}

		const auto axisProjection = [&axes](TransformGizmoAxis axis) -> const AxisProjection*
		{
			const auto it = std::ranges::find_if(axes, [axis](const AxisProjection& projection)
				{
					return projection.Axis == axis;
				});
			return it == axes.end() ? nullptr : &(*it);
		};

		struct PlaneProjection
		{
			TransformGizmoPlane Plane = TransformGizmoPlane::None;
			std::array<ImVec2, 4> Corners = {};
			ImVec2 Center = {};
			bool Projected = false;
		};

		std::array<PlaneProjection, 3> planes = {{
			{ TransformGizmoPlane::XY },
			{ TransformGizmoPlane::XZ },
			{ TransformGizmoPlane::YZ }
		}};
		if (m_TransformGizmoMode == TransformGizmoMode::Translate)
		{
			const auto projectPlanePoint = [&](const DirectX::XMFLOAT3& axisA, float amountA, const DirectX::XMFLOAT3& axisB, float amountB, ImVec2& screenPosition)
			{
				const DirectX::XMFLOAT3 world = {
					originWorld.x + axisA.x * amountA + axisB.x * amountB,
					originWorld.y + axisA.y * amountA + axisB.y * amountB,
					originWorld.z + axisA.z * amountA + axisB.z * amountB
				};
				return ProjectWorldToSceneCanvas(context.SceneCamera, world, canvasPosition, canvasSize, screenPosition);
			};

			const float planeOffset = gizmoScale * 0.22f;
			const float planeSize = gizmoScale * 0.24f;
			for (PlaneProjection& plane : planes)
			{
				const auto [axisA, axisB] = TransformGizmoPlaneAxes(plane.Plane);
				const AxisProjection* projectionA = axisProjection(axisA);
				const AxisProjection* projectionB = axisProjection(axisB);
				if (!projectionA || !projectionB || !projectionA->Projected || !projectionB->Projected)
				{
					continue;
				}

				plane.Projected =
					projectPlanePoint(projectionA->Direction, planeOffset, projectionB->Direction, planeOffset, plane.Corners[0]) &&
					projectPlanePoint(projectionA->Direction, planeOffset + planeSize, projectionB->Direction, planeOffset, plane.Corners[1]) &&
					projectPlanePoint(projectionA->Direction, planeOffset + planeSize, projectionB->Direction, planeOffset + planeSize, plane.Corners[2]) &&
					projectPlanePoint(projectionA->Direction, planeOffset, projectionB->Direction, planeOffset + planeSize, plane.Corners[3]);
				plane.Center = ImVec2(
					(plane.Corners[0].x + plane.Corners[1].x + plane.Corners[2].x + plane.Corners[3].x) * 0.25f,
					(plane.Corners[0].y + plane.Corners[1].y + plane.Corners[2].y + plane.Corners[3].y) * 0.25f);
			}
		}

		struct RingProjection
		{
			TransformGizmoAxis Axis = TransformGizmoAxis::None;
			std::array<ImVec2, 65> Points = {};
			std::array<bool, 65> Projected = {};
		};

		std::array<RingProjection, 3> rings = {{
			{ TransformGizmoAxis::X },
			{ TransformGizmoAxis::Y },
			{ TransformGizmoAxis::Z }
		}};
		if (m_TransformGizmoMode == TransformGizmoMode::Rotate)
		{
			constexpr size_t kRingSegments = 64;
			const float ringRadius = gizmoScale * 0.72f;
			const DirectX::XMVECTOR originVector = DirectX::XMLoadFloat3(&originWorld);
			const DirectX::XMVECTOR worldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
			const DirectX::XMVECTOR fallbackRight = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
			for (RingProjection& ring : rings)
			{
				const AxisProjection* projection = axisProjection(ring.Axis);
				if (!projection)
				{
					continue;
				}

				const DirectX::XMVECTOR axisVector = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&projection->Direction));
				const float upDot = std::fabs(DirectX::XMVectorGetX(DirectX::XMVector3Dot(axisVector, worldUp)));
				const DirectX::XMVECTOR reference = upDot > 0.92f ? fallbackRight : worldUp;
				const DirectX::XMVECTOR basisU = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(reference, axisVector));
				const DirectX::XMVECTOR basisV = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(axisVector, basisU));
				for (size_t segment = 0; segment <= kRingSegments; ++segment)
				{
					const float angle = (static_cast<float>(segment) / static_cast<float>(kRingSegments)) * DirectX::XM_2PI;
					const DirectX::XMVECTOR offset =
						DirectX::XMVectorScale(basisU, std::cos(angle) * ringRadius) +
						DirectX::XMVectorScale(basisV, std::sin(angle) * ringRadius);
					DirectX::XMFLOAT3 worldPoint = {};
					DirectX::XMStoreFloat3(&worldPoint, DirectX::XMVectorAdd(originVector, offset));
					ring.Projected[segment] = ProjectWorldToSceneCanvas(context.SceneCamera, worldPoint, canvasPosition, canvasSize, ring.Points[segment]);
				}
			}
		}

		const ImVec2 mousePosition = ImGui::GetMousePos();
		TransformGizmoAxis hoveredAxis = TransformGizmoAxis::None;
		TransformGizmoPlane hoveredPlane = TransformGizmoPlane::None;
		bool hoveredUniformScale = false;
		float hoveredDistance = 12.0f;
		const auto pointInTriangle = [](const ImVec2& point, const ImVec2& a, const ImVec2& b, const ImVec2& c) noexcept
		{
			const auto sign = [](const ImVec2& p1, const ImVec2& p2, const ImVec2& p3) noexcept
			{
				return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
			};
			const float d1 = sign(point, a, b);
			const float d2 = sign(point, b, c);
			const float d3 = sign(point, c, a);
			const bool hasNegative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
			const bool hasPositive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
			return !(hasNegative && hasPositive);
		};
		const auto pointInPlane = [&pointInTriangle](const ImVec2& point, const PlaneProjection& plane) noexcept
		{
			return plane.Projected &&
				(pointInTriangle(point, plane.Corners[0], plane.Corners[1], plane.Corners[2]) ||
					pointInTriangle(point, plane.Corners[0], plane.Corners[2], plane.Corners[3]));
		};
		const auto drawGizmoBadge = [&](const ImVec2& anchor, const std::string& text, ImU32 accentColor)
		{
			const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
			ImVec2 badgeMin(anchor.x, anchor.y);
			const float maxX = canvasPosition.x + canvasSize.x - textSize.x - 18.0f;
			const float maxY = canvasPosition.y + canvasSize.y - textSize.y - 14.0f;
			if (maxX > canvasPosition.x + 6.0f)
			{
				badgeMin.x = std::clamp(badgeMin.x, canvasPosition.x + 6.0f, maxX);
			}
			if (maxY > canvasPosition.y + 6.0f)
			{
				badgeMin.y = std::clamp(badgeMin.y, canvasPosition.y + 6.0f, maxY);
			}
			const ImVec2 badgeMax(badgeMin.x + textSize.x + 12.0f, badgeMin.y + textSize.y + 8.0f);
			drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(12, 16, 22, 228), 4.0f);
			drawList->AddRect(badgeMin, badgeMax, accentColor, 4.0f);
			drawList->AddText(ImVec2(badgeMin.x + 6.0f, badgeMin.y + 4.0f), IM_COL32(245, 245, 245, 242), text.c_str());
		};
		const auto formatScaleTriplet = [](const DirectX::XMFLOAT3& scale)
		{
			return std::format("X {:.2f}  Y {:.2f}  Z {:.2f}", scale.x, scale.y, scale.z);
		};
		const auto scaleComponent = [](const DirectX::XMFLOAT3& scale, TransformGizmoAxis axis) noexcept
		{
			switch (axis)
			{
			case TransformGizmoAxis::X:
				return scale.x;
			case TransformGizmoAxis::Y:
				return scale.y;
			case TransformGizmoAxis::Z:
				return scale.z;
			default:
				return 0.0f;
			}
		};

		if (m_TransformGizmoActiveAxis == TransformGizmoAxis::None &&
			m_TransformGizmoActivePlane == TransformGizmoPlane::None &&
			!m_TransformGizmoUniformScaleActive &&
			m_SceneViewport.IsHovered &&
			!ImGui::GetIO().WantTextInput)
		{
			if (m_TransformGizmoMode == TransformGizmoMode::Scale)
			{
				const float dx = mousePosition.x - originScreen.x;
				const float dy = mousePosition.y - originScreen.y;
				hoveredUniformScale = dx * dx + dy * dy <= 12.0f * 12.0f;
			}
			if (m_TransformGizmoMode == TransformGizmoMode::Rotate)
			{
				float hoveredRingDistance = 9.0f;
				for (const RingProjection& ring : rings)
				{
					for (size_t segment = 0; segment + 1 < ring.Points.size(); ++segment)
					{
						if (!ring.Projected[segment] || !ring.Projected[segment + 1])
						{
							continue;
						}

						const float distance = DistancePointToSegment(mousePosition, ring.Points[segment], ring.Points[segment + 1]);
						if (distance < hoveredRingDistance)
						{
							hoveredRingDistance = distance;
							hoveredAxis = ring.Axis;
							hoveredDistance = distance;
						}
					}
				}
			}
			for (const PlaneProjection& plane : planes)
			{
				if (!hoveredUniformScale && pointInPlane(mousePosition, plane))
				{
					hoveredPlane = plane.Plane;
					break;
				}
			}
			for (const AxisProjection& axis : axes)
			{
				if (!axis.Projected)
				{
					continue;
				}

				const float distance = DistancePointToSegment(mousePosition, originScreen, axis.EndScreen);
				if (distance < hoveredDistance)
				{
					hoveredDistance = distance;
					hoveredAxis = axis.Axis;
				}
			}
		}
		const bool hoveredAnyGizmo = hoveredAxis != TransformGizmoAxis::None || hoveredPlane != TransformGizmoPlane::None || hoveredUniformScale;
		if (hoveredAnyGizmo && m_SceneViewport.IsHovered && !ImGui::GetIO().WantTextInput)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}

		if (m_TransformGizmoActiveAxis != TransformGizmoAxis::None || m_TransformGizmoActivePlane != TransformGizmoPlane::None || m_TransformGizmoUniformScaleActive)
		{
			if (m_TransformGizmoActiveEntity != selectedEntity || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				const Math::Transform afterTransform = transform->LocalTransform;
				if (!TransformNearlyEqual(m_TransformGizmoStartTransform, afterTransform) && context.OnTransformEditCommitted)
				{
					context.OnTransformEditCommitted(selectedEntity, m_TransformGizmoStartTransform, afterTransform);
				}
				m_TransformGizmoActiveEntity = InvalidEntityId;
				m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
				m_TransformGizmoActivePlane = TransformGizmoPlane::None;
				m_TransformGizmoUniformScaleActive = false;
			}
			else
			{
				const ImVec2 delta = {
					mousePosition.x - m_TransformGizmoStartMouse.x,
					mousePosition.y - m_TransformGizmoStartMouse.y
				};
				Math::Transform nextTransform = m_TransformGizmoStartTransform;
				if (m_TransformGizmoUniformScaleActive)
				{
					const float scalarPixels =
						delta.x * m_TransformGizmoStartAxisScreen.x +
						delta.y * m_TransformGizmoStartAxisScreen.y;
					const float factor = (std::max)(0.01f, 1.0f + scalarPixels / 160.0f);
					nextTransform.Scale.x = (std::max)(0.01f, m_TransformGizmoStartTransform.Scale.x * factor);
					nextTransform.Scale.y = (std::max)(0.01f, m_TransformGizmoStartTransform.Scale.y * factor);
					nextTransform.Scale.z = (std::max)(0.01f, m_TransformGizmoStartTransform.Scale.z * factor);
					if (m_TransformSnappingEnabled)
					{
						nextTransform.Scale.x = (std::max)(0.01f, SnapValue(nextTransform.Scale.x, m_ScaleSnap));
						nextTransform.Scale.y = (std::max)(0.01f, SnapValue(nextTransform.Scale.y, m_ScaleSnap));
						nextTransform.Scale.z = (std::max)(0.01f, SnapValue(nextTransform.Scale.z, m_ScaleSnap));
					}
				}
				else if (m_TransformGizmoActivePlane != TransformGizmoPlane::None)
				{
					const auto [axisA, axisB] = TransformGizmoPlaneAxes(m_TransformGizmoActivePlane);
					const DirectX::XMFLOAT3 axisDirectionA = TransformGizmoAxisDirection(axisA, m_TransformGizmoSpace, m_TransformGizmoStartWorldTransform);
					const DirectX::XMFLOAT3 axisDirectionB = TransformGizmoAxisDirection(axisB, m_TransformGizmoSpace, m_TransformGizmoStartWorldTransform);
					float worldDeltaA =
						(delta.x * m_TransformGizmoStartAxisScreen.x + delta.y * m_TransformGizmoStartAxisScreen.y) /
						90.0f * m_TransformGizmoStartScale;
					float worldDeltaB =
						(delta.x * m_TransformGizmoStartPlaneSecondScreen.x + delta.y * m_TransformGizmoStartPlaneSecondScreen.y) /
						90.0f * m_TransformGizmoStartScale;
					if (m_TransformSnappingEnabled)
					{
						worldDeltaA = SnapValue(worldDeltaA, m_TranslateSnap);
						worldDeltaB = SnapValue(worldDeltaB, m_TranslateSnap);
					}
					nextTransform.Translation.x += axisDirectionA.x * worldDeltaA + axisDirectionB.x * worldDeltaB;
					nextTransform.Translation.y += axisDirectionA.y * worldDeltaA + axisDirectionB.y * worldDeltaB;
					nextTransform.Translation.z += axisDirectionA.z * worldDeltaA + axisDirectionB.z * worldDeltaB;
				}
				else
				{
					const float scalarPixels =
						delta.x * m_TransformGizmoStartAxisScreen.x +
						delta.y * m_TransformGizmoStartAxisScreen.y;
					const TransformGizmoAxis activeAxis = m_TransformGizmoActiveAxis;
					const DirectX::XMFLOAT3 axisDirection = TransformGizmoAxisDirection(activeAxis, m_TransformGizmoSpace, m_TransformGizmoStartWorldTransform);

					switch (m_TransformGizmoMode)
					{
					case TransformGizmoMode::Rotate:
					{
						float angleRadians = scalarPixels * 0.012f;
						if (m_TransformSnappingEnabled)
						{
							angleRadians = SnapValue(angleRadians, DirectX::XMConvertToRadians(m_RotateSnapDegrees));
						}
						const DirectX::XMVECTOR axisVector = DirectX::XMLoadFloat3(&axisDirection);
						const DirectX::XMVECTOR deltaRotation = DirectX::XMQuaternionRotationAxis(axisVector, angleRadians);
						const DirectX::XMVECTOR startRotation = DirectX::XMLoadFloat4(&m_TransformGizmoStartTransform.Rotation);
						DirectX::XMStoreFloat4(&nextTransform.Rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionMultiply(deltaRotation, startRotation)));
						break;
					}
					case TransformGizmoMode::Scale:
					{
						const size_t axisIndex = TransformGizmoAxisIndex(activeAxis);
						std::array<float*, 3> scaleValues = { &nextTransform.Scale.x, &nextTransform.Scale.y, &nextTransform.Scale.z };
						const float startValue = *scaleValues[axisIndex];
						const float scaleUnit = (std::max)(std::fabs(startValue), 0.25f);
						float nextValue = startValue + (scalarPixels / 120.0f) * scaleUnit;
						if (m_TransformSnappingEnabled)
						{
							nextValue = SnapValue(nextValue, m_ScaleSnap);
						}
						*scaleValues[axisIndex] = (std::max)(0.01f, nextValue);
						break;
					}
					case TransformGizmoMode::Translate:
					default:
					{
						float worldDelta = (scalarPixels / 90.0f) * m_TransformGizmoStartScale;
						if (m_TransformSnappingEnabled)
						{
							worldDelta = SnapValue(worldDelta, m_TranslateSnap);
						}
						nextTransform.Translation.x += axisDirection.x * worldDelta;
						nextTransform.Translation.y += axisDirection.y * worldDelta;
						nextTransform.Translation.z += axisDirection.z * worldDelta;
						break;
					}
					}
				}

				nextTransform.Rotation = Math::NormalizeQuaternionOrIdentity(nextTransform.Rotation);
				transform->LocalTransform = nextTransform;
				transform->UpdateWorld();
				if (context.OnPhysicsActorDirty)
				{
					context.OnPhysicsActorDirty(selectedEntity);
				}
			}
		}
		else if (hoveredUniformScale && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			constexpr float kUniformScaleAxis = 0.70710678f;
			m_TransformGizmoActiveEntity = selectedEntity;
			m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
			m_TransformGizmoActivePlane = TransformGizmoPlane::None;
			m_TransformGizmoUniformScaleActive = true;
			m_TransformGizmoStartTransform = transform->LocalTransform;
			m_TransformGizmoStartWorldTransform = transform->WorldTransform;
			m_TransformGizmoStartMouse = { mousePosition.x, mousePosition.y };
			m_TransformGizmoStartAxisScreen = { kUniformScaleAxis, -kUniformScaleAxis };
			m_TransformGizmoStartScale = gizmoScale;
		}
		else if (hoveredPlane != TransformGizmoPlane::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const auto [axisA, axisB] = TransformGizmoPlaneAxes(hoveredPlane);
			const AxisProjection* projectionA = axisProjection(axisA);
			const AxisProjection* projectionB = axisProjection(axisB);
			if (projectionA && projectionB && projectionA->Projected && projectionB->Projected)
			{
				const auto normalizedScreenAxis = [originScreen](const AxisProjection& axis)
				{
					ImVec2 screenAxis = {
						axis.EndScreen.x - originScreen.x,
						axis.EndScreen.y - originScreen.y
					};
					const float axisLength = std::sqrt(screenAxis.x * screenAxis.x + screenAxis.y * screenAxis.y);
					if (axisLength > 0.0001f)
					{
						screenAxis.x /= axisLength;
						screenAxis.y /= axisLength;
					}
					else
					{
						screenAxis = { 1.0f, 0.0f };
					}
					return screenAxis;
				};

				const ImVec2 screenAxisA = normalizedScreenAxis(*projectionA);
				const ImVec2 screenAxisB = normalizedScreenAxis(*projectionB);
				m_TransformGizmoActiveEntity = selectedEntity;
				m_TransformGizmoActiveAxis = TransformGizmoAxis::None;
				m_TransformGizmoActivePlane = hoveredPlane;
				m_TransformGizmoUniformScaleActive = false;
				m_TransformGizmoStartTransform = transform->LocalTransform;
				m_TransformGizmoStartWorldTransform = transform->WorldTransform;
				m_TransformGizmoStartMouse = { mousePosition.x, mousePosition.y };
				m_TransformGizmoStartAxisScreen = { screenAxisA.x, screenAxisA.y };
				m_TransformGizmoStartPlaneSecondScreen = { screenAxisB.x, screenAxisB.y };
				m_TransformGizmoStartScale = gizmoScale;
			}
		}
		else if (hoveredAxis != TransformGizmoAxis::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			for (const AxisProjection& axis : axes)
			{
				if (axis.Axis != hoveredAxis || !axis.Projected)
				{
					continue;
				}

				ImVec2 screenAxis = {
					axis.EndScreen.x - originScreen.x,
					axis.EndScreen.y - originScreen.y
				};
				const float axisLength = std::sqrt(screenAxis.x * screenAxis.x + screenAxis.y * screenAxis.y);
				if (axisLength > 0.0001f)
				{
					screenAxis.x /= axisLength;
					screenAxis.y /= axisLength;
				}
				else
				{
					screenAxis = { 1.0f, 0.0f };
				}

				m_TransformGizmoActiveEntity = selectedEntity;
				m_TransformGizmoActiveAxis = hoveredAxis;
				m_TransformGizmoActivePlane = TransformGizmoPlane::None;
				m_TransformGizmoUniformScaleActive = false;
				m_TransformGizmoStartTransform = transform->LocalTransform;
				m_TransformGizmoStartWorldTransform = transform->WorldTransform;
				m_TransformGizmoStartMouse = { mousePosition.x, mousePosition.y };
				m_TransformGizmoStartAxisScreen = { screenAxis.x, screenAxis.y };
				m_TransformGizmoStartScale = gizmoScale;
				break;
			}
		}

		drawList->AddCircleFilled(originScreen, 4.5f, IM_COL32(245, 245, 245, 245), 14);
		drawList->AddCircle(originScreen, 7.0f, IM_COL32(25, 28, 34, 230), 14, 1.4f);
		if (m_TransformGizmoMode == TransformGizmoMode::Rotate)
		{
			for (const RingProjection& ring : rings)
			{
				const bool highlighted =
					ring.Axis == hoveredAxis ||
					(ring.Axis == m_TransformGizmoActiveAxis && selectedEntity == m_TransformGizmoActiveEntity);
				const ImU32 color = TransformGizmoAxisColor(ring.Axis, highlighted);
				const float thickness = highlighted ? 3.0f : 1.65f;
				for (size_t segment = 0; segment + 1 < ring.Points.size(); ++segment)
				{
					if (!ring.Projected[segment] || !ring.Projected[segment + 1])
					{
						continue;
					}
					drawList->AddLine(ring.Points[segment], ring.Points[segment + 1], color, thickness);
				}
				if (highlighted)
				{
					drawList->AddText(
						ImVec2(originScreen.x + 14.0f, originScreen.y + 26.0f),
						color,
						std::format("Rotate {}", TransformGizmoAxisName(ring.Axis)).c_str());
				}
			}
		}
		if (m_TransformGizmoMode == TransformGizmoMode::Scale)
		{
			const bool highlighted = hoveredUniformScale || (m_TransformGizmoUniformScaleActive && selectedEntity == m_TransformGizmoActiveEntity);
			const float halfSize = highlighted ? 7.0f : 5.6f;
			drawList->AddRectFilled(
				ImVec2(originScreen.x - halfSize, originScreen.y - halfSize),
				ImVec2(originScreen.x + halfSize, originScreen.y + halfSize),
				highlighted ? IM_COL32(245, 245, 245, 250) : IM_COL32(220, 226, 236, 230),
				2.0f);
			drawList->AddRect(
				ImVec2(originScreen.x - halfSize - 1.5f, originScreen.y - halfSize - 1.5f),
				ImVec2(originScreen.x + halfSize + 1.5f, originScreen.y + halfSize + 1.5f),
				highlighted ? IM_COL32(255, 246, 196, 245) : IM_COL32(25, 28, 34, 230),
				2.0f,
				0,
				highlighted ? 2.0f : 1.2f);
			const ImU32 markColor = highlighted ? IM_COL32(24, 29, 38, 245) : IM_COL32(58, 68, 84, 210);
			drawList->AddLine(
				ImVec2(originScreen.x - halfSize * 0.52f, originScreen.y),
				ImVec2(originScreen.x + halfSize * 0.52f, originScreen.y),
				markColor,
				1.4f);
			drawList->AddLine(
				ImVec2(originScreen.x, originScreen.y - halfSize * 0.52f),
				ImVec2(originScreen.x, originScreen.y + halfSize * 0.52f),
				markColor,
				1.4f);
			if (highlighted)
			{
				drawList->AddText(ImVec2(originScreen.x + 10.0f, originScreen.y - 18.0f), IM_COL32(245, 245, 245, 235), "Uniform");
			}
		}
		if (m_TransformGizmoMode == TransformGizmoMode::Translate)
		{
			const auto planeColor = [](TransformGizmoPlane plane, bool highlighted) noexcept
			{
				const int alpha = highlighted ? 94 : 48;
				switch (plane)
				{
				case TransformGizmoPlane::XY:
					return IM_COL32(255, 215, 88, alpha);
				case TransformGizmoPlane::XZ:
					return IM_COL32(255, 96, 210, alpha);
				case TransformGizmoPlane::YZ:
					return IM_COL32(92, 226, 246, alpha);
				default:
					return IM_COL32(255, 255, 255, alpha);
				}
			};
			const auto planeStrokeColor = [](TransformGizmoPlane plane, bool highlighted) noexcept
			{
				const int alpha = highlighted ? 235 : 150;
				switch (plane)
				{
				case TransformGizmoPlane::XY:
					return IM_COL32(255, 215, 88, alpha);
				case TransformGizmoPlane::XZ:
					return IM_COL32(255, 96, 210, alpha);
				case TransformGizmoPlane::YZ:
					return IM_COL32(92, 226, 246, alpha);
				default:
					return IM_COL32(255, 255, 255, alpha);
				}
			};

			for (const PlaneProjection& plane : planes)
			{
				if (!plane.Projected)
				{
					continue;
				}

				const bool highlighted =
					plane.Plane == hoveredPlane ||
					(plane.Plane == m_TransformGizmoActivePlane && selectedEntity == m_TransformGizmoActiveEntity);
				drawList->AddConvexPolyFilled(plane.Corners.data(), static_cast<int>(plane.Corners.size()), planeColor(plane.Plane, highlighted));
				drawList->AddPolyline(
					plane.Corners.data(),
					static_cast<int>(plane.Corners.size()),
					planeStrokeColor(plane.Plane, highlighted),
					ImDrawFlags_Closed,
					highlighted ? 2.0f : 1.2f);
				if (highlighted)
				{
					drawList->AddText(
						ImVec2(plane.Center.x + 4.0f, plane.Center.y - 7.0f),
						planeStrokeColor(plane.Plane, true),
						TransformGizmoPlaneName(plane.Plane));
				}
			}
		}
		for (const AxisProjection& axis : axes)
		{
			if (!axis.Projected)
			{
				continue;
			}

			const bool highlighted =
				axis.Axis == hoveredAxis ||
				(axis.Axis == m_TransformGizmoActiveAxis && selectedEntity == m_TransformGizmoActiveEntity);
			const ImU32 color = TransformGizmoAxisColor(axis.Axis, highlighted);
			const float thickness = highlighted ? 3.8f : 2.4f;
			drawList->AddLine(originScreen, axis.EndScreen, color, thickness);
			if (m_TransformGizmoMode == TransformGizmoMode::Rotate)
			{
				drawList->AddCircle(axis.EndScreen, highlighted ? 8.0f : 6.5f, color, 18, thickness * 0.55f);
			}
			else if (m_TransformGizmoMode == TransformGizmoMode::Scale)
			{
				const float halfSize = highlighted ? 5.5f : 4.5f;
				drawList->AddRectFilled(
					ImVec2(axis.EndScreen.x - halfSize, axis.EndScreen.y - halfSize),
					ImVec2(axis.EndScreen.x + halfSize, axis.EndScreen.y + halfSize),
					color);
				drawList->AddRect(
					ImVec2(axis.EndScreen.x - halfSize - 1.2f, axis.EndScreen.y - halfSize - 1.2f),
					ImVec2(axis.EndScreen.x + halfSize + 1.2f, axis.EndScreen.y + halfSize + 1.2f),
					highlighted ? IM_COL32(255, 246, 196, 245) : IM_COL32(12, 16, 22, 210),
					1.0f,
					0,
					highlighted ? 1.8f : 1.0f);
			}
			else
			{
				drawList->AddTriangleFilled(
					axis.EndScreen,
					ImVec2(axis.EndScreen.x - 5.0f, axis.EndScreen.y + 10.0f),
					ImVec2(axis.EndScreen.x + 5.0f, axis.EndScreen.y + 10.0f),
					color);
			}
			drawList->AddText(
				ImVec2(axis.EndScreen.x + 6.0f, axis.EndScreen.y - 8.0f),
				color,
				TransformGizmoAxisName(axis.Axis));
		}

		if (m_TransformGizmoUniformScaleActive && selectedEntity == m_TransformGizmoActiveEntity)
		{
			const auto averageAbsScale = [](const DirectX::XMFLOAT3& scale) noexcept
			{
				return (std::fabs(scale.x) + std::fabs(scale.y) + std::fabs(scale.z)) / 3.0f;
			};
			const float factor = averageAbsScale(transform->LocalTransform.Scale) /
				(std::max)(0.0001f, averageAbsScale(m_TransformGizmoStartTransform.Scale));
			const std::string activeLabel = std::format(
				"Scale Uniform {:.2f}x  {}{}",
				factor,
				formatScaleTriplet(transform->LocalTransform.Scale),
				m_TransformSnappingEnabled ? " Snap" : "");
			drawGizmoBadge(ImVec2(originScreen.x + 12.0f, originScreen.y + 10.0f), activeLabel, IM_COL32(255, 246, 196, 190));
			return true;
		}
		if (m_TransformGizmoActivePlane != TransformGizmoPlane::None && selectedEntity == m_TransformGizmoActiveEntity)
		{
			const std::string activeLabel = std::format(
				"{} {} {} {}{}",
				TransformGizmoModeName(m_TransformGizmoMode),
				TransformGizmoSpaceName(m_TransformGizmoSpace),
				TransformGizmoPivotName(m_TransformGizmoPivot),
				TransformGizmoPlaneName(m_TransformGizmoActivePlane),
				m_TransformSnappingEnabled ? " Snap" : "");
			drawGizmoBadge(ImVec2(originScreen.x + 12.0f, originScreen.y + 10.0f), activeLabel, IM_COL32(255, 215, 88, 170));
			return true;
		}
		if (m_TransformGizmoActiveAxis != TransformGizmoAxis::None && selectedEntity == m_TransformGizmoActiveEntity)
		{
			const std::string activeLabel = m_TransformGizmoMode == TransformGizmoMode::Scale
				? std::format(
					"Scale {} {:.2f}  {}{}",
					TransformGizmoAxisName(m_TransformGizmoActiveAxis),
					scaleComponent(transform->LocalTransform.Scale, m_TransformGizmoActiveAxis),
					formatScaleTriplet(transform->LocalTransform.Scale),
					m_TransformSnappingEnabled ? " Snap" : "")
				: std::format(
					"{} {} {} {}{}",
					TransformGizmoModeName(m_TransformGizmoMode),
					TransformGizmoSpaceName(m_TransformGizmoSpace),
					TransformGizmoPivotName(m_TransformGizmoPivot),
					TransformGizmoAxisName(m_TransformGizmoActiveAxis),
					m_TransformSnappingEnabled ? " Snap" : "");
			drawGizmoBadge(
				ImVec2(originScreen.x + 12.0f, originScreen.y + 10.0f),
				activeLabel,
				TransformGizmoAxisColor(m_TransformGizmoActiveAxis, true));
			return true;
		}

		if (m_TransformGizmoMode == TransformGizmoMode::Scale)
		{
			if (hoveredUniformScale)
			{
				const std::string hoverLabel = std::format("Uniform Scale  {}", formatScaleTriplet(transform->LocalTransform.Scale));
				drawGizmoBadge(ImVec2(originScreen.x + 12.0f, originScreen.y + 10.0f), hoverLabel, IM_COL32(255, 246, 196, 150));
				ImGui::SetTooltip("Uniform scale. Drag diagonally to scale all axes together.");
			}
			else if (hoveredAxis != TransformGizmoAxis::None)
			{
				const std::string hoverLabel = std::format(
					"Scale {} {:.2f}",
					TransformGizmoAxisName(hoveredAxis),
					scaleComponent(transform->LocalTransform.Scale, hoveredAxis));
				drawGizmoBadge(
					ImVec2(originScreen.x + 12.0f, originScreen.y + 10.0f),
					hoverLabel,
					TransformGizmoAxisColor(hoveredAxis, true));
				ImGui::SetTooltip("Scale %s axis", TransformGizmoAxisName(hoveredAxis));
			}
		}

		return hoveredAxis != TransformGizmoAxis::None || hoveredPlane != TransformGizmoPlane::None || hoveredUniformScale;
	}

	void EditorLayer::DrawSelectionBoundsGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (!m_ShowSelectionOutline || selectedEntity == InvalidEntityId || !drawList)
		{
			return;
		}

		const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity);
		const TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity);
		if (!bounds || !transform)
		{
			return;
		}

		if (context.ActiveScene.GetMeshComponent(selectedEntity) && !context.ActiveScene.IsMeshEnabled(selectedEntity))
		{
			return;
		}

		constexpr std::array<std::pair<size_t, size_t>, 12> kBoxEdges = {{
			{ 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
			{ 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		}};

		const std::array<DirectX::XMFLOAT3, 8> localCorners = {{
			{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMin.z },
			{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMin.z },
			{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMin.z },
			{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMin.z },
			{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMax.z },
			{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMax.z },
			{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMax.z },
			{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMax.z }
		}};

		const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
		std::array<ImVec2, 8> projectedCorners = {};
		std::array<bool, 8> projected = {};
		size_t projectedCount = 0;
		ImVec2 projectedMin((std::numeric_limits<float>::max)(), (std::numeric_limits<float>::max)());
		ImVec2 projectedMax((std::numeric_limits<float>::lowest)(), (std::numeric_limits<float>::lowest)());
		for (size_t cornerIndex = 0; cornerIndex < localCorners.size(); ++cornerIndex)
		{
			DirectX::XMFLOAT3 worldCorner = {};
			DirectX::XMStoreFloat3(
				&worldCorner,
				DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorners[cornerIndex]), worldMatrix));
			projected[cornerIndex] = ProjectWorldToSceneCanvas(context.SceneCamera, worldCorner, canvasPosition, canvasSize, projectedCorners[cornerIndex]);
			if (projected[cornerIndex])
			{
				++projectedCount;
				projectedMin.x = (std::min)(projectedMin.x, projectedCorners[cornerIndex].x);
				projectedMin.y = (std::min)(projectedMin.y, projectedCorners[cornerIndex].y);
				projectedMax.x = (std::max)(projectedMax.x, projectedCorners[cornerIndex].x);
				projectedMax.y = (std::max)(projectedMax.y, projectedCorners[cornerIndex].y);
			}
		}

		if (projectedCount < 2)
		{
			return;
		}

		projectedMin.x = std::clamp(projectedMin.x, canvasPosition.x, canvasPosition.x + canvasSize.x);
		projectedMin.y = std::clamp(projectedMin.y, canvasPosition.y, canvasPosition.y + canvasSize.y);
		projectedMax.x = std::clamp(projectedMax.x, canvasPosition.x, canvasPosition.x + canvasSize.x);
		projectedMax.y = std::clamp(projectedMax.y, canvasPosition.y, canvasPosition.y + canvasSize.y);

		const ImU32 fillColor = IM_COL32(255, 214, 84, 22);
		const ImU32 rectColor = IM_COL32(255, 226, 104, 70);
		if (projectedMax.x > projectedMin.x + 4.0f && projectedMax.y > projectedMin.y + 4.0f)
		{
			drawList->AddRectFilled(projectedMin, projectedMax, fillColor, 3.0f);
			drawList->AddRect(projectedMin, projectedMax, rectColor, 3.0f, 0, 1.0f);
		}

		const ImU32 glowColor = IM_COL32(255, 214, 84, 78);
		const ImU32 edgeColor = IM_COL32(255, 226, 104, 230);
		for (const auto& [begin, end] : kBoxEdges)
		{
			if (!projected[begin] || !projected[end])
			{
				continue;
			}

			drawList->AddLine(projectedCorners[begin], projectedCorners[end], glowColor, 4.0f);
			drawList->AddLine(projectedCorners[begin], projectedCorners[end], edgeColor, 1.45f);
		}

		for (size_t cornerIndex = 0; cornerIndex < projectedCorners.size(); ++cornerIndex)
		{
			if (!projected[cornerIndex])
			{
				continue;
			}

			drawList->AddCircleFilled(projectedCorners[cornerIndex], 3.4f, IM_COL32(255, 248, 188, 245), 12);
			drawList->AddCircle(projectedCorners[cornerIndex], 5.3f, IM_COL32(255, 184, 72, 180), 12, 1.1f);
		}

		const DirectX::XMFLOAT3 dimensions = {
			(bounds->LocalMax.x - bounds->LocalMin.x) * std::fabs(transform->WorldTransform.Scale.x),
			(bounds->LocalMax.y - bounds->LocalMin.y) * std::fabs(transform->WorldTransform.Scale.y),
			(bounds->LocalMax.z - bounds->LocalMin.z) * std::fabs(transform->WorldTransform.Scale.z)
		};
		std::string label = std::format("{:.2f} x {:.2f} x {:.2f} m", dimensions.x, dimensions.y, dimensions.z);
		if (const std::string* name = context.ActiveScene.GetEntityName(selectedEntity); name && !name->empty())
		{
			label = std::format("{} | {}", *name, label);
		}

		const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
		ImVec2 labelMin(projectedMin.x, projectedMin.y - textSize.y - 10.0f);
		const float minLabelX = canvasPosition.x + 4.0f;
		const float minLabelY = canvasPosition.y + 4.0f;
		const float maxLabelX = (std::max)(minLabelX, canvasPosition.x + canvasSize.x - textSize.x - 16.0f);
		const float maxLabelY = (std::max)(minLabelY, canvasPosition.y + canvasSize.y - textSize.y - 12.0f);
		labelMin.x = std::clamp(labelMin.x, minLabelX, maxLabelX);
		labelMin.y = std::clamp(labelMin.y, minLabelY, maxLabelY);
		const ImVec2 labelMax(labelMin.x + textSize.x + 12.0f, labelMin.y + textSize.y + 6.0f);
		drawList->AddRectFilled(labelMin, labelMax, IM_COL32(14, 18, 24, 226), 4.0f);
		drawList->AddRect(labelMin, labelMax, IM_COL32(255, 226, 104, 155), 4.0f);
		drawList->AddText(ImVec2(labelMin.x + 6.0f, labelMin.y + 3.0f), IM_COL32(255, 246, 202, 255), label.c_str());
	}

	void EditorLayer::DrawColliderGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		constexpr std::array<std::pair<size_t, size_t>, 12> kBoxEdges = {{
			{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
			{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		}};
		constexpr std::array<std::pair<size_t, size_t>, 4> kPlaneEdges = {{
			{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }
		}};

		for (const SceneEntity& entity : context.ActiveScene.GetEntities())
		{
			const ColliderComponent* collider = context.ActiveScene.GetColliderComponent(entity.Id);
			const TransformComponent* transform = context.ActiveScene.GetTransformComponent(entity.Id);
			if (!collider || !context.ActiveScene.IsColliderEnabled(entity.Id) || !transform)
			{
				continue;
			}

			DirectX::XMFLOAT3 halfExtents = {};
			switch (collider->Shape)
			{
			case Physics::ColliderShape::Sphere:
				halfExtents = { collider->Radius, collider->Radius, collider->Radius };
				break;
			case Physics::ColliderShape::Capsule:
				halfExtents = { collider->Radius, collider->Height * 0.5f, collider->Radius };
				break;
			case Physics::ColliderShape::Plane:
				halfExtents = { (std::max)(collider->Size.x * 0.5f, 0.5f), 0.0f, (std::max)(collider->Size.z * 0.5f, 0.5f) };
				break;
			case Physics::ColliderShape::Box:
			default:
				halfExtents = { collider->Size.x * 0.5f, collider->Size.y * 0.5f, collider->Size.z * 0.5f };
				break;
			}

			const DirectX::XMFLOAT3 center = collider->Offset;
			std::array<DirectX::XMFLOAT3, 8> localCorners = {{
				{ center.x - halfExtents.x, center.y + halfExtents.y, center.z - halfExtents.z },
				{ center.x + halfExtents.x, center.y + halfExtents.y, center.z - halfExtents.z },
				{ center.x + halfExtents.x, center.y + halfExtents.y, center.z + halfExtents.z },
				{ center.x - halfExtents.x, center.y + halfExtents.y, center.z + halfExtents.z },
				{ center.x - halfExtents.x, center.y - halfExtents.y, center.z - halfExtents.z },
				{ center.x + halfExtents.x, center.y - halfExtents.y, center.z - halfExtents.z },
				{ center.x + halfExtents.x, center.y - halfExtents.y, center.z + halfExtents.z },
				{ center.x - halfExtents.x, center.y - halfExtents.y, center.z + halfExtents.z }
			}};

			if (collider->Shape == Physics::ColliderShape::Plane)
			{
				localCorners[0] = { center.x - halfExtents.x, center.y, center.z - halfExtents.z };
				localCorners[1] = { center.x + halfExtents.x, center.y, center.z - halfExtents.z };
				localCorners[2] = { center.x + halfExtents.x, center.y, center.z + halfExtents.z };
				localCorners[3] = { center.x - halfExtents.x, center.y, center.z + halfExtents.z };
			}

			const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
			std::array<ImVec2, 8> projectedCorners = {};
			std::array<bool, 8> projected = {};
			const size_t cornerCount = collider->Shape == Physics::ColliderShape::Plane ? 4 : 8;
			for (size_t cornerIndex = 0; cornerIndex < cornerCount; ++cornerIndex)
			{
				DirectX::XMFLOAT3 worldCorner = {};
				DirectX::XMStoreFloat3(
					&worldCorner,
					DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorners[cornerIndex]), worldMatrix));
				projected[cornerIndex] = ProjectWorldToSceneCanvas(context.SceneCamera, worldCorner, canvasPosition, canvasSize, projectedCorners[cornerIndex]);
			}

			const bool selected = entity.Id == context.ActiveScene.GetSelectedEntity();
			const ImU32 color = selected ? IM_COL32(124, 255, 154, 235) : IM_COL32(91, 221, 255, 150);
			const float thickness = selected ? 1.8f : 1.2f;
			const auto drawEdge = [&](size_t begin, size_t end)
				{
					if (projected[begin] && projected[end])
					{
						drawList->AddLine(projectedCorners[begin], projectedCorners[end], color, thickness);
					}
				};

			if (collider->Shape == Physics::ColliderShape::Plane)
			{
				for (const auto& [begin, end] : kPlaneEdges)
				{
					drawEdge(begin, end);
				}
			}
			else
			{
				for (const auto& [begin, end] : kBoxEdges)
				{
					drawEdge(begin, end);
				}
			}
		}
	}

	void EditorLayer::DrawGameCameraFrustumGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		const Camera& gameCamera = context.GameCamera;
		const float nearZ = (std::max)(gameCamera.GetNearZ(), 0.001f);
		const float cameraFarZ = (std::max)(gameCamera.GetFarZ(), nearZ + 0.01f);
		const float farZ = std::clamp(m_GameCameraGizmoDepth, nearZ + 0.01f, cameraFarZ);
		const float aspect = (std::max)(gameCamera.GetAspect(), 0.01f);
		const float fovY = std::clamp(gameCamera.GetFovY(), 0.01f, DirectX::XM_PI - 0.01f);
		const float halfFovTangent = std::tan(fovY * 0.5f);

		const float nearHalfHeight = halfFovTangent * nearZ;
		const float nearHalfWidth = nearHalfHeight * aspect;
		const float farHalfHeight = halfFovTangent * farZ;
		const float farHalfWidth = farHalfHeight * aspect;

		const DirectX::XMFLOAT3 cameraPositionValue = gameCamera.GetPosition();
		const DirectX::XMFLOAT3 forwardValue = gameCamera.GetForward();
		const DirectX::XMFLOAT3 rightValue = gameCamera.GetRight();
		const DirectX::XMFLOAT3 upValue = gameCamera.GetUp();
		const DirectX::XMVECTOR cameraPosition = DirectX::XMLoadFloat3(&cameraPositionValue);
		const DirectX::XMVECTOR forward = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&forwardValue));
		const DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&rightValue));
		const DirectX::XMVECTOR up = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&upValue));
		const DirectX::XMVECTOR nearCenter = cameraPosition + DirectX::XMVectorScale(forward, nearZ);
		const DirectX::XMVECTOR farCenter = cameraPosition + DirectX::XMVectorScale(forward, farZ);

		const auto makeCorner = [](DirectX::XMVECTOR center, DirectX::XMVECTOR rightAxis, float rightDistance, DirectX::XMVECTOR upAxis, float upDistance) noexcept
			{
				return center + DirectX::XMVectorScale(rightAxis, rightDistance) + DirectX::XMVectorScale(upAxis, upDistance);
			};

		std::array<DirectX::XMFLOAT3, 8> corners = {};
		DirectX::XMStoreFloat3(&corners[0], makeCorner(nearCenter, right, -nearHalfWidth, up, nearHalfHeight));
		DirectX::XMStoreFloat3(&corners[1], makeCorner(nearCenter, right, nearHalfWidth, up, nearHalfHeight));
		DirectX::XMStoreFloat3(&corners[2], makeCorner(nearCenter, right, nearHalfWidth, up, -nearHalfHeight));
		DirectX::XMStoreFloat3(&corners[3], makeCorner(nearCenter, right, -nearHalfWidth, up, -nearHalfHeight));
		DirectX::XMStoreFloat3(&corners[4], makeCorner(farCenter, right, -farHalfWidth, up, farHalfHeight));
		DirectX::XMStoreFloat3(&corners[5], makeCorner(farCenter, right, farHalfWidth, up, farHalfHeight));
		DirectX::XMStoreFloat3(&corners[6], makeCorner(farCenter, right, farHalfWidth, up, -farHalfHeight));
		DirectX::XMStoreFloat3(&corners[7], makeCorner(farCenter, right, -farHalfWidth, up, -farHalfHeight));

		std::array<ImVec2, 8> projectedCorners = {};
		std::array<bool, 8> projected = {};
		for (size_t i = 0; i < corners.size(); ++i)
		{
			projected[i] = ProjectWorldToSceneCanvas(context.SceneCamera, corners[i], canvasPosition, canvasSize, projectedCorners[i]);
		}

		constexpr ImU32 nearColor = IM_COL32(255, 211, 92, 240);
		constexpr ImU32 farColor = IM_COL32(255, 211, 92, 145);
		constexpr ImU32 edgeColor = IM_COL32(109, 213, 255, 185);
		constexpr float lineThickness = 1.6f;

		const auto drawLine = [&](size_t begin, size_t end, ImU32 color, float thickness)
			{
				if (projected[begin] && projected[end])
				{
					drawList->AddLine(projectedCorners[begin], projectedCorners[end], color, thickness);
				}
			};

		drawLine(0, 1, nearColor, lineThickness);
		drawLine(1, 2, nearColor, lineThickness);
		drawLine(2, 3, nearColor, lineThickness);
		drawLine(3, 0, nearColor, lineThickness);
		drawLine(4, 5, farColor, lineThickness);
		drawLine(5, 6, farColor, lineThickness);
		drawLine(6, 7, farColor, lineThickness);
		drawLine(7, 4, farColor, lineThickness);
		drawLine(0, 4, edgeColor, lineThickness);
		drawLine(1, 5, edgeColor, lineThickness);
		drawLine(2, 6, edgeColor, lineThickness);
		drawLine(3, 7, edgeColor, lineThickness);

		DirectX::XMFLOAT3 cameraWorldPosition = {};
		DirectX::XMStoreFloat3(&cameraWorldPosition, cameraPosition);
		ImVec2 cameraScreenPosition = {};
		if (ProjectWorldToSceneCanvas(context.SceneCamera, cameraWorldPosition, canvasPosition, canvasSize, cameraScreenPosition))
		{
			constexpr ImU32 markerFillColor = IM_COL32(255, 236, 148, 245);
			constexpr ImU32 markerStrokeColor = IM_COL32(25, 32, 42, 230);
			drawList->AddCircleFilled(cameraScreenPosition, 4.0f, markerFillColor, 12);
			drawList->AddCircle(cameraScreenPosition, 7.0f, markerStrokeColor, 12, 1.8f);
			drawList->AddLine(
				ImVec2(cameraScreenPosition.x - 7.0f, cameraScreenPosition.y),
				ImVec2(cameraScreenPosition.x + 7.0f, cameraScreenPosition.y),
				markerStrokeColor,
				1.4f);
			drawList->AddLine(
				ImVec2(cameraScreenPosition.x, cameraScreenPosition.y - 7.0f),
				ImVec2(cameraScreenPosition.x, cameraScreenPosition.y + 7.0f),
				markerStrokeColor,
				1.4f);
			drawList->AddText(
				ImVec2(cameraScreenPosition.x + 10.0f, cameraScreenPosition.y - 10.0f),
				IM_COL32(255, 244, 190, 235),
				"Game Camera");
		}
	}

	bool EditorLayer::ProjectWorldToSceneCanvas(
		const Camera& sceneCamera,
		const DirectX::XMFLOAT3& worldPosition,
		const ImVec2& canvasPosition,
		const ImVec2& canvasSize,
		ImVec2& screenPosition) const
	{
		const DirectX::XMVECTOR world = DirectX::XMLoadFloat3(&worldPosition);
		const DirectX::XMVECTOR clip = DirectX::XMVector3Transform(world, sceneCamera.GetViewProjectionMatrix());
		const float w = DirectX::XMVectorGetW(clip);
		if (!std::isfinite(w) || w <= 0.0001f)
		{
			return false;
		}

		const float ndcX = DirectX::XMVectorGetX(clip) / w;
		const float ndcY = DirectX::XMVectorGetY(clip) / w;
		if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
		{
			return false;
		}

		screenPosition.x = canvasPosition.x + ((ndcX + 1.0f) * 0.5f * canvasSize.x);
		screenPosition.y = canvasPosition.y + ((1.0f - ndcY) * 0.5f * canvasSize.y);
		return true;
	}

	bool EditorLayer::BuildSceneMouseRay(
		const Camera& sceneCamera,
		const ImVec2& mousePosition,
		const ImVec2& canvasPosition,
		const ImVec2& canvasSize,
		DirectX::XMFLOAT3& rayOrigin,
		DirectX::XMFLOAT3& rayDirection) const
	{
		if (canvasSize.x <= 1.0f || canvasSize.y <= 1.0f)
		{
			return false;
		}

		const float ndcX = ((mousePosition.x - canvasPosition.x) / canvasSize.x) * 2.0f - 1.0f;
		const float ndcY = 1.0f - ((mousePosition.y - canvasPosition.y) / canvasSize.y) * 2.0f;
		if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
		{
			return false;
		}

		const DirectX::XMMATRIX inverseViewProjection = DirectX::XMMatrixInverse(nullptr, sceneCamera.GetViewProjectionMatrix());
		const DirectX::XMVECTOR nearPoint = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverseViewProjection);
		const DirectX::XMVECTOR farPoint = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverseViewProjection);
		const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(farPoint, nearPoint));
		DirectX::XMStoreFloat3(&rayOrigin, nearPoint);
		DirectX::XMStoreFloat3(&rayDirection, direction);
		return std::isfinite(rayOrigin.x) && std::isfinite(rayOrigin.y) && std::isfinite(rayOrigin.z) &&
			std::isfinite(rayDirection.x) && std::isfinite(rayDirection.y) && std::isfinite(rayDirection.z);
	}

	bool EditorLayer::ProjectSceneMouseToMeasureTarget(
		EditorContext& context,
		const ImVec2& mousePosition,
		const ImVec2& canvasPosition,
		const ImVec2& canvasSize,
		DirectX::XMFLOAT3& worldPosition)
	{
		DirectX::XMFLOAT3 rayOrigin = {};
		DirectX::XMFLOAT3 rayDirection = {};
		if (!BuildSceneMouseRay(context.SceneCamera, mousePosition, canvasPosition, canvasSize, rayOrigin, rayDirection))
		{
			return false;
		}

		switch (m_MeasureTarget)
		{
		case SceneMeasureTarget::ViewPlane:
		{
			const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
			ViewFocus focus = selectedEntity != InvalidEntityId
				? ComputeEntityViewFocus(context.ActiveScene, selectedEntity)
				: ViewFocus{};
			if (selectedEntity == InvalidEntityId)
			{
				const DirectX::XMFLOAT3 cameraForward = context.SceneCamera.GetForward();
				const DirectX::XMFLOAT3 cameraPosition = context.SceneCamera.GetPosition();
				focus.Center = {
					cameraPosition.x + cameraForward.x * 10.0f,
					cameraPosition.y + cameraForward.y * 10.0f,
					cameraPosition.z + cameraForward.z * 10.0f
				};
			}
			return IntersectRayPlane(rayOrigin, rayDirection, focus.Center, context.SceneCamera.GetForward(), worldPosition);
		}
		case SceneMeasureTarget::MeshSurface:
		{
			m_LastMeshMeasureTrianglesTested = 0;
			m_LastMeshMeasureTriangleCount = 0;
			m_LastMeshMeasureCacheTriangleCount = 0;
			m_LastMeshMeasureRaycastMs = 0.0;
			m_LastMeshMeasureCacheBuildMs = 0.0;
			m_LastMeshMeasureUsedBudget = false;
			m_LastMeshMeasureUsedAcceleration = false;
			m_LastMeshMeasureUsedDynamicAcceleration = false;
			m_LastMeshMeasureCacheRebuilt = false;
			m_LastMeshMeasureBoundsRejected = false;
			m_LastMeshMeasureHit = false;
			const auto meshMeasureStartTime = std::chrono::steady_clock::now();
			const auto finishMeshMeasureTiming = [this, meshMeasureStartTime]()
			{
				const auto elapsed = std::chrono::steady_clock::now() - meshMeasureStartTime;
				m_LastMeshMeasureRaycastMs = std::chrono::duration<double, std::milli>(elapsed).count();
			};

			const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
			const Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(selectedEntity);
			const TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity);
			if (selectedEntity != InvalidEntityId && mesh && transform && context.ActiveScene.IsMeshEnabled(selectedEntity))
			{
				const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
				bool canTestMeshTriangles = true;
				const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity);
				if (bounds)
				{
					const std::array<DirectX::XMFLOAT3, 8> localCorners = {{
						{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMin.z },
						{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMin.z },
						{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMin.z },
						{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMin.z },
						{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMax.z },
						{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMax.z },
						{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMax.z },
						{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMax.z }
					}};
					DirectX::XMFLOAT3 worldMin = {
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)(),
						(std::numeric_limits<float>::max)()
					};
					DirectX::XMFLOAT3 worldMax = {
						(std::numeric_limits<float>::lowest)(),
						(std::numeric_limits<float>::lowest)(),
						(std::numeric_limits<float>::lowest)()
					};
					for (const DirectX::XMFLOAT3& localCorner : localCorners)
					{
						DirectX::XMFLOAT3 worldCorner = {};
						DirectX::XMStoreFloat3(
							&worldCorner,
							DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorner), worldMatrix));
						worldMin.x = (std::min)(worldMin.x, worldCorner.x);
						worldMin.y = (std::min)(worldMin.y, worldCorner.y);
						worldMin.z = (std::min)(worldMin.z, worldCorner.z);
						worldMax.x = (std::max)(worldMax.x, worldCorner.x);
						worldMax.y = (std::max)(worldMax.y, worldCorner.y);
						worldMax.z = (std::max)(worldMax.z, worldCorner.z);
					}

					DirectX::XMFLOAT3 boundsHit = {};
					if (!IntersectRayAabbPoint(rayOrigin, rayDirection, worldMin, worldMax, boundsHit))
					{
						const size_t triangleCount = !mesh->Indices.empty()
							? mesh->Indices.size() / 3
							: mesh->Vertices.size() / 3;
						m_LastMeshMeasureTriangleCount = triangleCount;
						m_LastMeshMeasureBoundsRejected = true;
						canTestMeshTriangles = false;
					}
				}

				if (canTestMeshTriangles)
				{
					const DirectX::XMMATRIX inverseWorldMatrix = DirectX::XMMatrixInverse(nullptr, worldMatrix);
					DirectX::XMFLOAT3 localRayOrigin = {};
					DirectX::XMFLOAT3 localRayDirection = {};
					DirectX::XMStoreFloat3(
						&localRayOrigin,
						DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&rayOrigin), inverseWorldMatrix));
					DirectX::XMStoreFloat3(
						&localRayDirection,
						DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&rayDirection), inverseWorldMatrix)));
					if (!std::isfinite(localRayOrigin.x) || !std::isfinite(localRayOrigin.y) || !std::isfinite(localRayOrigin.z) ||
						!std::isfinite(localRayDirection.x) || !std::isfinite(localRayDirection.y) || !std::isfinite(localRayDirection.z))
					{
						canTestMeshTriangles = false;
					}

					if (canTestMeshTriangles)
					{
						const auto vertexPosition = [&](uint32_t vertexIndex) noexcept
							{
								if (vertexIndex < mesh->Vertices.size())
								{
									return mesh->Vertices[vertexIndex].Position;
								}
								return DirectX::XMFLOAT3{};
							};

						float closestDistance = (std::numeric_limits<float>::max)();
						DirectX::XMFLOAT3 closestHit = {};
						bool foundHit = false;
						const size_t triangleCount = !mesh->Indices.empty()
							? mesh->Indices.size() / 3
							: mesh->Vertices.size() / 3;
						constexpr size_t kMeshMeasureTriangleBudget = 65536;
						constexpr size_t kMeshMeasureCacheTriangleLimit = 262144;
						const size_t triangleBudget = (std::min)(triangleCount, kMeshMeasureTriangleBudget);
						const uint64_t cacheFrameIndex = mesh->IsAnimated ? context.RenderFrameStats.FrameIndex : 0;
						m_LastMeshMeasureTriangleCount = triangleCount;
						m_LastMeshMeasureUsedBudget = triangleCount > triangleBudget;

						const auto testTriangle = [&](
							const DirectX::XMFLOAT3& v0,
							const DirectX::XMFLOAT3& v1,
							const DirectX::XMFLOAT3& v2)
							{
								if (m_LastMeshMeasureTrianglesTested >= triangleBudget)
								{
									m_LastMeshMeasureUsedBudget = true;
									return;
								}

								float t = 0.0f;
								DirectX::XMFLOAT3 localHit = {};
								++m_LastMeshMeasureTrianglesTested;
								if (IntersectRayTrianglePoint(localRayOrigin, localRayDirection, v0, v1, v2, t, localHit))
								{
									DirectX::XMFLOAT3 worldHit = {};
									DirectX::XMStoreFloat3(
										&worldHit,
										DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&localHit), worldMatrix));
									const float distance = Distance(rayOrigin, worldHit);
									if (distance < closestDistance)
									{
										closestDistance = distance;
										closestHit = worldHit;
										foundHit = true;
									}
								}
							};

						const bool canUseMeshAcceleration =
							triangleCount > 0 &&
							triangleCount <= kMeshMeasureCacheTriangleLimit;
						if (canUseMeshAcceleration &&
							(m_MeshMeasureAccelerationCache.Mesh != mesh ||
								m_MeshMeasureAccelerationCache.VertexCount != mesh->Vertices.size() ||
								m_MeshMeasureAccelerationCache.IndexCount != mesh->Indices.size() ||
								m_MeshMeasureAccelerationCache.Animated != mesh->IsAnimated ||
								m_MeshMeasureAccelerationCache.FrameIndex != cacheFrameIndex))
						{
							const auto cacheBuildStartTime = std::chrono::steady_clock::now();
							m_MeshMeasureAccelerationCache = {};
							m_MeshMeasureAccelerationCache.Mesh = mesh;
							m_MeshMeasureAccelerationCache.VertexCount = mesh->Vertices.size();
							m_MeshMeasureAccelerationCache.IndexCount = mesh->Indices.size();
							m_MeshMeasureAccelerationCache.FrameIndex = cacheFrameIndex;
							m_MeshMeasureAccelerationCache.Animated = mesh->IsAnimated;
							m_MeshMeasureAccelerationCache.Triangles.reserve(triangleCount);
							const auto cacheTriangle = [&](uint32_t i0, uint32_t i1, uint32_t i2)
								{
									if (i0 >= mesh->Vertices.size() || i1 >= mesh->Vertices.size() || i2 >= mesh->Vertices.size())
									{
										return;
									}

									const DirectX::XMFLOAT3 v0 = vertexPosition(i0);
									const DirectX::XMFLOAT3 v1 = vertexPosition(i1);
									const DirectX::XMFLOAT3 v2 = vertexPosition(i2);
									MeshMeasureTriangleCacheEntry entry;
									entry.I0 = i0;
									entry.I1 = i1;
									entry.I2 = i2;
									entry.LocalMin = {
										(std::min)({ v0.x, v1.x, v2.x }),
										(std::min)({ v0.y, v1.y, v2.y }),
										(std::min)({ v0.z, v1.z, v2.z })
									};
									entry.LocalMax = {
										(std::max)({ v0.x, v1.x, v2.x }),
										(std::max)({ v0.y, v1.y, v2.y }),
										(std::max)({ v0.z, v1.z, v2.z })
									};
									m_MeshMeasureAccelerationCache.Triangles.push_back(entry);
								};
							if (!mesh->Indices.empty())
							{
								const size_t triangleIndexCount = mesh->Indices.size() - (mesh->Indices.size() % 3);
								for (size_t index = 0; index + 2 < triangleIndexCount; index += 3)
								{
									cacheTriangle(mesh->Indices[index + 0], mesh->Indices[index + 1], mesh->Indices[index + 2]);
								}
							}
							else
							{
								const size_t triangleVertexCount = mesh->Vertices.size() - (mesh->Vertices.size() % 3);
								for (size_t vertex = 0; vertex + 2 < triangleVertexCount; vertex += 3)
								{
									cacheTriangle(
										static_cast<uint32_t>(vertex + 0),
										static_cast<uint32_t>(vertex + 1),
										static_cast<uint32_t>(vertex + 2));
								}
							}
							const auto cacheBuildElapsed = std::chrono::steady_clock::now() - cacheBuildStartTime;
							m_LastMeshMeasureCacheBuildMs = std::chrono::duration<double, std::milli>(cacheBuildElapsed).count();
							m_LastMeshMeasureCacheRebuilt = true;
						}

						if (canUseMeshAcceleration &&
							m_MeshMeasureAccelerationCache.Mesh == mesh &&
							m_MeshMeasureAccelerationCache.VertexCount == mesh->Vertices.size() &&
							m_MeshMeasureAccelerationCache.IndexCount == mesh->Indices.size() &&
							m_MeshMeasureAccelerationCache.Animated == mesh->IsAnimated &&
							m_MeshMeasureAccelerationCache.FrameIndex == cacheFrameIndex)
						{
							m_LastMeshMeasureUsedAcceleration = true;
							m_LastMeshMeasureUsedDynamicAcceleration = mesh->IsAnimated;
							m_LastMeshMeasureCacheTriangleCount = m_MeshMeasureAccelerationCache.Triangles.size();
							for (const MeshMeasureTriangleCacheEntry& triangle : m_MeshMeasureAccelerationCache.Triangles)
							{
								if (m_LastMeshMeasureTrianglesTested >= triangleBudget)
								{
									m_LastMeshMeasureUsedBudget = true;
									break;
								}

								DirectX::XMFLOAT3 triangleBoundsHit = {};
								if (!IntersectRayAabbPoint(localRayOrigin, localRayDirection, triangle.LocalMin, triangle.LocalMax, triangleBoundsHit))
								{
									continue;
								}

								testTriangle(vertexPosition(triangle.I0), vertexPosition(triangle.I1), vertexPosition(triangle.I2));
							}
						}
						else if (!mesh->Indices.empty())
						{
							const size_t triangleIndexCount = mesh->Indices.size() - (mesh->Indices.size() % 3);
							for (size_t index = 0; index + 2 < triangleIndexCount && m_LastMeshMeasureTrianglesTested < triangleBudget; index += 3)
							{
								const uint32_t i0 = mesh->Indices[index + 0];
								const uint32_t i1 = mesh->Indices[index + 1];
								const uint32_t i2 = mesh->Indices[index + 2];
								if (i0 >= mesh->Vertices.size() || i1 >= mesh->Vertices.size() || i2 >= mesh->Vertices.size())
								{
									continue;
								}

								testTriangle(vertexPosition(i0), vertexPosition(i1), vertexPosition(i2));
							}
						}
						else
						{
							const size_t triangleVertexCount = mesh->Vertices.size() - (mesh->Vertices.size() % 3);
							for (size_t vertex = 0; vertex + 2 < triangleVertexCount && m_LastMeshMeasureTrianglesTested < triangleBudget; vertex += 3)
							{
								testTriangle(
									vertexPosition(static_cast<uint32_t>(vertex + 0)),
									vertexPosition(static_cast<uint32_t>(vertex + 1)),
									vertexPosition(static_cast<uint32_t>(vertex + 2)));
							}
						}

						if (foundHit)
						{
							m_LastMeshMeasureHit = true;
							worldPosition = closestHit;
							finishMeshMeasureTiming();
							return true;
						}
					}
				}
			}
			finishMeshMeasureTiming();
		}
		[[fallthrough]];
		case SceneMeasureTarget::SelectionBounds:
		{
			const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
			const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity);
			const TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity);
			if (selectedEntity != InvalidEntityId && bounds && transform)
			{
				const std::array<DirectX::XMFLOAT3, 8> localCorners = {{
					{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMin.z },
					{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMin.z },
					{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMin.z },
					{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMin.z },
					{ bounds->LocalMin.x, bounds->LocalMin.y, bounds->LocalMax.z },
					{ bounds->LocalMax.x, bounds->LocalMin.y, bounds->LocalMax.z },
					{ bounds->LocalMin.x, bounds->LocalMax.y, bounds->LocalMax.z },
					{ bounds->LocalMax.x, bounds->LocalMax.y, bounds->LocalMax.z }
				}};

				DirectX::XMFLOAT3 worldMin = {
					(std::numeric_limits<float>::max)(),
					(std::numeric_limits<float>::max)(),
					(std::numeric_limits<float>::max)()
				};
				DirectX::XMFLOAT3 worldMax = {
					(std::numeric_limits<float>::lowest)(),
					(std::numeric_limits<float>::lowest)(),
					(std::numeric_limits<float>::lowest)()
				};
				const DirectX::XMMATRIX worldMatrix = transform->WorldTransform.ToXmMatrix();
				for (const DirectX::XMFLOAT3& localCorner : localCorners)
				{
					DirectX::XMFLOAT3 worldCorner = {};
					DirectX::XMStoreFloat3(
						&worldCorner,
						DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&localCorner), worldMatrix));
					worldMin.x = (std::min)(worldMin.x, worldCorner.x);
					worldMin.y = (std::min)(worldMin.y, worldCorner.y);
					worldMin.z = (std::min)(worldMin.z, worldCorner.z);
					worldMax.x = (std::max)(worldMax.x, worldCorner.x);
					worldMax.y = (std::max)(worldMax.y, worldCorner.y);
					worldMax.z = (std::max)(worldMax.z, worldCorner.z);
				}
				if (IntersectRayAabbPoint(rayOrigin, rayDirection, worldMin, worldMax, worldPosition))
				{
					return true;
				}
			}
			break;
		}
		case SceneMeasureTarget::Ground:
		default:
			break;
		}

		return IntersectRayPlane(
			rayOrigin,
			rayDirection,
			{ 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			worldPosition);
	}

	void EditorLayer::DrawGameView(EditorContext& context)
	{
		const float width = static_cast<float>((std::max)(context.ViewportWidth, 1));
		const float height = static_cast<float>((std::max)(context.ViewportHeight, 1));
		SetInitialWindowRect("Game", 276.0f + width * 0.39f, 32.0f, width * 0.30f, height * 0.58f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.025f, 0.02f));
		const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		const bool visible = ImGui::Begin("Game", nullptr, windowFlags);
		if (!visible)
		{
			m_GameViewport = {};
			ImGui::End();
			ImGui::PopStyleColor();
			return;
		}

		const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
		ImVec2 canvasSize = ImGui::GetContentRegionAvail();
		canvasSize.x = (std::max)(canvasSize.x, 64.0f);
		canvasSize.y = (std::max)(canvasSize.y, 64.0f);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRect(canvasPosition, ImVec2(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y), IM_COL32(120, 120, 130, 140));

		ImGui::InvisibleButton("GameCanvas", canvasSize);
		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		StoreViewportState(m_GameViewport, canvasPosition.x, canvasPosition.y, canvasSize.x, canvasSize.y, hovered, focused);
		AcceptModelDrop(context, AssetDropTarget::Game);

		std::string overlay = "Game | ";
		overlay.append(SampleModeName(context.SampleMode));
		drawList->AddText(ImVec2(canvasPosition.x + 10.0f, canvasPosition.y + 10.0f), IM_COL32(235, 235, 235, 220), overlay.c_str());
		drawList->AddText(ImVec2(canvasPosition.x + 10.0f, canvasPosition.y + 30.0f), IM_COL32(200, 200, 210, 210), RenderModeToString(context.CurrentRenderMode).data());
		const DirectX::XMFLOAT3 cameraPosition = context.GameCamera.GetPosition();
		const std::string cameraText = std::format("Camera {:.1f}, {:.1f}, {:.1f}", cameraPosition.x, cameraPosition.y, cameraPosition.z);
		drawList->AddText(ImVec2(canvasPosition.x + 10.0f, canvasPosition.y + 50.0f), IM_COL32(200, 200, 210, 210), cameraText.c_str());

		ImGui::End();
		ImGui::PopStyleColor();
	}

	void EditorLayer::DrawInspector(EditorContext& context)
	{
		const float width = static_cast<float>((std::max)(context.ViewportWidth, 1));
		SetInitialWindowRect("Inspector", width - 360.0f, 32.0f, 352.0f, 560.0f);
		ImGui::Begin("Inspector");

		if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
		{
			DirectX::XMFLOAT3 ambientColor = context.AmbientColor;
			if (ImGui::ColorEdit3("Ambient Color", &ambientColor.x) && context.OnAmbientColorChanged)
			{
				ambientColor.x = std::clamp(ambientColor.x, 0.0f, 4.0f);
				ambientColor.y = std::clamp(ambientColor.y, 0.0f, 4.0f);
				ambientColor.z = std::clamp(ambientColor.z, 0.0f, 4.0f);
				context.OnAmbientColorChanged(ambientColor);
			}

			float ambientIntensity = context.AmbientIntensity;
			if (ImGui::DragFloat("Ambient Intensity", &ambientIntensity, 0.01f, 0.0f, 2.0f, "%.2f") && context.OnAmbientIntensityChanged)
			{
				context.OnAmbientIntensityChanged(ambientIntensity);
			}

			float exposure = context.Exposure;
			if (ImGui::DragFloat("Exposure", &exposure, 0.01f, 0.05f, 8.0f, "%.2f") && context.OnExposureChanged)
			{
				context.OnExposureChanged(exposure);
			}

			ImGui::SeparatorText("Skybox");
			Rendering::SkyboxSettings skybox = context.Skybox;
			bool skyboxChanged = false;
			skyboxChanged |= ImGui::Checkbox("Skybox Enabled", &skybox.Enabled);
			if (ImGui::Button("Unity Default"))
			{
				skybox = Rendering::SkyboxSettings{};
				skyboxChanged = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Sunset"))
			{
				skybox.Enabled = true;
				skybox.ZenithColor = { 0.18f, 0.23f, 0.48f };
				skybox.HorizonColor = { 1.0f, 0.48f, 0.22f };
				skybox.GroundColor = { 0.12f, 0.10f, 0.16f };
				skybox.SunColor = { 1.0f, 0.62f, 0.24f };
				skybox.SunDirection = { -0.65f, 0.22f, -0.35f };
				skybox.Intensity = 1.12f;
				skybox.HorizonHeight = -0.08f;
				skybox.HorizonBlend = 1.9f;
				skybox.SunSize = 0.055f;
				skybox.SunIntensity = 2.4f;
				skyboxChanged = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Night"))
			{
				skybox.Enabled = true;
				skybox.ZenithColor = { 0.012f, 0.018f, 0.06f };
				skybox.HorizonColor = { 0.05f, 0.07f, 0.16f };
				skybox.GroundColor = { 0.01f, 0.012f, 0.018f };
				skybox.SunColor = { 0.38f, 0.48f, 0.85f };
				skybox.SunDirection = { 0.28f, 0.58f, -0.42f };
				skybox.Intensity = 0.65f;
				skybox.HorizonHeight = -0.03f;
				skybox.HorizonBlend = 2.4f;
				skybox.SunSize = 0.025f;
				skybox.SunIntensity = 0.35f;
				skyboxChanged = true;
			}
			skyboxChanged |= ImGui::ColorEdit3("Zenith Color", &skybox.ZenithColor.x);
			skyboxChanged |= ImGui::ColorEdit3("Horizon Color", &skybox.HorizonColor.x);
			skyboxChanged |= ImGui::ColorEdit3("Ground Color", &skybox.GroundColor.x);
			skyboxChanged |= ImGui::DragFloat("Sky Intensity", &skybox.Intensity, 0.01f, 0.0f, 8.0f, "%.2f");
			skyboxChanged |= ImGui::DragFloat("Horizon Height", &skybox.HorizonHeight, 0.005f, -0.95f, 0.95f, "%.2f");
			skyboxChanged |= ImGui::DragFloat("Horizon Blend", &skybox.HorizonBlend, 0.02f, 0.05f, 8.0f, "%.2f");
			skyboxChanged |= ImGui::ColorEdit3("Sun Color", &skybox.SunColor.x);
			skyboxChanged |= ImGui::DragFloat3("Sun Direction", &skybox.SunDirection.x, 0.01f, -1.0f, 1.0f, "%.2f");
			skyboxChanged |= ImGui::DragFloat("Sun Size", &skybox.SunSize, 0.001f, 0.001f, 0.35f, "%.3f");
			skyboxChanged |= ImGui::DragFloat("Sun Intensity", &skybox.SunIntensity, 0.02f, 0.0f, 16.0f, "%.2f");
			if (skyboxChanged && context.OnSkyboxSettingsChanged)
			{
				context.OnSkyboxSettingsChanged(Rendering::ClampSkyboxSettings(skybox));
			}

			float keyLightIntensity = context.KeyLightIntensity;
			if (ImGui::DragFloat("Key Light Intensity", &keyLightIntensity, 0.05f, 0.0f, 100.0f, "%.2f") && context.OnKeyLightIntensityChanged)
			{
				context.OnKeyLightIntensityChanged(keyLightIntensity);
			}

			Rendering::ShadowSettings shadowSettings = context.ShadowSettings;
			bool shadowSettingsChanged = false;
			shadowSettingsChanged |= ImGui::Checkbox("Shadows", &shadowSettings.Enabled);
			int shadowMapSize = shadowSettings.MapSize <= 512
				? 0
				: shadowSettings.MapSize <= 1024 ? 1 : shadowSettings.MapSize >= 4096 ? 3 : 2;
			if (ImGui::Combo("Shadow Map", &shadowMapSize, "512\0" "1024\0" "2048\0" "4096\0"))
			{
				switch (shadowMapSize)
				{
				case 0:
					shadowSettings.MapSize = 512;
					break;
				case 1:
					shadowSettings.MapSize = 1024;
					break;
				case 3:
					shadowSettings.MapSize = 4096;
					break;
				case 2:
				default:
					shadowSettings.MapSize = 2048;
					break;
				}
				shadowSettingsChanged = true;
			}
			shadowSettingsChanged |= ImGui::DragFloat("Shadow Distance", &shadowSettings.Distance, 1.0f, 1.0f, 10000.0f, "%.1f");
			shadowSettingsChanged |= ImGui::DragFloat("Shadow Ortho Size", &shadowSettings.OrthographicSize, 1.0f, 1.0f, 10000.0f, "%.1f");
			if (shadowSettingsChanged && context.OnShadowSettingsChanged)
			{
				context.OnShadowSettingsChanged(shadowSettings);
			}
			ImGui::Text(
				"Shadow Source: %s",
				context.ShadowStats.HasDirectionalCaster ? "Directional Light" : "None");
		}
		ImGui::Separator();

		const EntityId sceneSelectedEntity = context.ActiveScene.GetSelectedEntity();
		if (m_InspectorLocked &&
			(m_LockedInspectorEntity == InvalidEntityId || !context.ActiveScene.ContainsEntity(m_LockedInspectorEntity)))
		{
			m_InspectorLocked = false;
			m_LockedInspectorEntity = InvalidEntityId;
		}
		if (!m_InspectorLocked)
		{
			m_LockedInspectorEntity = sceneSelectedEntity;
		}

		bool inspectorLocked = m_InspectorLocked;
		if (ImGui::Checkbox("Lock", &inspectorLocked))
		{
			m_InspectorLocked = inspectorLocked;
			m_LockedInspectorEntity = inspectorLocked ? sceneSelectedEntity : InvalidEntityId;
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Keep showing this entity while selecting others.");
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##InspectorFilter",
			"Search components or properties...",
			m_InspectorFilter.data(),
			m_InspectorFilter.size());
		if (!m_InspectorPinnedComponents.empty())
		{
			ImGui::TextDisabled("%zu pinned component type%s", m_InspectorPinnedComponents.size(), m_InspectorPinnedComponents.size() == 1 ? "" : "s");
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				for (const SceneComponentKind pinnedKind : m_InspectorPinnedComponents)
				{
					ImGui::BulletText("%s", ComponentKindName(pinnedKind));
				}
				ImGui::EndTooltip();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear Pins"))
			{
				m_InspectorPinnedComponents.clear();
				SaveProjectState(context);
			}
		}
		if (context.ActiveSceneIsRuntimeClone)
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.08f, 0.18f, 0.32f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.10f, 0.22f, 0.38f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.10f, 0.22f, 0.38f, 1.0f));
			const bool runtimeNoticeOpen = ImGui::CollapsingHeader("Play Runtime Clone - Runtime Only", ImGuiTreeNodeFlags_DefaultOpen);
			ImGui::PopStyleColor(3);
			if (runtimeNoticeOpen)
			{
				ImGui::TextWrapped("Inspector changes edit only the Play runtime clone. They are discarded on Stop and never mark the edit scene dirty.");
				ImGui::TextWrapped("Runtime Transform, Material scalar, and component property edits support Ctrl+Z / Ctrl+Y while Play is active.");
				ImGui::BeginDisabled(!context.OnResetPlayRuntimeScene);
				if (ImGui::SmallButton("Reset Runtime Clone") && context.OnResetPlayRuntimeScene)
				{
					context.OnResetPlayRuntimeScene();
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Rebuild the runtime clone from the locked edit scene snapshot.");
				}
			}
		}

		const EntityId selectedEntity = m_InspectorLocked ? m_LockedInspectorEntity : sceneSelectedEntity;
		const std::string_view inspectorFilter = TextFilter(m_InspectorFilter);
		const auto inspectorSectionMatches = [inspectorFilter](std::string_view label, std::string_view keywords = {})
		{
			return inspectorFilter.empty() ||
				ContainsCaseInsensitive(label, inspectorFilter) ||
				(!keywords.empty() && ContainsCaseInsensitive(keywords, inspectorFilter));
		};
		const auto canPasteComponent = [this](SceneComponentKind kind)
		{
			return m_HasComponentClipboard &&
				m_ComponentClipboardKind == kind &&
				SnapshotHasComponent(kind, m_ComponentClipboardSnapshot);
		};
		if (m_InspectorLocked && selectedEntity != InvalidEntityId)
		{
			ImGui::TextDisabled("Locked Entity: %u", selectedEntity);
		}

		if (selectedEntity == InvalidEntityId)
		{
			ImGui::TextUnformatted("No entity selected");
			ImGui::End();
			return;
		}

		const std::string* entityName = context.ActiveScene.GetEntityName(selectedEntity);
		ImGui::Text("Entity: %s", entityName ? entityName->c_str() : "<unnamed>");
		ImGui::Text("ID: %u", selectedEntity);
		size_t prefabOverrideCount = 0;
		bool prefabNameOverridden = false;
		bool prefabTransformOverridden = false;
		std::vector<SceneComponentKind> prefabOverriddenKinds;
		ScenePersistence::LoadedSceneEntity prefabOverrideSource;
		std::filesystem::path prefabOverrideSourcePath;
		bool prefabOverrideSourceLoaded = false;
		const auto markPrefabComponentOverride = [&prefabOverrideCount, &prefabOverriddenKinds](SceneComponentKind kind)
		{
			++prefabOverrideCount;
			if (std::ranges::find(prefabOverriddenKinds, kind) == prefabOverriddenKinds.end())
			{
				prefabOverriddenKinds.push_back(kind);
			}
		};
		if (const PrefabInstanceComponent* prefab = context.ActiveScene.GetPrefabInstanceComponent(selectedEntity))
		{
			const bool prefabComponentEnabled = context.ActiveScene.IsComponentEnabled<PrefabInstanceComponent>(selectedEntity);
			if (!prefabComponentEnabled)
			{
				ImGui::TextDisabled("Prefab Instance: component disabled");
			}
			else if (prefab->PrefabPath.empty())
			{
				ImGui::TextDisabled("Prefab Instance: no prefab path assigned");
			}
			else if (!prefab->TrackPrefabOverrides)
			{
				ImGui::TextDisabled("Prefab Instance: override tracking disabled");
			}
			else if (!context.OnLoadPrefabForInspection)
			{
				ImGui::TextDisabled("Prefab Instance: override inspection unavailable");
			}
			else
			{
				std::string prefabLoadError;
				if (!context.OnLoadPrefabForInspection(prefab->PrefabPath, prefabOverrideSource, prefabLoadError))
				{
					ImGui::TextWrapped("Prefab Instance: source load failed (%s)", prefabLoadError.empty() ? "<unknown>" : prefabLoadError.c_str());
				}
				else
				{
					prefabOverrideSourceLoaded = true;
					prefabOverrideSourcePath = prefab->PrefabPath;
					const std::string currentNameText = entityName && !entityName->empty() ? *entityName : std::string("<unnamed>");
					prefabNameOverridden = currentNameText != prefabOverrideSource.Name;
					if (prefabNameOverridden)
					{
						++prefabOverrideCount;
					}
					const TransformComponent* currentTransform = context.ActiveScene.GetTransformComponent(selectedEntity);
					const bool currentHasTransform = currentTransform != nullptr;
					if (currentHasTransform != prefabOverrideSource.HasTransform ||
						(currentHasTransform && prefabOverrideSource.HasTransform && !TransformNearlyEqual(currentTransform->LocalTransform, prefabOverrideSource.Transform)))
					{
						prefabTransformOverridden = true;
						++prefabOverrideCount;
					}

					for (const Reflection::ComponentDescriptor& descriptor : Reflection::GetSceneComponentDescriptors())
					{
						ScenePersistence::LoadedSceneEntity currentSnapshot;
						const bool currentHas = CaptureComponentSnapshot(context.ActiveScene, selectedEntity, descriptor.Kind, currentSnapshot);
						const bool prefabHas = SnapshotHasComponent(descriptor.Kind, prefabOverrideSource);
						if (!currentHas && !prefabHas)
						{
							continue;
						}
						if (currentHas != prefabHas)
						{
							markPrefabComponentOverride(descriptor.Kind);
							continue;
						}

						bool currentEnabled = true;
						bool prefabEnabled = true;
						if (SnapshotComponentEnabled(descriptor.Kind, currentSnapshot, currentEnabled) &&
							SnapshotComponentEnabled(descriptor.Kind, prefabOverrideSource, prefabEnabled) &&
							currentEnabled != prefabEnabled)
						{
							markPrefabComponentOverride(descriptor.Kind);
						}

						for (const Reflection::PropertyDescriptor& property : descriptor.Properties)
						{
							const std::string currentValue = SnapshotPropertyValue(descriptor.Kind, property.Name, currentSnapshot);
							const std::string prefabValue = SnapshotPropertyValue(descriptor.Kind, property.Name, prefabOverrideSource);
							if (currentValue != prefabValue)
							{
								markPrefabComponentOverride(descriptor.Kind);
							}
						}
					}

					if (prefabOverrideCount == 0)
					{
						ImGui::TextColored(ImVec4(0.50f, 0.82f, 0.55f, 1.0f), "Prefab: no reflected overrides");
					}
					else
					{
						ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "Prefab Overrides: %zu", prefabOverrideCount);
						if (prefabNameOverridden)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("[Name]");
							if (ImGui::IsItemHovered())
							{
								ImGui::SetTooltip("Entity name differs from its prefab source.");
							}
							ImGui::SameLine();
							bool revertNameOverride = false;
							ImGui::BeginDisabled(!context.OnRenameEntity);
							revertNameOverride = ImGui::SmallButton("Revert Name##PrefabNameOverride");
							ImGui::EndDisabled();
							if (revertNameOverride)
							{
								context.OnRenameEntity(selectedEntity, prefabOverrideSource.Name);
								ImGui::End();
								return;
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip("Revert Entity name to the prefab source value.");
							}
							ImGui::SameLine();
							const bool canApplyNameOverride =
								!prefabOverrideSourcePath.empty() &&
								static_cast<bool>(context.OnSavePrefabInspectionRoot);
							bool applyNameOverride = false;
							ImGui::BeginDisabled(!canApplyNameOverride);
							applyNameOverride = ImGui::SmallButton("Apply Name##PrefabNameOverride");
							ImGui::EndDisabled();
							if (applyNameOverride)
							{
								ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabOverrideSource;
								updatedPrefabRoot.Name = currentNameText;
								static_cast<void>(context.OnSavePrefabInspectionRoot(prefabOverrideSourcePath, updatedPrefabRoot));
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip("Apply the current Entity name to the prefab source.");
							}
						}
						if (prefabTransformOverridden)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("[Transform]");
						}
						const size_t maxVisibleOverrideKinds = 4;
						const size_t visibleOverrideKindCount = (std::min)(prefabOverriddenKinds.size(), maxVisibleOverrideKinds);
						for (size_t kindIndex = 0; kindIndex < visibleOverrideKindCount; ++kindIndex)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("[%s]", ComponentKindName(prefabOverriddenKinds[kindIndex]));
						}
						if (prefabOverriddenKinds.size() > visibleOverrideKindCount)
						{
							ImGui::SameLine();
							ImGui::TextDisabled("[+%zu]", prefabOverriddenKinds.size() - visibleOverrideKindCount);
						}
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::SetTooltip("Detailed per-property prefab diff is available in the Prefab Overrides panel below.");
					}
				}
			}
		}
		const NestedSceneChildStatus nestedStatus = context.OnGetNestedSceneChildStatus
			? context.OnGetNestedSceneChildStatus(selectedEntity)
			: NestedSceneChildStatus{};
		if (nestedStatus.IsNestedSceneChild)
		{
			ImGui::TextColored(ImVec4(0.62f, 0.80f, 1.0f, 1.0f), "Nested Scene Child");
			ImGui::Text("SceneReference Owner: %u", nestedStatus.OwnerEntity);
			if (!nestedStatus.SourceScenePath.empty())
			{
				ImGui::TextWrapped("Source Scene: %s", nestedStatus.SourceScenePath.string().c_str());
			}
			ImGui::TextDisabled("Runtime-expanded child. It is excluded from scene save and should be persisted in the source scene.");

			const bool canSelectOwner = nestedStatus.OwnerEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(nestedStatus.OwnerEntity);
			bool selectOwner = false;
			ImGui::BeginDisabled(!canSelectOwner);
			selectOwner = ImGui::SmallButton("Select Owner");
			ImGui::EndDisabled();
			if (selectOwner)
			{
				m_HierarchySelection.clear();
				m_HierarchySelection.push_back(nestedStatus.OwnerEntity);
				m_LastHierarchyClickedEntity = nestedStatus.OwnerEntity;
				context.ActiveScene.SetSelectedEntity(nestedStatus.OwnerEntity);
				if (m_InspectorLocked)
				{
					m_LockedInspectorEntity = nestedStatus.OwnerEntity;
				}
				ImGui::End();
				return;
			}

			ImGui::SameLine();
			bool openSourceScene = false;
			ImGui::BeginDisabled(nestedStatus.SourceScenePath.empty() || !context.OnOpenScene);
			openSourceScene = ImGui::SmallButton("Open Source");
			ImGui::EndDisabled();
			if (openSourceScene)
			{
				context.OnOpenScene(nestedStatus.SourceScenePath);
				ImGui::End();
				return;
			}

			ImGui::SameLine();
			bool makeLocal = false;
			ImGui::BeginDisabled(!context.OnMakeNestedSceneChildLocal);
			makeLocal = ImGui::SmallButton("Make Local");
			ImGui::EndDisabled();
			if (makeLocal)
			{
				static_cast<void>(context.OnMakeNestedSceneChildLocal(selectedEntity));
				ImGui::End();
				return;
			}

			ImGui::SameLine();
			bool reloadSourceScene = false;
			ImGui::BeginDisabled(nestedStatus.OwnerEntity == InvalidEntityId || !context.OnLoadSceneReference);
			reloadSourceScene = ImGui::SmallButton("Reload Source");
			ImGui::EndDisabled();
			if (reloadSourceScene)
			{
				static_cast<void>(context.OnLoadSceneReference(nestedStatus.OwnerEntity));
				if (context.ActiveScene.ContainsEntity(nestedStatus.OwnerEntity))
				{
					m_HierarchySelection.clear();
					m_HierarchySelection.push_back(nestedStatus.OwnerEntity);
					m_LastHierarchyClickedEntity = nestedStatus.OwnerEntity;
					context.ActiveScene.SetSelectedEntity(nestedStatus.OwnerEntity);
					if (m_InspectorLocked)
					{
						m_LockedInspectorEntity = nestedStatus.OwnerEntity;
					}
				}
				ImGui::End();
				return;
			}

			ImGui::SameLine();
			bool unloadSourceScene = false;
			ImGui::BeginDisabled(nestedStatus.OwnerEntity == InvalidEntityId || !context.OnUnloadSceneReference);
			unloadSourceScene = ImGui::SmallButton("Unload Source");
			ImGui::EndDisabled();
			if (unloadSourceScene)
			{
				static_cast<void>(context.OnUnloadSceneReference(nestedStatus.OwnerEntity));
				if (context.ActiveScene.ContainsEntity(nestedStatus.OwnerEntity))
				{
					m_HierarchySelection.clear();
					m_HierarchySelection.push_back(nestedStatus.OwnerEntity);
					m_LastHierarchyClickedEntity = nestedStatus.OwnerEntity;
					context.ActiveScene.SetSelectedEntity(nestedStatus.OwnerEntity);
					if (m_InspectorLocked)
					{
						m_LockedInspectorEntity = nestedStatus.OwnerEntity;
					}
				}
				ImGui::End();
				return;
			}
		}

		std::vector<EntityId> inspectorSelection;
		if (!m_InspectorLocked)
		{
			std::erase_if(m_HierarchySelection, [&context](EntityId entityId)
				{
					return !context.ActiveScene.ContainsEntity(entityId);
				});
			for (EntityId entityId : m_HierarchySelection)
			{
				if (entityId != InvalidEntityId &&
					context.ActiveScene.ContainsEntity(entityId) &&
					std::ranges::find(inspectorSelection, entityId) == inspectorSelection.end())
				{
					inspectorSelection.push_back(entityId);
				}
			}
		}
		if (std::ranges::find(inspectorSelection, selectedEntity) == inspectorSelection.end())
		{
			inspectorSelection.push_back(selectedEntity);
		}
		const bool multiInspecting = inspectorSelection.size() > 1;
		if (multiInspecting)
		{
			ImGui::TextDisabled("%zu entities selected. Transform edits apply to all selected entities.", inspectorSelection.size());
		}
		ImGui::Separator();

		if (ImGui::Button("+ Add Component"))
		{
			ImGui::OpenPopup("AddComponentPopup");
		}
		if (ImGui::BeginPopup("AddComponentPopup"))
		{
			DrawAddComponentMenuItem<MeshComponent>(context, selectedEntity, SceneComponentKind::Mesh, ComponentKindName(SceneComponentKind::Mesh));
			DrawAddComponentMenuItem<CameraComponent>(context, selectedEntity, SceneComponentKind::Camera, ComponentKindName(SceneComponentKind::Camera));
			DrawAddComponentMenuItem<LightComponent>(context, selectedEntity, SceneComponentKind::Light, ComponentKindName(SceneComponentKind::Light));
			DrawAddComponentMenuItem<RigidBodyComponent>(context, selectedEntity, SceneComponentKind::RigidBody, ComponentKindName(SceneComponentKind::RigidBody));
			DrawAddComponentMenuItem<ColliderComponent>(context, selectedEntity, SceneComponentKind::Collider, ComponentKindName(SceneComponentKind::Collider));
			DrawAddComponentMenuItem<PhysicsMaterialComponent>(context, selectedEntity, SceneComponentKind::PhysicsMaterial, ComponentKindName(SceneComponentKind::PhysicsMaterial));
			ImGui::Separator();
			DrawAddComponentMenuItem<PrefabInstanceComponent>(context, selectedEntity, SceneComponentKind::PrefabInstance, ComponentKindName(SceneComponentKind::PrefabInstance));
			DrawAddComponentMenuItem<SceneReferenceComponent>(context, selectedEntity, SceneComponentKind::SceneReference, ComponentKindName(SceneComponentKind::SceneReference));
			DrawAddComponentMenuItem<ScriptComponent>(context, selectedEntity, SceneComponentKind::Script, ComponentKindName(SceneComponentKind::Script));
			DrawAddComponentMenuItem<Sprite2DComponent>(context, selectedEntity, SceneComponentKind::Sprite2D, ComponentKindName(SceneComponentKind::Sprite2D));
			DrawAddComponentMenuItem<UiElementComponent>(context, selectedEntity, SceneComponentKind::UiElement, ComponentKindName(SceneComponentKind::UiElement));
			DrawAddComponentMenuItem<AudioSourceComponent>(context, selectedEntity, SceneComponentKind::AudioSource, ComponentKindName(SceneComponentKind::AudioSource));
			DrawAddComponentMenuItem<NavigationAgentComponent>(context, selectedEntity, SceneComponentKind::NavigationAgent, ComponentKindName(SceneComponentKind::NavigationAgent));
			DrawAddComponentMenuItem<NetworkIdentityComponent>(context, selectedEntity, SceneComponentKind::NetworkIdentity, ComponentKindName(SceneComponentKind::NetworkIdentity));

			const Asset::StaticMeshAsset* meshAsset = context.ActiveScene.GetMeshAsset(selectedEntity);
			const bool canAddAnimator = meshAsset && meshAsset->IsAnimated && !meshAsset->Animations.empty();
			DrawAddComponentMenuItem<AnimatorComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Animator,
				ComponentKindName(SceneComponentKind::Animator),
				canAddAnimator,
				"Animator requires an animated mesh.");
			ImGui::EndPopup();
		}
		ImGui::Separator();

		std::erase_if(m_InspectorComponentOrders, [&context](const auto& entry)
			{
				return !context.ActiveScene.ContainsEntity(entry.first);
			});
		std::vector<SceneComponentKind>& inspectorComponentOrder = m_InspectorComponentOrders[selectedEntity];
		NormalizeInspectorComponentOrder(context.ActiveScene, selectedEntity, inspectorComponentOrder);
		NormalizeInspectorPinnedComponents(m_InspectorPinnedComponents);

		const auto isComponentPinned = [this](SceneComponentKind kind)
		{
			return std::ranges::find(m_InspectorPinnedComponents, kind) != m_InspectorPinnedComponents.end();
		};
		const auto toggleInspectorComponentPinned = [this, &context, &isComponentPinned](SceneComponentKind kind)
		{
			if (isComponentPinned(kind))
			{
				std::erase(m_InspectorPinnedComponents, kind);
			}
			else
			{
				m_InspectorPinnedComponents.push_back(kind);
				NormalizeInspectorPinnedComponents(m_InspectorPinnedComponents);
			}
			SaveProjectState(context);
		};

		std::vector<SceneComponentKind> inspectorDisplayOrder;
		inspectorDisplayOrder.reserve(inspectorComponentOrder.size());
		for (SceneComponentKind pinnedKind : m_InspectorPinnedComponents)
		{
			if (std::ranges::find(inspectorComponentOrder, pinnedKind) != inspectorComponentOrder.end() &&
				std::ranges::find(inspectorDisplayOrder, pinnedKind) == inspectorDisplayOrder.end())
			{
				inspectorDisplayOrder.push_back(pinnedKind);
			}
		}
		for (SceneComponentKind kind : inspectorComponentOrder)
		{
			if (std::ranges::find(inspectorDisplayOrder, kind) == inspectorDisplayOrder.end())
			{
				inspectorDisplayOrder.push_back(kind);
			}
		}

		const auto canMoveComponentUp = [&inspectorDisplayOrder, &isComponentPinned](SceneComponentKind kind)
		{
			if (isComponentPinned(kind))
			{
				return false;
			}
			const auto it = std::ranges::find(inspectorDisplayOrder, kind);
			return it != inspectorDisplayOrder.end() &&
				it != inspectorDisplayOrder.begin() &&
				!isComponentPinned(*std::prev(it));
		};
		const auto canMoveComponentDown = [&inspectorDisplayOrder, &isComponentPinned](SceneComponentKind kind)
		{
			if (isComponentPinned(kind))
			{
				return false;
			}
			const auto it = std::ranges::find(inspectorDisplayOrder, kind);
			return it != inspectorDisplayOrder.end() && std::next(it) != inspectorDisplayOrder.end();
		};
		const auto moveInspectorComponent = [&inspectorComponentOrder](SceneComponentKind kind, int direction) -> bool
		{
			const auto it = std::ranges::find(inspectorComponentOrder, kind);
			if (it == inspectorComponentOrder.end())
			{
				return false;
			}
			if (direction < 0 && it != inspectorComponentOrder.begin())
			{
				std::iter_swap(it, std::prev(it));
				return true;
			}
			else if (direction > 0 && std::next(it) != inspectorComponentOrder.end())
			{
				std::iter_swap(it, std::next(it));
				return true;
			}
			return false;
		};
		const auto componentHasPrefabOverride = [&prefabOverriddenKinds](SceneComponentKind kind)
		{
			return std::ranges::find(prefabOverriddenKinds, kind) != prefabOverriddenKinds.end();
		};
		const auto canUseComponentPrefabOverrideAction = [&](SceneComponentKind kind)
		{
			return prefabOverrideSourceLoaded &&
				componentHasPrefabOverride(kind) &&
				CanRevertPrefabOverrideComponent(kind) &&
				(static_cast<bool>(context.OnComponentPaste) || static_cast<bool>(context.OnComponentRemoved));
		};
		const auto canApplyComponentPrefabOverride = [&](SceneComponentKind kind)
		{
			return canUseComponentPrefabOverrideAction(kind) &&
				static_cast<bool>(context.OnSavePrefabInspectionRoot) &&
				!prefabOverrideSourcePath.empty();
		};
		const auto revertComponentPrefabOverride = [&](SceneComponentKind kind)
		{
			if (!canUseComponentPrefabOverrideAction(kind))
			{
				return;
			}
			if (SnapshotHasComponent(kind, prefabOverrideSource))
			{
				if (context.OnComponentPaste)
				{
					context.OnComponentPaste(selectedEntity, kind, prefabOverrideSource);
				}
			}
			else if (context.OnComponentRemoved)
			{
				context.OnComponentRemoved(selectedEntity, kind);
			}
		};
		const auto applyComponentPrefabOverride = [&](SceneComponentKind kind)
		{
			if (!canUseComponentPrefabOverrideAction(kind) ||
				!context.OnSavePrefabInspectionRoot ||
				prefabOverrideSourcePath.empty())
			{
				return;
			}

			ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabOverrideSource;
			ScenePersistence::LoadedSceneEntity currentSnapshot;
			const bool currentHas = CaptureComponentSnapshot(context.ActiveScene, selectedEntity, kind, currentSnapshot);
			const bool changed = currentHas
				? CopySnapshotComponentFromSource(kind, updatedPrefabRoot, currentSnapshot)
				: RemoveSnapshotComponent(kind, updatedPrefabRoot);
			if (changed)
			{
				static_cast<void>(context.OnSavePrefabInspectionRoot(prefabOverrideSourcePath, updatedPrefabRoot));
			}
		};
		const auto handleComponentSectionActions =
			[this, &context, selectedEntity, &moveInspectorComponent, &toggleInspectorComponentPinned, &revertComponentPrefabOverride, &applyComponentPrefabOverride](SceneComponentKind kind, const ComponentSectionState& section)
		{
			if (section.PinToggledRequested)
			{
				toggleInspectorComponentPinned(kind);
			}
			if (section.MoveUpRequested)
			{
				if (moveInspectorComponent(kind, -1))
				{
					SaveProjectState(context);
				}
			}
			if (section.MoveDownRequested)
			{
				if (moveInspectorComponent(kind, 1))
				{
					SaveProjectState(context);
				}
			}
			if (section.ResetRequested && context.OnComponentReset)
			{
				context.OnComponentReset(selectedEntity, kind);
			}
			if (section.CopyRequested)
			{
				if (CaptureComponentSnapshot(context.ActiveScene, selectedEntity, kind, m_ComponentClipboardSnapshot))
				{
					m_HasComponentClipboard = true;
					m_ComponentClipboardKind = kind;
				}
			}
			if (section.PasteRequested && context.OnComponentPaste)
			{
				context.OnComponentPaste(selectedEntity, kind, m_ComponentClipboardSnapshot);
			}
			if (section.PrefabRevertRequested)
			{
				revertComponentPrefabOverride(kind);
			}
			if (section.PrefabApplyRequested)
			{
				applyComponentPrefabOverride(kind);
			}
		};
		const auto multiComponentTargets = [&context, &inspectorSelection](SceneComponentKind kind)
		{
			std::vector<EntityId> targets;
			for (EntityId entityId : inspectorSelection)
			{
				if (HasInspectableComponent(context.ActiveScene, entityId, kind))
				{
					targets.push_back(entityId);
				}
			}
			return targets;
		};
		const auto componentEditTargets = [&](SceneComponentKind kind)
		{
			if (multiInspecting)
			{
				return multiComponentTargets(kind);
			}
			if (selectedEntity != InvalidEntityId && HasInspectableComponent(context.ActiveScene, selectedEntity, kind))
			{
				return std::vector<EntityId>{ selectedEntity };
			}
			return std::vector<EntityId>{};
		};
		const auto captureComponentEditRecords = [&](SceneComponentKind kind, const std::vector<EntityId>& targets)
		{
			std::vector<ComponentEditRecord> records;
			records.reserve(targets.size());
			for (EntityId entityId : targets)
			{
				ComponentEditRecord record;
				record.Entity = entityId;
				record.Kind = kind;
				if (CaptureComponentSnapshot(context.ActiveScene, entityId, kind, record.Before))
				{
					record.After = record.Before;
					records.push_back(std::move(record));
				}
			}
			return records;
		};
		const auto beginMultiComponentEdit = [&](SceneComponentKind kind, const std::vector<ComponentEditRecord>& beforeRecords)
		{
			if (m_MultiComponentEditing && m_MultiComponentEditingKind == kind)
			{
				return;
			}

			m_MultiComponentEditBefore = beforeRecords;
			m_MultiComponentEditing = !m_MultiComponentEditBefore.empty();
			m_MultiComponentEditingKind = kind;
		};
		const auto applyComponentSnapshotImmediate = [&](EntityId entityId, SceneComponentKind kind, const ScenePersistence::LoadedSceneEntity& snapshot)
		{
			switch (kind)
			{
			case SceneComponentKind::Camera:
				if (CameraComponent* targetCamera = context.ActiveScene.GetCameraComponent(entityId); targetCamera && snapshot.HasCamera)
				{
					const bool keepGameCameraRole = targetCamera->IsGameCamera;
					targetCamera->FovY = snapshot.Camera.FovY;
					targetCamera->NearZ = snapshot.Camera.NearZ;
					targetCamera->FarZ = snapshot.Camera.FarZ;
					targetCamera->IsGameCamera = keepGameCameraRole;
				}
				break;
			case SceneComponentKind::Light:
				if (LightComponent* targetLight = context.ActiveScene.GetLightComponent(entityId); targetLight && snapshot.HasLight)
				{
					*targetLight = snapshot.Light;
					static_cast<void>(context.ActiveScene.SetLightEnabled(entityId, snapshot.LightEnabled));
				}
				break;
			case SceneComponentKind::RigidBody:
				if (RigidBodyComponent* targetRigidBody = context.ActiveScene.GetRigidBodyComponent(entityId); targetRigidBody && snapshot.HasRigidBody)
				{
					*targetRigidBody = snapshot.RigidBody;
					static_cast<void>(context.ActiveScene.SetRigidBodyEnabled(entityId, snapshot.RigidBodyEnabled));
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(entityId);
					}
				}
				break;
			case SceneComponentKind::Collider:
				if (ColliderComponent* targetCollider = context.ActiveScene.GetColliderComponent(entityId); targetCollider && snapshot.HasCollider)
				{
					*targetCollider = snapshot.Collider;
					static_cast<void>(context.ActiveScene.SetColliderEnabled(entityId, snapshot.ColliderEnabled));
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(entityId);
					}
				}
				break;
			case SceneComponentKind::PhysicsMaterial:
				if (PhysicsMaterialComponent* targetPhysicsMaterial = context.ActiveScene.GetPhysicsMaterialComponent(entityId); targetPhysicsMaterial && snapshot.HasPhysicsMaterial)
				{
					*targetPhysicsMaterial = snapshot.PhysicsMaterial;
					static_cast<void>(context.ActiveScene.SetPhysicsMaterialEnabled(entityId, snapshot.PhysicsMaterialEnabled));
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(entityId);
					}
				}
				break;
			case SceneComponentKind::NavigationAgent:
				if (NavigationAgentComponent* targetNavigationAgent = context.ActiveScene.GetNavigationAgentComponent(entityId); targetNavigationAgent && snapshot.HasNavigationAgent)
				{
					*targetNavigationAgent = snapshot.NavigationAgent;
					static_cast<void>(context.ActiveScene.SetComponentEnabled<NavigationAgentComponent>(entityId, snapshot.NavigationAgentEnabled));
				}
				break;
			case SceneComponentKind::Sprite2D:
				if (Sprite2DComponent* targetSprite = context.ActiveScene.GetSprite2DComponent(entityId); targetSprite && snapshot.HasSprite2D)
				{
					const std::filesystem::path keepTexturePath = targetSprite->TexturePath;
					targetSprite->Color = snapshot.Sprite2D.Color;
					targetSprite->Size = snapshot.Sprite2D.Size;
					targetSprite->Pivot = snapshot.Sprite2D.Pivot;
					targetSprite->SortingLayer = snapshot.Sprite2D.SortingLayer;
					targetSprite->OrderInLayer = snapshot.Sprite2D.OrderInLayer;
					targetSprite->TexturePath = keepTexturePath;
				}
				break;
			case SceneComponentKind::AudioSource:
				if (AudioSourceComponent* targetAudio = context.ActiveScene.GetAudioSourceComponent(entityId); targetAudio && snapshot.HasAudioSource)
				{
					const std::filesystem::path keepClipPath = targetAudio->ClipPath;
					targetAudio->Volume = snapshot.AudioSource.Volume;
					targetAudio->Pitch = snapshot.AudioSource.Pitch;
					targetAudio->Loop = snapshot.AudioSource.Loop;
					targetAudio->PlayOnStart = snapshot.AudioSource.PlayOnStart;
					targetAudio->Spatialize = snapshot.AudioSource.Spatialize;
					targetAudio->MinDistance = snapshot.AudioSource.MinDistance;
					targetAudio->MaxDistance = snapshot.AudioSource.MaxDistance;
					targetAudio->ClipPath = keepClipPath;
				}
				break;
			case SceneComponentKind::UiElement:
				if (UiElementComponent* targetUi = context.ActiveScene.GetUiElementComponent(entityId); targetUi && snapshot.HasUiElement)
				{
					const std::string keepText = targetUi->Text;
					targetUi->Kind = snapshot.UiElement.Kind;
					targetUi->AnchorMin = snapshot.UiElement.AnchorMin;
					targetUi->AnchorMax = snapshot.UiElement.AnchorMax;
					targetUi->Position = snapshot.UiElement.Position;
					targetUi->Size = snapshot.UiElement.Size;
					targetUi->Color = snapshot.UiElement.Color;
					targetUi->Text = keepText;
				}
				break;
			case SceneComponentKind::NetworkIdentity:
				if (NetworkIdentityComponent* targetNetwork = context.ActiveScene.GetNetworkIdentityComponent(entityId); targetNetwork && snapshot.HasNetworkIdentity)
				{
					targetNetwork->ReplicateTransform = snapshot.NetworkIdentity.ReplicateTransform;
					targetNetwork->ServerAuthoritative = snapshot.NetworkIdentity.ServerAuthoritative;
				}
				break;
			case SceneComponentKind::Script:
				if (ScriptComponent* targetScript = context.ActiveScene.GetScriptComponent(entityId); targetScript && snapshot.HasScript)
				{
					const std::filesystem::path keepScriptPath = targetScript->ScriptPath;
					targetScript->Language = snapshot.Script.Language;
					targetScript->ClassName = snapshot.Script.ClassName;
					targetScript->RunInEditor = snapshot.Script.RunInEditor;
					targetScript->ScriptPath = keepScriptPath;
				}
				break;
			case SceneComponentKind::PrefabInstance:
				if (PrefabInstanceComponent* targetPrefab = context.ActiveScene.GetPrefabInstanceComponent(entityId); targetPrefab && snapshot.HasPrefabInstance)
				{
					const std::filesystem::path keepPrefabPath = targetPrefab->PrefabPath;
					const std::string keepSourceName = targetPrefab->SourceName;
					targetPrefab->TrackPrefabOverrides = snapshot.PrefabInstance.TrackPrefabOverrides;
					targetPrefab->PrefabPath = keepPrefabPath;
					targetPrefab->SourceName = keepSourceName;
				}
				break;
			case SceneComponentKind::SceneReference:
				if (SceneReferenceComponent* targetSceneReference = context.ActiveScene.GetSceneReferenceComponent(entityId); targetSceneReference && snapshot.HasSceneReference)
				{
					const std::filesystem::path keepScenePath = targetSceneReference->ScenePath;
					targetSceneReference->LoadAdditively = snapshot.SceneReference.LoadAdditively;
					targetSceneReference->AutoLoad = snapshot.SceneReference.AutoLoad;
					targetSceneReference->ScenePath = keepScenePath;
				}
				break;
			default:
				break;
			}
		};
		const auto applyPrimaryComponentToTargets = [&](SceneComponentKind kind, const std::vector<EntityId>& targets)
		{
			ScenePersistence::LoadedSceneEntity sourceSnapshot;
			if (!CaptureComponentSnapshot(context.ActiveScene, selectedEntity, kind, sourceSnapshot))
			{
				return;
			}

			for (EntityId entityId : targets)
			{
				applyComponentSnapshotImmediate(entityId, kind, sourceSnapshot);
			}
			if (context.OnSceneEdited)
			{
				context.OnSceneEdited();
			}
		};
		const auto commitMultiComponentEdit = [&](SceneComponentKind kind, const std::vector<EntityId>& targets)
		{
			if (!m_MultiComponentEditing || m_MultiComponentEditingKind != kind)
			{
				return;
			}

			std::vector<ComponentEditRecord> records;
			records.reserve(m_MultiComponentEditBefore.size());
			for (ComponentEditRecord record : m_MultiComponentEditBefore)
			{
				if (std::ranges::find(targets, record.Entity) == targets.end())
				{
					continue;
				}
				if (CaptureComponentSnapshot(context.ActiveScene, record.Entity, kind, record.After))
				{
					records.push_back(std::move(record));
				}
			}
			if (!records.empty() && context.OnComponentBatchEditCommitted)
			{
				context.OnComponentBatchEditCommitted(std::move(records));
			}
			m_MultiComponentEditBefore.clear();
			m_MultiComponentEditing = false;
			m_MultiComponentEditingKind = SceneComponentKind::Mesh;
		};
		const auto trackMultiComponentControl = [&](
			SceneComponentKind kind,
			const std::vector<EntityId>& targets,
			const std::vector<ComponentEditRecord>& beforeRecords,
			bool changed)
		{
			if (targets.empty())
			{
				return;
			}
			const bool activated = ImGui::IsItemActivated();
			const bool committed = ImGui::IsItemDeactivatedAfterEdit();
			if (activated)
			{
				beginMultiComponentEdit(kind, beforeRecords);
			}
			if (changed)
			{
				beginMultiComponentEdit(kind, beforeRecords);
				if (targets.size() > 1)
				{
					applyPrimaryComponentToTargets(kind, targets);
				}
			}
			if (committed || (changed && !ImGui::IsAnyItemActive()))
			{
				commitMultiComponentEdit(kind, targets);
			}
		};

		if (TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity);
			transform && inspectorSectionMatches("Transform", "Position Rotation Scale Translation"))
		{
			const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
			if (prefabTransformOverridden)
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "[Override]");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Transform differs from its prefab source.");
				}
				ImGui::SameLine();
				const bool canRevertTransformOverride =
					prefabOverrideSourceLoaded &&
					prefabOverrideSource.HasTransform &&
					static_cast<bool>(context.OnTransformEditCommitted);
				bool revertTransformOverride = false;
				ImGui::BeginDisabled(!canRevertTransformOverride);
				revertTransformOverride = ImGui::SmallButton("Revert##TransformPrefabOverride");
				ImGui::EndDisabled();
				if (revertTransformOverride)
				{
					context.OnTransformEditCommitted(selectedEntity, transform->LocalTransform, prefabOverrideSource.Transform);
					ImGui::End();
					return;
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Revert Transform to the prefab source value.");
				}
				ImGui::SameLine();
				const bool canApplyTransformOverride =
					prefabOverrideSourceLoaded &&
					!prefabOverrideSourcePath.empty() &&
					static_cast<bool>(context.OnSavePrefabInspectionRoot);
				bool applyTransformOverride = false;
				ImGui::BeginDisabled(!canApplyTransformOverride);
				applyTransformOverride = ImGui::SmallButton("Apply##TransformPrefabOverride");
				ImGui::EndDisabled();
				if (applyTransformOverride)
				{
					ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabOverrideSource;
					updatedPrefabRoot.HasTransform = true;
					updatedPrefabRoot.Transform = transform->LocalTransform;
					static_cast<void>(context.OnSavePrefabInspectionRoot(prefabOverrideSourcePath, updatedPrefabRoot));
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Apply the current Transform to the prefab source.");
				}
			}
			if (transformOpen)
			{
				if (multiInspecting)
				{
					const auto captureMultiTransformBefore = [&]()
					{
						if (m_MultiTransformEditing)
						{
							return;
						}

						m_MultiTransformEditBefore.clear();
						for (EntityId entityId : inspectorSelection)
						{
							if (const TransformComponent* selectedTransform = context.ActiveScene.GetTransformComponent(entityId))
							{
								m_MultiTransformEditBefore.push_back(TransformEditRecord{
									.Entity = entityId,
									.Before = selectedTransform->LocalTransform,
									.After = selectedTransform->LocalTransform
								});
							}
						}
						m_MultiTransformEditing = !m_MultiTransformEditBefore.empty();
					};
					const auto applyMultiTransformChange = [&](const Math::Transform& primaryBefore, const Math::Transform& primaryAfter, bool positionChanged, bool rotationChanged, bool scaleChanged)
					{
						const DirectX::XMFLOAT3 translationDelta = {
							primaryAfter.Translation.x - primaryBefore.Translation.x,
							primaryAfter.Translation.y - primaryBefore.Translation.y,
							primaryAfter.Translation.z - primaryBefore.Translation.z
						};
						const DirectX::XMFLOAT3 scaleRatio = {
							std::fabs(primaryBefore.Scale.x) > 1.0e-5f ? primaryAfter.Scale.x / primaryBefore.Scale.x : 1.0f,
							std::fabs(primaryBefore.Scale.y) > 1.0e-5f ? primaryAfter.Scale.y / primaryBefore.Scale.y : 1.0f,
							std::fabs(primaryBefore.Scale.z) > 1.0e-5f ? primaryAfter.Scale.z / primaryBefore.Scale.z : 1.0f
						};

						for (EntityId entityId : inspectorSelection)
						{
							TransformComponent* selectedTransform = context.ActiveScene.GetTransformComponent(entityId);
							if (!selectedTransform)
							{
								continue;
							}
							if (positionChanged)
							{
								selectedTransform->LocalTransform.Translation.x += translationDelta.x;
								selectedTransform->LocalTransform.Translation.y += translationDelta.y;
								selectedTransform->LocalTransform.Translation.z += translationDelta.z;
							}
							if (rotationChanged)
							{
								selectedTransform->LocalTransform.Rotation = primaryAfter.Rotation;
							}
							if (scaleChanged)
							{
								selectedTransform->LocalTransform.Scale.x *= scaleRatio.x;
								selectedTransform->LocalTransform.Scale.y *= scaleRatio.y;
								selectedTransform->LocalTransform.Scale.z *= scaleRatio.z;
							}
							selectedTransform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(selectedTransform->LocalTransform.Rotation);
							selectedTransform->UpdateWorld();
							if (context.OnPhysicsActorDirty)
							{
								context.OnPhysicsActorDirty(entityId);
							}
						}
						if (context.OnSceneEdited)
						{
							context.OnSceneEdited();
						}
					};
					const auto commitMultiTransformEdit = [&]()
					{
						if (!m_MultiTransformEditing)
						{
							return;
						}

						std::vector<TransformEditRecord> records;
						records.reserve(m_MultiTransformEditBefore.size());
						for (TransformEditRecord record : m_MultiTransformEditBefore)
						{
							if (const TransformComponent* selectedTransform = context.ActiveScene.GetTransformComponent(record.Entity))
							{
								record.After = selectedTransform->LocalTransform;
								if (!TransformNearlyEqual(record.Before, record.After))
								{
									records.push_back(record);
								}
							}
						}
						if (!records.empty() && context.OnTransformBatchEditCommitted)
						{
							context.OnTransformBatchEditCommitted(std::move(records));
						}
						m_MultiTransformEditBefore.clear();
						m_MultiTransformEditing = false;
					};

					const Math::Transform primaryBeforeControls = transform->LocalTransform;
					Math::Transform primaryAfterControls = primaryBeforeControls;
					bool positionChanged = false;
					bool rotationChanged = false;
					bool scaleChanged = false;
					positionChanged = ImGui::DragFloat3("Primary Position", &primaryAfterControls.Translation.x, 0.05f);
					const bool positionActivated = ImGui::IsItemActivated();
					const bool positionCommitted = ImGui::IsItemDeactivatedAfterEdit();
					rotationChanged = ImGui::DragFloat4("Shared Rotation", &primaryAfterControls.Rotation.x, 0.01f, -1.0f, 1.0f);
					const bool rotationActivated = ImGui::IsItemActivated();
					const bool rotationCommitted = ImGui::IsItemDeactivatedAfterEdit();
					scaleChanged = ImGui::DragFloat3("Relative Scale", &primaryAfterControls.Scale.x, 0.01f, 0.01f, 100.0f);
					const bool scaleActivated = ImGui::IsItemActivated();
					const bool scaleCommitted = ImGui::IsItemDeactivatedAfterEdit();

					if (positionActivated || rotationActivated || scaleActivated)
					{
						captureMultiTransformBefore();
					}
					if (positionChanged || rotationChanged || scaleChanged)
					{
						primaryAfterControls.Rotation = Math::NormalizeQuaternionOrIdentity(primaryAfterControls.Rotation);
						applyMultiTransformChange(primaryBeforeControls, primaryAfterControls, positionChanged, rotationChanged, scaleChanged);
					}
					if (positionCommitted || rotationCommitted || scaleCommitted)
					{
						commitMultiTransformEdit();
					}

					ImGui::TextDisabled("Primary Position moves all selected entities by delta.");
					ImGui::TextDisabled("Shared Rotation sets all selected entities to the same rotation.");
					ImGui::TextDisabled("Relative Scale scales all selected entities by the primary scale ratio.");
				}
				else
				{
				const Math::Transform transformBeforeControls = transform->LocalTransform;
				bool transformChanged = false;
				transformChanged |= ImGui::DragFloat3("Position", &transform->LocalTransform.Translation.x, 0.05f);
				const bool positionActivated = ImGui::IsItemActivated();
				const bool positionCommitted = ImGui::IsItemDeactivatedAfterEdit();
				transformChanged |= ImGui::DragFloat4("Rotation", &transform->LocalTransform.Rotation.x, 0.01f, -1.0f, 1.0f);
				const bool rotationActivated = ImGui::IsItemActivated();
				const bool rotationCommitted = ImGui::IsItemDeactivatedAfterEdit();
				transformChanged |= ImGui::DragFloat3("Scale", &transform->LocalTransform.Scale.x, 0.01f, 0.01f, 100.0f);
				const bool scaleActivated = ImGui::IsItemActivated();
				const bool scaleCommitted = ImGui::IsItemDeactivatedAfterEdit();
				if ((positionActivated || rotationActivated || scaleActivated) && m_TransformEditingEntity != selectedEntity)
				{
					m_TransformEditingEntity = selectedEntity;
					m_TransformEditBefore = transformBeforeControls;
				}
				if (transformChanged)
				{
					transform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(transform->LocalTransform.Rotation);
					transform->UpdateWorld();
					if (context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(selectedEntity);
					}
				}
				if ((positionCommitted || rotationCommitted || scaleCommitted) && m_TransformEditingEntity == selectedEntity)
				{
					if (context.OnTransformEditCommitted)
					{
						context.OnTransformEditCommitted(selectedEntity, m_TransformEditBefore, transform->LocalTransform);
					}
					m_TransformEditingEntity = InvalidEntityId;
					m_TransformEditBefore = Math::Transform::Identity();
				}
				}
			}
		}

		for (SceneComponentKind inspectorComponentKind : inspectorDisplayOrder)
		{
		if (CameraComponent* camera = context.ActiveScene.GetCameraComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Camera &&
			camera && inspectorSectionMatches("Camera", "FOV Near Far Game Camera Runtime Aspect"))
		{
			const ComponentSectionState section = BeginComponentSection<CameraComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Camera,
				"Camera",
				canPasteComponent(SceneComponentKind::Camera),
				CanResetComponentKind(SceneComponentKind::Camera),
				canMoveComponentUp(SceneComponentKind::Camera),
				canMoveComponentDown(SceneComponentKind::Camera),
				componentHasPrefabOverride(SceneComponentKind::Camera),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Camera),
				canApplyComponentPrefabOverride(SceneComponentKind::Camera),
				isComponentPinned(SceneComponentKind::Camera));
			handleComponentSectionActions(SceneComponentKind::Camera, section);
			if (section.Open && !section.Removed)
			{
				{
					ImGui::BeginDisabled(!section.Enabled);
					const std::vector<EntityId> cameraTargets = componentEditTargets(SceneComponentKind::Camera);
					const std::vector<ComponentEditRecord> cameraBeforeRecords = !cameraTargets.empty()
						? captureComponentEditRecords(SceneComponentKind::Camera, cameraTargets)
						: std::vector<ComponentEditRecord>{};
					if (cameraTargets.size() > 1)
					{
						ImGui::TextDisabled("Editing %zu Camera components.", cameraTargets.size());
						ImGui::TextDisabled("FOV/Near/Far are shared. Game Camera role stays per Entity.");
					}
					bool cameraChanged = false;
					float fovDegrees = DirectX::XMConvertToDegrees(camera->FovY);
					if (ImGui::DragFloat("FOV", &fovDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
					{
						camera->FovY = DirectX::XMConvertToRadians((std::clamp)(fovDegrees, 1.0f, 179.0f));
						cameraChanged = true;
						trackMultiComponentControl(SceneComponentKind::Camera, cameraTargets, cameraBeforeRecords, true);
					}
					bool controlChanged = ImGui::DragFloat("Near", &camera->NearZ, 0.01f, 0.001f, 100.0f, "%.3f");
					cameraChanged |= controlChanged;
					trackMultiComponentControl(SceneComponentKind::Camera, cameraTargets, cameraBeforeRecords, controlChanged);
					controlChanged = ImGui::DragFloat("Far", &camera->FarZ, 1.0f, 1.0f, 100000.0f, "%.1f");
					cameraChanged |= controlChanged;
					trackMultiComponentControl(SceneComponentKind::Camera, cameraTargets, cameraBeforeRecords, controlChanged);
					ImGui::Text("Role: %s", camera->IsGameCamera ? "Game Camera" : "Camera");

					const DirectX::XMFLOAT3 position = context.GameCamera.GetPosition();
					ImGui::Text("Runtime Position: %.2f, %.2f, %.2f", position.x, position.y, position.z);
					ImGui::Text("Runtime Aspect: %.3f", context.GameCamera.GetAspect());
					if (cameraChanged && context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					ImGui::EndDisabled();
				}
				ImGui::TreePop();
			}
		}

		if (LightComponent* light = context.ActiveScene.GetLightComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Light &&
			light && inspectorSectionMatches("Light", "Emits Type Color Intensity Range Shadow Spot"))
		{
			const ComponentSectionState section = BeginComponentSection<LightComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Light,
				"Light",
				canPasteComponent(SceneComponentKind::Light),
				CanResetComponentKind(SceneComponentKind::Light),
				canMoveComponentUp(SceneComponentKind::Light),
				canMoveComponentDown(SceneComponentKind::Light),
				componentHasPrefabOverride(SceneComponentKind::Light),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Light),
				canApplyComponentPrefabOverride(SceneComponentKind::Light),
				isComponentPinned(SceneComponentKind::Light));
			handleComponentSectionActions(SceneComponentKind::Light, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> lightTargets = componentEditTargets(SceneComponentKind::Light);
				const std::vector<ComponentEditRecord> lightBeforeRecords = !lightTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::Light, lightTargets)
					: std::vector<ComponentEditRecord>{};
				if (lightTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Light components.", lightTargets.size());
				}
				bool lightChanged = false;
				bool controlChanged = ImGui::Checkbox("Emits Light", &light->Enabled);
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				int typeIndex = std::to_underlying(light->Type);
				if (ImGui::Combo("Type", &typeIndex, "Directional\0Point\0Spot\0"))
				{
					light->Type = static_cast<LightType>((std::clamp)(typeIndex, 0, 2));
					lightChanged = true;
					trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, true);
				}
				controlChanged = ImGui::ColorEdit3("Color", &light->Color.x);
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Intensity", &light->Intensity, 0.05f, 0.0f, 100.0f);
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Range", &light->Range, 1.0f, 0.0f, 10000.0f);
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Cast Shadows", &light->CastShadows);
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Shadow Bias", &light->ShadowBias, 0.0001f, 0.0f, 0.1f, "%.5f");
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Shadow Normal Bias", &light->ShadowNormalBias, 0.001f, 0.0f, 1.0f, "%.4f");
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Shadow Strength", &light->ShadowStrength, 0.01f, 0.0f, 1.0f, "%.2f");
				lightChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, controlChanged);

				float spotAngleDegrees = DirectX::XMConvertToDegrees(light->SpotAngle);
				if (ImGui::DragFloat("Spot Angle", &spotAngleDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
				{
					light->SpotAngle = DirectX::XMConvertToRadians((std::clamp)(spotAngleDegrees, 1.0f, 179.0f));
					lightChanged = true;
					trackMultiComponentControl(SceneComponentKind::Light, lightTargets, lightBeforeRecords, true);
				}
				ImGui::Text("Resolved Type: %s", LightTypeName(light->Type));
				if (lightChanged && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (RigidBodyComponent* rigidBody = context.ActiveScene.GetRigidBodyComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::RigidBody &&
			rigidBody && inspectorSectionMatches("Rigidbody", "Physics Body Type Mass Damping Gravity Velocity Dynamic Kinematic Static"))
		{
			const ComponentSectionState section = BeginComponentSection<RigidBodyComponent>(
				context,
				selectedEntity,
				SceneComponentKind::RigidBody,
				"Rigidbody",
				canPasteComponent(SceneComponentKind::RigidBody),
				CanResetComponentKind(SceneComponentKind::RigidBody),
				canMoveComponentUp(SceneComponentKind::RigidBody),
				canMoveComponentDown(SceneComponentKind::RigidBody),
				componentHasPrefabOverride(SceneComponentKind::RigidBody),
				canUseComponentPrefabOverrideAction(SceneComponentKind::RigidBody),
				canApplyComponentPrefabOverride(SceneComponentKind::RigidBody),
				isComponentPinned(SceneComponentKind::RigidBody));
			handleComponentSectionActions(SceneComponentKind::RigidBody, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> rigidBodyTargets = componentEditTargets(SceneComponentKind::RigidBody);
				const std::vector<ComponentEditRecord> rigidBodyBeforeRecords = !rigidBodyTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::RigidBody, rigidBodyTargets)
					: std::vector<ComponentEditRecord>{};
				if (rigidBodyTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Rigidbody components.", rigidBodyTargets.size());
				}
				bool physicsChanged = false;
				const bool colliderEnabled = context.ActiveScene.IsColliderEnabled(selectedEntity);
				ImGui::Text("State: %s", Physics::ToString(rigidBody->Type).data());
				ImGui::Text("Simulation: %s", context.PhysicsSimulationEnabled ? "On" : "Off");
				ImGui::Text("Gravity: %s", rigidBody->UseGravity && rigidBody->Type == Physics::RigidBodyType::Dynamic ? "Enabled" : "Inactive");
				if (!context.ActiveScene.GetColliderComponent(selectedEntity) || !colliderEnabled)
				{
					ImGui::TextUnformatted("No enabled collider: actor will not be dynamic.");
				}
				int bodyTypeIndex = static_cast<int>(Physics::ToIndex(rigidBody->Type));
				if (ImGui::Combo("Body Type", &bodyTypeIndex, "Static\0Dynamic\0Kinematic\0"))
				{
					rigidBody->Type = static_cast<Physics::RigidBodyType>((std::clamp)(bodyTypeIndex, 0, 2));
					physicsChanged = true;
					trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, true);
				}
				bool controlChanged = ImGui::DragFloat("Mass", &rigidBody->Mass, 0.05f, 0.001f, 10000.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Linear Damping", &rigidBody->LinearDamping, 0.01f, 0.0f, 100.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Angular Damping", &rigidBody->AngularDamping, 0.01f, 0.0f, 100.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Use Gravity", &rigidBody->UseGravity);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat3("Linear Velocity", &rigidBody->LinearVelocity.x, 0.05f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat3("Angular Velocity", &rigidBody->AngularVelocity.x, 0.05f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::RigidBody, rigidBodyTargets, rigidBodyBeforeRecords, controlChanged);
				if (physicsChanged)
				{
					if (context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(selectedEntity);
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (ColliderComponent* collider = context.ActiveScene.GetColliderComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Collider &&
			collider && inspectorSectionMatches("Collider", "Physics Shape Box Sphere Capsule Plane Size Radius Height Offset Trigger"))
		{
			const ComponentSectionState section = BeginComponentSection<ColliderComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Collider,
				"Collider",
				canPasteComponent(SceneComponentKind::Collider),
				CanResetComponentKind(SceneComponentKind::Collider),
				canMoveComponentUp(SceneComponentKind::Collider),
				canMoveComponentDown(SceneComponentKind::Collider),
				componentHasPrefabOverride(SceneComponentKind::Collider),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Collider),
				canApplyComponentPrefabOverride(SceneComponentKind::Collider),
				isComponentPinned(SceneComponentKind::Collider));
			handleComponentSectionActions(SceneComponentKind::Collider, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> colliderTargets = componentEditTargets(SceneComponentKind::Collider);
				const std::vector<ComponentEditRecord> colliderBeforeRecords = !colliderTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::Collider, colliderTargets)
					: std::vector<ComponentEditRecord>{};
				if (colliderTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Collider components.", colliderTargets.size());
				}
				bool physicsChanged = false;
				ImGui::Text("Simulation: %s", context.PhysicsSimulationEnabled ? "On" : "Off");
				if (!context.ActiveScene.GetRigidBodyComponent(selectedEntity) || !context.ActiveScene.IsRigidBodyEnabled(selectedEntity))
				{
					ImGui::TextUnformatted("No enabled Rigidbody: collider behaves as static.");
				}
				if (collider->Shape == Physics::ColliderShape::Plane)
				{
					ImGui::TextUnformatted("Plane collider: infinite PhysX plane.");
				}
				int shapeIndex = static_cast<int>(Physics::ToIndex(collider->Shape));
				if (ImGui::Combo("Shape", &shapeIndex, "Box\0Sphere\0Capsule\0Plane\0"))
				{
					collider->Shape = static_cast<Physics::ColliderShape>((std::clamp)(shapeIndex, 0, 3));
					physicsChanged = true;
					trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, true);
				}
				bool controlChanged = ImGui::DragFloat3("Collider Size", &collider->Size.x, 0.05f, 0.001f, 10000.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Radius", &collider->Radius, 0.01f, 0.001f, 10000.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Height", &collider->Height, 0.01f, 0.001f, 10000.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat3("Offset", &collider->Offset.x, 0.01f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Trigger", &collider->IsTrigger);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Collider, colliderTargets, colliderBeforeRecords, controlChanged);
				if (physicsChanged)
				{
					if (context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(selectedEntity);
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (PhysicsMaterialComponent* physicsMaterial = context.ActiveScene.GetPhysicsMaterialComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::PhysicsMaterial &&
			physicsMaterial && inspectorSectionMatches("Physics Material", "Friction Restitution Collider"))
		{
			const ComponentSectionState section = BeginComponentSection<PhysicsMaterialComponent>(
				context,
				selectedEntity,
				SceneComponentKind::PhysicsMaterial,
				"Physics Material",
				canPasteComponent(SceneComponentKind::PhysicsMaterial),
				CanResetComponentKind(SceneComponentKind::PhysicsMaterial),
				canMoveComponentUp(SceneComponentKind::PhysicsMaterial),
				canMoveComponentDown(SceneComponentKind::PhysicsMaterial),
				componentHasPrefabOverride(SceneComponentKind::PhysicsMaterial),
				canUseComponentPrefabOverrideAction(SceneComponentKind::PhysicsMaterial),
				canApplyComponentPrefabOverride(SceneComponentKind::PhysicsMaterial),
				isComponentPinned(SceneComponentKind::PhysicsMaterial));
			handleComponentSectionActions(SceneComponentKind::PhysicsMaterial, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> physicsMaterialTargets = componentEditTargets(SceneComponentKind::PhysicsMaterial);
				const std::vector<ComponentEditRecord> physicsMaterialBeforeRecords = !physicsMaterialTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::PhysicsMaterial, physicsMaterialTargets)
					: std::vector<ComponentEditRecord>{};
				if (physicsMaterialTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Physics Material components.", physicsMaterialTargets.size());
				}
				bool physicsChanged = false;
				if (!context.ActiveScene.GetColliderComponent(selectedEntity) || !context.ActiveScene.IsColliderEnabled(selectedEntity))
				{
					ImGui::TextUnformatted("No enabled collider: material is ignored.");
				}
				bool controlChanged = ImGui::DragFloat("Static Friction", &physicsMaterial->StaticFriction, 0.01f, 0.0f, 10.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::PhysicsMaterial, physicsMaterialTargets, physicsMaterialBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Dynamic Friction", &physicsMaterial->DynamicFriction, 0.01f, 0.0f, 10.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::PhysicsMaterial, physicsMaterialTargets, physicsMaterialBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Restitution", &physicsMaterial->Restitution, 0.01f, 0.0f, 1.0f);
				physicsChanged |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::PhysicsMaterial, physicsMaterialTargets, physicsMaterialBeforeRecords, controlChanged);
				if (physicsChanged)
				{
					if (context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(selectedEntity);
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (PrefabInstanceComponent* prefab = context.ActiveScene.GetPrefabInstanceComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::PrefabInstance &&
			prefab && inspectorSectionMatches("Prefab Instance", "Prefab Path Source Name Overrides"))
		{
			const ComponentSectionState section = BeginComponentSection<PrefabInstanceComponent>(
				context,
				selectedEntity,
				SceneComponentKind::PrefabInstance,
				"Prefab Instance",
				canPasteComponent(SceneComponentKind::PrefabInstance),
				CanResetComponentKind(SceneComponentKind::PrefabInstance),
				canMoveComponentUp(SceneComponentKind::PrefabInstance),
				canMoveComponentDown(SceneComponentKind::PrefabInstance),
				componentHasPrefabOverride(SceneComponentKind::PrefabInstance),
				canUseComponentPrefabOverrideAction(SceneComponentKind::PrefabInstance),
				canApplyComponentPrefabOverride(SceneComponentKind::PrefabInstance),
				isComponentPinned(SceneComponentKind::PrefabInstance));
			handleComponentSectionActions(SceneComponentKind::PrefabInstance, section);
			if (section.Open && !section.Removed)
			{
				const std::vector<EntityId> prefabTargets = componentEditTargets(SceneComponentKind::PrefabInstance);
				const std::vector<ComponentEditRecord> prefabBeforeRecords = !prefabTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::PrefabInstance, prefabTargets)
					: std::vector<ComponentEditRecord>{};
				ImGui::BeginDisabled(!section.Enabled);
				if (prefabTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Prefab Instance components.", prefabTargets.size());
					ImGui::TextDisabled("Prefab path and source name stay per Entity.");
				}
				ImGui::TextWrapped("Prefab Path: %s", prefab->PrefabPath.empty() ? "<not assigned>" : prefab->PrefabPath.string().c_str());
				ImGui::Text("Source Name: %s", prefab->SourceName.empty() ? "<none>" : prefab->SourceName.c_str());
				const bool trackOverridesChanged = ImGui::Checkbox("Track Overrides", &prefab->TrackPrefabOverrides);
				if (trackOverridesChanged && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				trackMultiComponentControl(SceneComponentKind::PrefabInstance, prefabTargets, prefabBeforeRecords, trackOverridesChanged);
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (SceneReferenceComponent* sceneReference = context.ActiveScene.GetSceneReferenceComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::SceneReference &&
			sceneReference && inspectorSectionMatches("Scene Reference", "Scene Path Additive Auto Load"))
		{
			const ComponentSectionState section = BeginComponentSection<SceneReferenceComponent>(
				context,
				selectedEntity,
				SceneComponentKind::SceneReference,
				"Scene Reference",
				canPasteComponent(SceneComponentKind::SceneReference),
				CanResetComponentKind(SceneComponentKind::SceneReference),
				canMoveComponentUp(SceneComponentKind::SceneReference),
				canMoveComponentDown(SceneComponentKind::SceneReference),
				componentHasPrefabOverride(SceneComponentKind::SceneReference),
				canUseComponentPrefabOverrideAction(SceneComponentKind::SceneReference),
				canApplyComponentPrefabOverride(SceneComponentKind::SceneReference),
				isComponentPinned(SceneComponentKind::SceneReference));
			handleComponentSectionActions(SceneComponentKind::SceneReference, section);
			if (section.Open && !section.Removed)
			{
				const std::vector<EntityId> sceneReferenceTargets = componentEditTargets(SceneComponentKind::SceneReference);
				const std::vector<ComponentEditRecord> sceneReferenceBeforeRecords = !sceneReferenceTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::SceneReference, sceneReferenceTargets)
					: std::vector<ComponentEditRecord>{};
				ImGui::BeginDisabled(!section.Enabled);
				if (sceneReferenceTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Scene Reference components.", sceneReferenceTargets.size());
					ImGui::TextDisabled("Scene path stays per Entity.");
				}
				ImGui::TextWrapped("Scene Path: %s", sceneReference->ScenePath.empty() ? "<not assigned>" : sceneReference->ScenePath.string().c_str());
				const bool hasScenePath = !sceneReference->ScenePath.empty();
				const SceneReferenceRuntimeStatus runtimeStatus = context.OnGetSceneReferenceStatus
					? context.OnGetSceneReferenceStatus(selectedEntity)
					: SceneReferenceRuntimeStatus{};
				if (!runtimeStatus.ResolvedScenePath.empty())
				{
					ImGui::TextWrapped("Resolved: %s", runtimeStatus.ResolvedScenePath.string().c_str());
				}
				ImGui::Text("Runtime: %s", runtimeStatus.StatusText.empty() ? "Not loaded." : runtimeStatus.StatusText.c_str());
				if (runtimeStatus.Loaded)
				{
					ImGui::Text("Loaded Entities: %zu", runtimeStatus.LoadedEntityCount);
					ImGui::SameLine();
					ImGui::TextDisabled(runtimeStatus.Watching ? "Auto reload: watching" : "Auto reload: unavailable");
				}
				else if (hasScenePath)
				{
					ImGui::TextDisabled(runtimeStatus.FileExists ? "File exists" : "File missing");
				}
				ImGui::BeginDisabled(!hasScenePath || !context.OnOpenScene);
				if (ImGui::SmallButton("Open Scene"))
				{
					context.OnOpenScene(sceneReference->ScenePath);
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Open this scene as the active editor scene.");
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(!hasScenePath || !context.OnAssetReveal);
				if (ImGui::SmallButton("Reveal"))
				{
					context.OnAssetReveal(sceneReference->ScenePath);
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!hasScenePath || !sceneReference->LoadAdditively || !context.OnLoadSceneReference);
				if (ImGui::SmallButton("Load As Children"))
				{
					static_cast<void>(context.OnLoadSceneReference(selectedEntity));
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Instantiate the referenced scene under this Entity. Enable Load Additively to use this action.");
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(!runtimeStatus.Loaded || !hasScenePath || !sceneReference->LoadAdditively || !context.OnLoadSceneReference);
				if (ImGui::SmallButton("Reload"))
				{
					static_cast<void>(context.OnLoadSceneReference(selectedEntity));
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					ImGui::SetTooltip("Unload and reload the referenced scene children.");
				}
				ImGui::SameLine();
				ImGui::BeginDisabled(!runtimeStatus.Loaded || !context.OnUnloadSceneReference);
				if (ImGui::SmallButton("Unload Loaded"))
				{
					static_cast<void>(context.OnUnloadSceneReference(selectedEntity));
				}
				ImGui::EndDisabled();
				bool changed = false;
				const bool loadAdditivelyChanged = ImGui::Checkbox("Load Additively", &sceneReference->LoadAdditively);
				changed |= loadAdditivelyChanged;
				trackMultiComponentControl(SceneComponentKind::SceneReference, sceneReferenceTargets, sceneReferenceBeforeRecords, loadAdditivelyChanged);
				const bool autoLoadChanged = ImGui::Checkbox("Auto Load", &sceneReference->AutoLoad);
				changed |= autoLoadChanged;
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Auto Load is stored for runtime/nested-scene workflows; manual load/unload is available in this v1 editor.");
				}
				trackMultiComponentControl(SceneComponentKind::SceneReference, sceneReferenceTargets, sceneReferenceBeforeRecords, autoLoadChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (ScriptComponent* script = context.ActiveScene.GetScriptComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Script &&
			script && inspectorSectionMatches("Script", "Native Lua CSharp GDScript Class Run In Editor Lifecycle"))
		{
			const ComponentSectionState section = BeginComponentSection<ScriptComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Script,
				"Script",
				canPasteComponent(SceneComponentKind::Script),
				CanResetComponentKind(SceneComponentKind::Script),
				canMoveComponentUp(SceneComponentKind::Script),
				canMoveComponentDown(SceneComponentKind::Script),
				componentHasPrefabOverride(SceneComponentKind::Script),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Script),
				canApplyComponentPrefabOverride(SceneComponentKind::Script),
				isComponentPinned(SceneComponentKind::Script));
			handleComponentSectionActions(SceneComponentKind::Script, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				ImGui::TextWrapped("Script Path: %s", script->ScriptPath.empty() ? "<native/unassigned>" : script->ScriptPath.string().c_str());
				const std::vector<EntityId> scriptTargets = componentEditTargets(SceneComponentKind::Script);
				const std::vector<ComponentEditRecord> scriptBeforeRecords = !scriptTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::Script, scriptTargets)
					: std::vector<ComponentEditRecord>{};
				if (scriptTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Script components.", scriptTargets.size());
					ImGui::TextDisabled("Script path stays per Entity.");
				}
				int languageIndex = std::to_underlying(script->Language);
				bool changed = false;
				if (ImGui::Combo("Language", &languageIndex, "Native\0Lua\0CSharp-like\0GDScript-like\0"))
				{
					script->Language = static_cast<ScriptLanguage>((std::clamp)(languageIndex, 0, 3));
					changed = true;
					trackMultiComponentControl(SceneComponentKind::Script, scriptTargets, scriptBeforeRecords, true);
				}
				if (script->Language == ScriptLanguage::Native)
				{
					constexpr std::array kNativeScriptNames = { "GameScript", "SpinScript" };
					int nativeClassIndex = 0;
					for (size_t index = 0; index < kNativeScriptNames.size(); ++index)
					{
						if (script->ClassName == kNativeScriptNames[index])
						{
							nativeClassIndex = static_cast<int>(index);
							break;
						}
					}
					if (ImGui::Combo("Native Class", &nativeClassIndex, "GameScript\0SpinScript\0"))
					{
						nativeClassIndex = std::clamp(nativeClassIndex, 0, static_cast<int>(kNativeScriptNames.size() - 1));
						script->ClassName = kNativeScriptNames[static_cast<size_t>(nativeClassIndex)];
						changed = true;
						trackMultiComponentControl(SceneComponentKind::Script, scriptTargets, scriptBeforeRecords, true);
					}
					ImGui::TextDisabled("Lifecycle: Start -> Update -> LateUpdate -> EndFrame");
				}
				else
				{
					ImGui::Text("Class: %s", script->ClassName.c_str());
					ImGui::TextDisabled("External script runtime is planned after native lifecycle v1.");
				}
				bool controlChanged = ImGui::Checkbox("Run In Editor", &script->RunInEditor);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Script, scriptTargets, scriptBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (Sprite2DComponent* sprite = context.ActiveScene.GetSprite2DComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Sprite2D &&
			sprite && inspectorSectionMatches("Sprite 2D", "Texture Color Size Pivot Sorting Layer Order"))
		{
			const ComponentSectionState section = BeginComponentSection<Sprite2DComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Sprite2D,
				"Sprite 2D",
				canPasteComponent(SceneComponentKind::Sprite2D),
				CanResetComponentKind(SceneComponentKind::Sprite2D),
				canMoveComponentUp(SceneComponentKind::Sprite2D),
				canMoveComponentDown(SceneComponentKind::Sprite2D),
				componentHasPrefabOverride(SceneComponentKind::Sprite2D),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Sprite2D),
				canApplyComponentPrefabOverride(SceneComponentKind::Sprite2D),
				isComponentPinned(SceneComponentKind::Sprite2D));
			handleComponentSectionActions(SceneComponentKind::Sprite2D, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				ImGui::TextWrapped("Texture: %s", sprite->TexturePath.empty() ? "<not assigned>" : sprite->TexturePath.string().c_str());
				const std::vector<EntityId> spriteTargets = componentEditTargets(SceneComponentKind::Sprite2D);
				const std::vector<ComponentEditRecord> spriteBeforeRecords = !spriteTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::Sprite2D, spriteTargets)
					: std::vector<ComponentEditRecord>{};
				if (spriteTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Sprite 2D components.", spriteTargets.size());
					ImGui::TextDisabled("Texture path stays per Entity.");
				}
				bool changed = false;
				bool controlChanged = ImGui::ColorEdit4("Color", &sprite->Color.x);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Sprite2D, spriteTargets, spriteBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat2("Size", &sprite->Size.x, 0.01f, 0.001f, 10000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Sprite2D, spriteTargets, spriteBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat2("Pivot", &sprite->Pivot.x, 0.01f, 0.0f, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Sprite2D, spriteTargets, spriteBeforeRecords, controlChanged);
				controlChanged = ImGui::DragInt("Sorting Layer", &sprite->SortingLayer, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Sprite2D, spriteTargets, spriteBeforeRecords, controlChanged);
				controlChanged = ImGui::DragInt("Order In Layer", &sprite->OrderInLayer, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::Sprite2D, spriteTargets, spriteBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (UiElementComponent* ui = context.ActiveScene.GetUiElementComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::UiElement &&
			ui && inspectorSectionMatches("UI Element", "Panel Label Button Image Anchor Position Size Color"))
		{
			const ComponentSectionState section = BeginComponentSection<UiElementComponent>(
				context,
				selectedEntity,
				SceneComponentKind::UiElement,
				"UI Element",
				canPasteComponent(SceneComponentKind::UiElement),
				CanResetComponentKind(SceneComponentKind::UiElement),
				canMoveComponentUp(SceneComponentKind::UiElement),
				canMoveComponentDown(SceneComponentKind::UiElement),
				componentHasPrefabOverride(SceneComponentKind::UiElement),
				canUseComponentPrefabOverrideAction(SceneComponentKind::UiElement),
				canApplyComponentPrefabOverride(SceneComponentKind::UiElement),
				isComponentPinned(SceneComponentKind::UiElement));
			handleComponentSectionActions(SceneComponentKind::UiElement, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> uiTargets = componentEditTargets(SceneComponentKind::UiElement);
				const std::vector<ComponentEditRecord> uiBeforeRecords = !uiTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::UiElement, uiTargets)
					: std::vector<ComponentEditRecord>{};
				if (uiTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu UI Element components.", uiTargets.size());
					ImGui::TextDisabled("Text stays per Entity.");
				}
				int kindIndex = std::to_underlying(ui->Kind);
				bool changed = false;
				if (ImGui::Combo("Kind", &kindIndex, "Panel\0Label\0Button\0Image\0"))
				{
					ui->Kind = static_cast<UiElementKind>((std::clamp)(kindIndex, 0, 3));
					changed = true;
					trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, true);
				}
				ImGui::Text("Text: %s", ui->Text.c_str());
				bool controlChanged = ImGui::DragFloat2("Anchor Min", &ui->AnchorMin.x, 0.01f, 0.0f, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat2("Anchor Max", &ui->AnchorMax.x, 0.01f, 0.0f, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat2("Position", &ui->Position.x, 0.5f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat2("Size", &ui->Size.x, 0.5f, 1.0f, 10000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, controlChanged);
				controlChanged = ImGui::ColorEdit4("Color", &ui->Color.x);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::UiElement, uiTargets, uiBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (AudioSourceComponent* audio = context.ActiveScene.GetAudioSourceComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::AudioSource &&
			audio && inspectorSectionMatches("Audio Source", "Clip Volume Pitch Loop Play Spatialize Distance"))
		{
			const ComponentSectionState section = BeginComponentSection<AudioSourceComponent>(
				context,
				selectedEntity,
				SceneComponentKind::AudioSource,
				"Audio Source",
				canPasteComponent(SceneComponentKind::AudioSource),
				CanResetComponentKind(SceneComponentKind::AudioSource),
				canMoveComponentUp(SceneComponentKind::AudioSource),
				canMoveComponentDown(SceneComponentKind::AudioSource),
				componentHasPrefabOverride(SceneComponentKind::AudioSource),
				canUseComponentPrefabOverrideAction(SceneComponentKind::AudioSource),
				canApplyComponentPrefabOverride(SceneComponentKind::AudioSource),
				isComponentPinned(SceneComponentKind::AudioSource));
			handleComponentSectionActions(SceneComponentKind::AudioSource, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				ImGui::TextWrapped("Clip: %s", audio->ClipPath.empty() ? "<not assigned>" : audio->ClipPath.string().c_str());
				const std::vector<EntityId> audioTargets = componentEditTargets(SceneComponentKind::AudioSource);
				const std::vector<ComponentEditRecord> audioBeforeRecords = !audioTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::AudioSource, audioTargets)
					: std::vector<ComponentEditRecord>{};
				if (audioTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Audio Source components.", audioTargets.size());
					ImGui::TextDisabled("Clip path stays per Entity.");
				}
				bool changed = false;
				bool controlChanged = ImGui::DragFloat("Volume", &audio->Volume, 0.01f, 0.0f, 1.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Pitch", &audio->Pitch, 0.01f, 0.1f, 4.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Loop", &audio->Loop);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Play On Start", &audio->PlayOnStart);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Spatialize", &audio->Spatialize);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Min Distance", &audio->MinDistance, 0.05f, 0.0f, 10000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Max Distance", &audio->MaxDistance, 0.1f, 0.0f, 100000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::AudioSource, audioTargets, audioBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (NavigationAgentComponent* navigation = context.ActiveScene.GetNavigationAgentComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::NavigationAgent &&
			navigation && inspectorSectionMatches("Navigation Agent", "Radius Height Speed Acceleration Target"))
		{
			const ComponentSectionState section = BeginComponentSection<NavigationAgentComponent>(
				context,
				selectedEntity,
				SceneComponentKind::NavigationAgent,
				"Navigation Agent",
				canPasteComponent(SceneComponentKind::NavigationAgent),
				CanResetComponentKind(SceneComponentKind::NavigationAgent),
				canMoveComponentUp(SceneComponentKind::NavigationAgent),
				canMoveComponentDown(SceneComponentKind::NavigationAgent),
				componentHasPrefabOverride(SceneComponentKind::NavigationAgent),
				canUseComponentPrefabOverrideAction(SceneComponentKind::NavigationAgent),
				canApplyComponentPrefabOverride(SceneComponentKind::NavigationAgent),
				isComponentPinned(SceneComponentKind::NavigationAgent));
			handleComponentSectionActions(SceneComponentKind::NavigationAgent, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const std::vector<EntityId> navigationTargets = componentEditTargets(SceneComponentKind::NavigationAgent);
				const std::vector<ComponentEditRecord> navigationBeforeRecords = !navigationTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::NavigationAgent, navigationTargets)
					: std::vector<ComponentEditRecord>{};
				if (navigationTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Navigation Agent components.", navigationTargets.size());
				}
				bool changed = false;
				bool controlChanged = ImGui::DragFloat("Radius", &navigation->Radius, 0.01f, 0.001f, 1000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Height", &navigation->Height, 0.01f, 0.001f, 1000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Speed", &navigation->Speed, 0.01f, 0.0f, 1000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat("Acceleration", &navigation->Acceleration, 0.01f, 0.0f, 10000.0f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Has Target", &navigation->HasTarget);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				controlChanged = ImGui::DragFloat3("Target", &navigation->Target.x, 0.05f);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NavigationAgent, navigationTargets, navigationBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (NetworkIdentityComponent* network = context.ActiveScene.GetNetworkIdentityComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::NetworkIdentity &&
			network && inspectorSectionMatches("Network Identity", "Network Id Prefab Key Replicate Server Authoritative"))
		{
			const ComponentSectionState section = BeginComponentSection<NetworkIdentityComponent>(
				context,
				selectedEntity,
				SceneComponentKind::NetworkIdentity,
				"Network Identity",
				canPasteComponent(SceneComponentKind::NetworkIdentity),
				CanResetComponentKind(SceneComponentKind::NetworkIdentity),
				canMoveComponentUp(SceneComponentKind::NetworkIdentity),
				canMoveComponentDown(SceneComponentKind::NetworkIdentity),
				componentHasPrefabOverride(SceneComponentKind::NetworkIdentity),
				canUseComponentPrefabOverrideAction(SceneComponentKind::NetworkIdentity),
				canApplyComponentPrefabOverride(SceneComponentKind::NetworkIdentity),
				isComponentPinned(SceneComponentKind::NetworkIdentity));
			handleComponentSectionActions(SceneComponentKind::NetworkIdentity, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				ImGui::Text("Network Id: %llu", static_cast<unsigned long long>(network->NetworkId));
				ImGui::Text("Prefab Key: %s", network->PrefabKey.empty() ? "<none>" : network->PrefabKey.c_str());
				const std::vector<EntityId> networkTargets = componentEditTargets(SceneComponentKind::NetworkIdentity);
				const std::vector<ComponentEditRecord> networkBeforeRecords = !networkTargets.empty()
					? captureComponentEditRecords(SceneComponentKind::NetworkIdentity, networkTargets)
					: std::vector<ComponentEditRecord>{};
				if (networkTargets.size() > 1)
				{
					ImGui::TextDisabled("Editing %zu Network Identity components.", networkTargets.size());
					ImGui::TextDisabled("Network Id and Prefab Key stay per Entity.");
				}
				bool changed = false;
				bool controlChanged = ImGui::Checkbox("Replicate Transform", &network->ReplicateTransform);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NetworkIdentity, networkTargets, networkBeforeRecords, controlChanged);
				controlChanged = ImGui::Checkbox("Server Authoritative", &network->ServerAuthoritative);
				changed |= controlChanged;
				trackMultiComponentControl(SceneComponentKind::NetworkIdentity, networkTargets, networkBeforeRecords, controlChanged);
				if (changed && context.OnSceneEdited)
				{
					context.OnSceneEdited();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Mesh &&
			bounds && inspectorSectionMatches("Bounds", "Local Min Max AABB"))
		{
			if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawVector3Text("Local Min", bounds->LocalMin);
				DrawVector3Text("Local Max", bounds->LocalMax);
			}
		}

		if (MeshComponent* meshComponent = context.ActiveScene.GetMeshComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Mesh &&
			meshComponent && inspectorSectionMatches("Mesh", "Vertices Indices Submeshes Materials Animated Bones Prefab Apply Revert Restore Pending Cancel Failed Conflict"))
		{
			const ComponentSectionState section = BeginComponentSection<MeshComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Mesh,
				"Mesh",
				canPasteComponent(SceneComponentKind::Mesh),
				CanResetComponentKind(SceneComponentKind::Mesh),
				canMoveComponentUp(SceneComponentKind::Mesh),
				canMoveComponentDown(SceneComponentKind::Mesh),
				componentHasPrefabOverride(SceneComponentKind::Mesh),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Mesh),
				canApplyComponentPrefabOverride(SceneComponentKind::Mesh),
				isComponentPinned(SceneComponentKind::Mesh));
			handleComponentSectionActions(SceneComponentKind::Mesh, section);
			if (section.Open && !section.Removed)
			{
				if (context.OnGetMeshRestoreStatus)
				{
					const MeshRestoreRuntimeStatus restoreStatus = context.OnGetMeshRestoreStatus(selectedEntity);
					if (restoreStatus.HasStatus)
					{
						ImGui::SeparatorText("Async Mesh Restore");
						const ImVec4 statusColor = restoreStatus.Pending
							? ImVec4(0.42f, 0.72f, 1.0f, 1.0f)
							: (restoreStatus.Conflicted
								? ImVec4(1.0f, 0.72f, 0.22f, 1.0f)
								: (restoreStatus.Failed
								? ImVec4(1.0f, 0.35f, 0.25f, 1.0f)
								: ImVec4(0.78f, 0.78f, 0.78f, 1.0f)));
						const char* statusText = restoreStatus.Pending
							? "Pending"
							: (restoreStatus.Conflicted ? "Conflict" : (restoreStatus.Failed ? "Failed" : (restoreStatus.Cancelled ? "Cancelled" : "Finished")));
						ImGui::TextColored(statusColor, "Status: %s", statusText);
						ImGui::Text("Generation: %llu", static_cast<unsigned long long>(restoreStatus.Generation));
						if (!restoreStatus.SourcePath.empty())
						{
							ImGui::TextWrapped("Source: %s", restoreStatus.SourcePath.string().c_str());
						}
						if (!restoreStatus.SourcePrefabPath.empty())
						{
							ImGui::TextWrapped("Prefab: %s", restoreStatus.SourcePrefabPath.string().c_str());
						}
						if (!restoreStatus.SourcePath.empty() || !restoreStatus.SourcePrefabPath.empty())
						{
							ImGui::SeparatorText("Path Validation");
							const auto drawRestorePathValidation = [&](const char* label, const std::filesystem::path& path)
							{
								if (path.empty())
								{
									return;
								}

								std::error_code pathError;
								const bool exists = std::filesystem::exists(path, pathError);
								ImGui::PushID(label);
								ImGui::TextWrapped("%s: %s", label, path.string().c_str());
								ImGui::SameLine();
								if (pathError)
								{
									ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "[Error]");
									if (ImGui::IsItemHovered())
									{
										ImGui::SetTooltip("%s", pathError.message().c_str());
									}
								}
								else if (exists)
								{
									ImGui::TextColored(ImVec4(0.42f, 0.92f, 0.52f, 1.0f), "[OK]");
									ImGui::SameLine();
									ImGui::BeginDisabled(!context.OnAssetReveal);
									if (ImGui::SmallButton("Reveal"))
									{
										context.OnAssetReveal(path);
									}
									ImGui::EndDisabled();
									ImGui::SameLine();
									ImGui::BeginDisabled(!context.OnAssetOpen);
									if (ImGui::SmallButton("Open"))
									{
										context.OnAssetOpen(path);
									}
									ImGui::EndDisabled();
								}
								else
								{
									ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "[Missing]");
									ImGui::SameLine();
									ImGui::BeginDisabled(!context.OnProjectRefresh);
									if (ImGui::SmallButton("Refresh Project"))
									{
										context.OnProjectRefresh();
									}
									ImGui::EndDisabled();
									if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
									{
										ImGui::SetTooltip("Refresh the Project snapshot after restoring or moving this file.");
									}
								}
								ImGui::PopID();
							};
							drawRestorePathValidation("Source Model", restoreStatus.SourcePath);
							drawRestorePathValidation("Source Prefab", restoreStatus.SourcePrefabPath);
						}
						if (!restoreStatus.Message.empty())
						{
							ImGui::TextWrapped("%s", restoreStatus.Message.c_str());
						}
						if (restoreStatus.Conflicted)
						{
							const Asset::StaticMeshAsset* currentMesh = meshComponent && meshComponent->Asset ? meshComponent->Asset.get() : nullptr;
							ImGui::SeparatorText("Conflict Preview");
							if (currentMesh)
							{
								ImGui::TextWrapped(
									"Current Mesh: %s",
									currentMesh->SourcePath.empty() ? "<generated/none>" : currentMesh->SourcePath.string().c_str());
								ImGui::Text(
									"Current Primitive: %s",
									Asset::PrimitiveMeshKindToString(currentMesh->PrimitiveKind).data());
								ImGui::Text(
									"Current Counts: %zu vertices, %zu indices, %zu materials",
									currentMesh->Vertices.size(),
									currentMesh->Indices.size(),
									currentMesh->Materials.size());
							}
							else
							{
								ImGui::TextUnformatted("Current Mesh: <none>");
							}
							ImGui::TextWrapped(
								"Stored Restore: %s",
								restoreStatus.SourcePath.empty() ? "<unknown>" : restoreStatus.SourcePath.string().c_str());
							if (restoreStatus.ImportedVertexCount > 0 || restoreStatus.ImportedIndexCount > 0 || restoreStatus.ImportedMaterialCount > 0)
							{
								ImGui::Text(
									"Stored Counts: %zu vertices, %zu indices, %zu materials",
									restoreStatus.ImportedVertexCount,
									restoreStatus.ImportedIndexCount,
									restoreStatus.ImportedMaterialCount);
							}
							if (!restoreStatus.MaterialDiffRows.empty())
							{
								if (ImGui::TreeNodeEx("Material Diff", ImGuiTreeNodeFlags_DefaultOpen))
								{
									ImGui::Checkbox("Pin Focus", &m_FocusedMaterialFocusPinned);
									if (ImGui::IsItemHovered())
									{
										ImGui::SetTooltip("Keep the selected Material Diff row highlighted in the Materials section until it is cleared.");
									}
									ImGui::SameLine();
									const bool hasMaterialFocusForSelection =
										m_FocusedMaterialEntity == selectedEntity &&
										m_FocusedMaterialIndex != static_cast<size_t>(-1);
									ImGui::BeginDisabled(!hasMaterialFocusForSelection);
									if (ImGui::SmallButton("Clear Focus"))
									{
										m_FocusedMaterialEntity = InvalidEntityId;
										m_FocusedMaterialIndex = static_cast<size_t>(-1);
										m_FocusedMaterialTextureSlot = Asset::MaterialTextureSlot::Count;
										m_FocusedMaterialControl = MeshRestoreMaterialFocusKind::None;
										m_FocusedMaterialHighlightFrames = 0;
									}
									ImGui::EndDisabled();
									if (hasMaterialFocusForSelection)
									{
										ImGui::SameLine();
										ImGui::TextDisabled(
											"Focused: Material[%zu]%s",
											m_FocusedMaterialIndex,
											m_FocusedMaterialFocusPinned ? " (pinned)" : "");
									}
									if (ImGui::BeginTable("MeshRestoreMaterialDiffTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
									{
										ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 190.0f);
										ImGui::TableSetupColumn("Current");
										ImGui::TableSetupColumn("Restore");
										ImGui::TableHeadersRow();
										for (size_t diffRowIndex = 0; diffRowIndex < restoreStatus.MaterialDiffRows.size(); ++diffRowIndex)
										{
											const MeshRestoreMaterialDiffRow& diffRow = restoreStatus.MaterialDiffRows[diffRowIndex];
											const bool canFocusMaterial = diffRow.MaterialIndex != static_cast<size_t>(-1);
											ImGui::TableNextRow();
											ImGui::TableSetColumnIndex(0);
											ImGui::PushID(static_cast<int>(diffRowIndex));
											if (ImGui::Selectable(diffRow.Field.c_str(), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, 0.0f)))
											{
												if (canFocusMaterial)
												{
													m_FocusedMaterialEntity = selectedEntity;
													m_FocusedMaterialIndex = diffRow.MaterialIndex;
													m_FocusedMaterialTextureSlot = diffRow.TextureSlot;
													m_FocusedMaterialControl = diffRow.FocusKind;
													m_FocusedMaterialHighlightFrames = 180;
												}
											}
											if (canFocusMaterial && ImGui::IsItemHovered())
											{
												if (diffRow.TextureSlot != Asset::MaterialTextureSlot::Count)
												{
													ImGui::SetTooltip(
														"Focus Material[%zu] %s slot in the Materials section.",
														diffRow.MaterialIndex,
														std::string(Asset::MaterialTextureSlotName(diffRow.TextureSlot)).c_str());
												}
												else
												{
													ImGui::SetTooltip("Focus Material[%zu] in the Materials section.", diffRow.MaterialIndex);
												}
											}
											ImGui::PopID();
											ImGui::TableSetColumnIndex(1);
											ImGui::TextWrapped("%s", diffRow.CurrentValue.c_str());
											ImGui::TableSetColumnIndex(2);
											ImGui::TextWrapped("%s", diffRow.RestoreValue.c_str());
										}
										ImGui::EndTable();
									}
									ImGui::TreePop();
								}
							}
							else if (!restoreStatus.MaterialDiffLines.empty())
							{
								if (ImGui::TreeNodeEx("Material Slot Diff", ImGuiTreeNodeFlags_DefaultOpen))
								{
									for (const std::string& diffLine : restoreStatus.MaterialDiffLines)
									{
										ImGui::Bullet();
										ImGui::SameLine();
										ImGui::TextWrapped("%s", diffLine.c_str());
									}
									ImGui::TreePop();
								}
							}

							ImGui::BeginDisabled(!context.OnApplyConflictedMeshRestore);
							if (ImGui::SmallButton("Apply Anyway"))
							{
								ImGui::OpenPopup("Apply Conflicted Mesh Restore");
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip("Apply the stored import result even though the current Mesh or source prefab changed while it was importing.");
							}
							ImGui::EndDisabled();
							if (ImGui::BeginPopupModal("Apply Conflicted Mesh Restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
							{
								ImGui::TextUnformatted("Apply this conflicted Mesh restore anyway?");
								ImGui::Separator();
								ImGui::TextWrapped("This replaces the current Entity Mesh with the stored import result shown in the preview.");
								ImGui::TextWrapped("Current edits made after the restore request may be overwritten.");
								ImGui::Spacing();
								ImGui::BeginDisabled(!context.OnApplyConflictedMeshRestore);
								if (ImGui::Button("Apply Anyway"))
								{
									static_cast<void>(context.OnApplyConflictedMeshRestore(selectedEntity));
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndDisabled();
								ImGui::SameLine();
								if (ImGui::Button("Cancel"))
								{
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndPopup();
							}
							ImGui::SameLine();
							const bool canReloadPrefabSource = context.OnReloadMeshRestoreFromPrefabSource && !restoreStatus.SourcePrefabPath.empty();
							ImGui::BeginDisabled(!canReloadPrefabSource);
							if (ImGui::SmallButton("Reload Prefab Source"))
							{
								ImGui::OpenPopup("Reload Mesh Restore Prefab Source");
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canReloadPrefabSource
									? "Reload the prefab file and start a fresh Mesh restore request."
									: "No source prefab path is available for this restore.");
							}
							ImGui::EndDisabled();
							if (ImGui::BeginPopupModal("Reload Mesh Restore Prefab Source", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
							{
								ImGui::TextUnformatted("Reload the prefab source and queue a fresh Mesh restore?");
								ImGui::Separator();
								ImGui::TextWrapped("This discards the stored conflicted import result and reads the prefab file again.");
								if (!restoreStatus.SourcePrefabPath.empty())
								{
									ImGui::TextWrapped("%s", restoreStatus.SourcePrefabPath.string().c_str());
								}
								ImGui::Spacing();
								ImGui::BeginDisabled(!canReloadPrefabSource);
								if (ImGui::Button("Reload Prefab Source"))
								{
									static_cast<void>(context.OnReloadMeshRestoreFromPrefabSource(selectedEntity));
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndDisabled();
								ImGui::SameLine();
								if (ImGui::Button("Cancel"))
								{
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndPopup();
							}
							ImGui::SameLine();
							ImGui::BeginDisabled(!context.OnCancelMeshRestore);
							if (ImGui::SmallButton("Keep Current"))
							{
								static_cast<void>(context.OnCancelMeshRestore(selectedEntity));
							}
							ImGui::EndDisabled();
						}
						else if (context.OnCancelMeshRestore)
						{
							const char* actionLabel = restoreStatus.Pending ? "Cancel Pending Restore" : "Dismiss Restore Status";
							if (ImGui::SmallButton(actionLabel))
							{
								static_cast<void>(context.OnCancelMeshRestore(selectedEntity));
							}
							if (restoreStatus.Pending && ImGui::IsItemHovered())
							{
								ImGui::SetTooltip("Invalidates this restore generation. The worker may finish later, but its result will be discarded.");
							}
						}
					}
				}

				ImGui::BeginDisabled(!section.Enabled);
				const Asset::StaticMeshAsset* mesh = meshComponent->Asset.get();
				if (!mesh)
				{
					ImGui::TextUnformatted("No mesh asset assigned.");
				}
				else
				{
					ImGui::Text("Vertices: %d", static_cast<int>(mesh->Vertices.size()));
					ImGui::Text("Indices: %d", static_cast<int>(mesh->Indices.size()));
					ImGui::Text("Submeshes: %d", static_cast<int>(mesh->Submeshes.size()));
					ImGui::Text("Materials: %d", static_cast<int>(mesh->Materials.size()));
					ImGui::Text("Animated: %s", mesh->IsAnimated ? "true" : "false");
					if (prefabOverrideSourceLoaded && prefabOverrideSource.HasMesh)
					{
						const bool meshPathDiffers = mesh->SourcePath.empty() != prefabOverrideSource.MeshAssetPath.empty() ||
							(!mesh->SourcePath.empty() && !SamePath(mesh->SourcePath, prefabOverrideSource.MeshAssetPath));
						const bool materialCountDiffers = mesh->Materials.size() != prefabOverrideSource.MaterialOverrides.size();
						if (meshPathDiffers || materialCountDiffers)
						{
							ImGui::Separator();
							ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "Mesh prefab override (protected)");
							if (meshPathDiffers)
							{
								ImGui::TextWrapped("Current Asset: %s", mesh->SourcePath.empty() ? "<generated/none>" : mesh->SourcePath.string().c_str());
								ImGui::TextWrapped("Prefab Asset:  %s", prefabOverrideSource.MeshAssetPath.empty() ? "<generated/none>" : prefabOverrideSource.MeshAssetPath.string().c_str());
							}
							if (materialCountDiffers)
							{
								ImGui::Text("Current Materials: %zu", mesh->Materials.size());
								ImGui::Text("Prefab Materials:  %zu", prefabOverrideSource.MaterialOverrides.size());
							}
							ImGui::TextDisabled("Mesh asset replacement is protected from section-level Revert/Apply in v1.");
							const bool canApplyMeshToPrefab = context.OnSavePrefabInspectionRoot && !prefabOverrideSourcePath.empty();
							ImGui::BeginDisabled(!canApplyMeshToPrefab);
							if (ImGui::SmallButton("Apply Mesh To Prefab..."))
							{
								ImGui::OpenPopup("Apply Mesh To Prefab");
							}
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canApplyMeshToPrefab
									? "Open a confirmation dialog before writing current mesh asset/material metadata to the prefab source."
									: "Prefab mesh apply is unavailable in this editor context.");
							}
							if (ImGui::BeginPopupModal("Apply Mesh To Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
							{
								ImGui::TextUnformatted("Apply current Mesh metadata to the prefab source?");
								ImGui::Separator();
								ImGui::TextWrapped("This writes the current mesh asset path, primitive kind, enabled state, and material override array to:");
								ImGui::TextWrapped("%s", prefabOverrideSourcePath.string().c_str());
								ImGui::Spacing();
								ImGui::TextWrapped("Current Asset: %s", mesh->SourcePath.empty() ? "<generated/none>" : mesh->SourcePath.string().c_str());
								ImGui::TextWrapped("Prefab Asset:  %s", prefabOverrideSource.MeshAssetPath.empty() ? "<generated/none>" : prefabOverrideSource.MeshAssetPath.string().c_str());
								ImGui::Text("Current Materials: %zu", mesh->Materials.size());
								ImGui::Text("Prefab Materials:  %zu", prefabOverrideSource.MaterialOverrides.size());
								ImGui::Spacing();
								ImGui::TextDisabled("Reverting mesh asset replacement still stays protected in v1.");
								ImGui::BeginDisabled(!context.OnSavePrefabInspectionRoot);
								if (ImGui::Button("Apply"))
								{
									ScenePersistence::LoadedSceneEntity currentMeshSnapshot;
									if (CaptureComponentSnapshot(context.ActiveScene, selectedEntity, SceneComponentKind::Mesh, currentMeshSnapshot))
									{
										ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabOverrideSource;
										updatedPrefabRoot.HasMesh = true;
										updatedPrefabRoot.MeshEnabled = currentMeshSnapshot.MeshEnabled;
										updatedPrefabRoot.MeshAssetPath = currentMeshSnapshot.MeshAssetPath;
										updatedPrefabRoot.PrimitiveKind = currentMeshSnapshot.PrimitiveKind;
										updatedPrefabRoot.MaterialOverrides = currentMeshSnapshot.MaterialOverrides;
										static_cast<void>(context.OnSavePrefabInspectionRoot(prefabOverrideSourcePath, updatedPrefabRoot));
									}
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndDisabled();
								ImGui::SameLine();
								if (ImGui::Button("Cancel"))
								{
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndPopup();
							}
							ImGui::SameLine();
							const bool canRevertMeshFromPrefab = context.OnRevertMeshToPrefabSource && prefabOverrideSource.HasMesh;
							ImGui::BeginDisabled(!canRevertMeshFromPrefab);
							if (ImGui::SmallButton("Revert Mesh From Prefab..."))
							{
								ImGui::OpenPopup("Revert Mesh From Prefab");
							}
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canRevertMeshFromPrefab
									? "Open a confirmation dialog before reimporting/replacing the current Entity Mesh from the prefab source."
									: "Prefab Mesh revert is unavailable in this editor context.");
							}
							if (ImGui::BeginPopupModal("Revert Mesh From Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
							{
								ImGui::TextUnformatted("Revert current Entity Mesh from the prefab source?");
								ImGui::Separator();
								ImGui::TextWrapped("This keeps the current Entity name and Transform, then restores Mesh asset/primitive metadata and material overrides from the prefab source.");
								ImGui::TextWrapped("Model assets are queued through the async import pipeline, so the current model stays visible if import fails.");
								ImGui::Spacing();
								ImGui::TextWrapped("Current Asset: %s", mesh->SourcePath.empty() ? "<generated/none>" : mesh->SourcePath.string().c_str());
								ImGui::TextWrapped("Prefab Asset:  %s", prefabOverrideSource.MeshAssetPath.empty() ? "<generated/none>" : prefabOverrideSource.MeshAssetPath.string().c_str());
								ImGui::Text("Current Materials: %zu", mesh->Materials.size());
								ImGui::Text("Prefab Materials:  %zu", prefabOverrideSource.MaterialOverrides.size());
								ImGui::Spacing();
								ImGui::TextDisabled("Undo/Redo restores the captured Mesh snapshots; model assets are queued through async import again.");
								ImGui::BeginDisabled(!context.OnRevertMeshToPrefabSource);
								if (ImGui::Button("Revert"))
								{
									static_cast<void>(context.OnRevertMeshToPrefabSource(selectedEntity, prefabOverrideSource, prefabOverrideSourcePath));
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndDisabled();
								ImGui::SameLine();
								if (ImGui::Button("Cancel"))
								{
									ImGui::CloseCurrentPopup();
								}
								ImGui::EndPopup();
							}
						}
					}
					if (mesh->IsAnimated)
					{
						ImGui::Text("Animation Clips: %d", static_cast<int>(mesh->Animations.size()));
						ImGui::Text("Bones: %d", static_cast<int>(mesh->Bones.size()));
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}

		if (AnimatorComponent* animator = context.ActiveScene.GetAnimatorComponent(selectedEntity);
			inspectorComponentKind == SceneComponentKind::Animator &&
			animator && inspectorSectionMatches("Animator", "Animation Clip Playing Loop Speed Time Duration Ticks Channels"))
		{
			const ComponentSectionState section = BeginComponentSection<AnimatorComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Animator,
				"Animator",
				canPasteComponent(SceneComponentKind::Animator),
				CanResetComponentKind(SceneComponentKind::Animator),
				canMoveComponentUp(SceneComponentKind::Animator),
				canMoveComponentDown(SceneComponentKind::Animator),
				componentHasPrefabOverride(SceneComponentKind::Animator),
				canUseComponentPrefabOverrideAction(SceneComponentKind::Animator),
				canApplyComponentPrefabOverride(SceneComponentKind::Animator),
				isComponentPinned(SceneComponentKind::Animator));
			handleComponentSectionActions(SceneComponentKind::Animator, section);
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				const Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(selectedEntity);
				if (!mesh || !mesh->IsAnimated || mesh->Animations.empty())
				{
					ImGui::TextUnformatted("Animator requires an animated mesh.");
				}
				else
				{
					bool animatorChanged = false;
					const uint32_t lastClipIndex = static_cast<uint32_t>(mesh->Animations.size() - 1);
					animator->CurrentClipIndex = (std::min)(animator->CurrentClipIndex, lastClipIndex);
					const Asset::AnimationClip& currentClip = mesh->Animations[animator->CurrentClipIndex];
					const std::string currentClipLabel = currentClip.Name.empty()
						? std::format("Clip {}", animator->CurrentClipIndex)
						: currentClip.Name;

					if (ImGui::BeginCombo("Clip", currentClipLabel.c_str()))
					{
						for (uint32_t clipIndex = 0; clipIndex < mesh->Animations.size(); ++clipIndex)
						{
							const Asset::AnimationClip& clip = mesh->Animations[clipIndex];
							const std::string clipLabel = clip.Name.empty()
								? std::format("Clip {}", clipIndex)
								: clip.Name;
							const bool selectedClip = clipIndex == animator->CurrentClipIndex;
							if (ImGui::Selectable(clipLabel.c_str(), selectedClip))
							{
								animator->CurrentClipIndex = clipIndex;
								animator->TimeSeconds = 0.0f;
								animatorChanged = true;
							}
							if (selectedClip)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}

					animatorChanged |= ImGui::Checkbox("Playing", &animator->Playing);
					ImGui::SameLine();
					animatorChanged |= ImGui::Checkbox("Loop", &animator->Loop);
					animatorChanged |= ImGui::DragFloat("Speed", &animator->Speed, 0.01f, 0.0f, 10.0f, "%.2f");

					const double ticksPerSecond = currentClip.TicksPerSecond > 0.0 ? currentClip.TicksPerSecond : 25.0;
					const float durationSeconds = currentClip.DurationTicks > 0.0
						? static_cast<float>(currentClip.DurationTicks / ticksPerSecond)
						: 0.0f;
					if (durationSeconds > 0.0f)
					{
						animator->TimeSeconds = (std::clamp)(animator->TimeSeconds, 0.0f, durationSeconds);
						animatorChanged |= ImGui::SliderFloat("Time", &animator->TimeSeconds, 0.0f, durationSeconds, "%.3f sec");
					}
					else
					{
						ImGui::TextUnformatted("Time: <invalid duration>");
					}

					ImGui::Text("Duration: %.3f sec / %.1f ticks", durationSeconds, currentClip.DurationTicks);
					ImGui::Text("Ticks/sec: %.2f", ticksPerSecond);
					ImGui::Text("Channels: %d", static_cast<int>(currentClip.Channels.size()));
					if (animatorChanged && context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}
		}

		if (Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(selectedEntity);
			mesh && inspectorSectionMatches("Materials", "Material Shading Model Phong PBR Unlit Base Color Vertex Normal Opacity Metallic Roughness Specular Shininess Texture Slots"))
		{
			const bool hasFocusedMaterial =
				m_FocusedMaterialEntity == selectedEntity &&
				m_FocusedMaterialIndex != static_cast<size_t>(-1) &&
				m_FocusedMaterialIndex < mesh->Materials.size() &&
				(m_FocusedMaterialFocusPinned || m_FocusedMaterialHighlightFrames > 0);
			if (hasFocusedMaterial)
			{
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			}
			if (ImGui::CollapsingHeader("Materials"))
			{
				ImGui::InputTextWithHint(
					"Remap Filter",
					"Filter remap candidates...",
					m_TextureRemapFilter.data(),
					m_TextureRemapFilter.size());
				const std::string_view textureRemapFilter = TextFilter(m_TextureRemapFilter);
				std::vector<TextureRemapPreviewRow> meshRemapPreviewRows =
					BuildMeshTextureRemapPreviewRows(context.ProjectSnapshot, *mesh, textureRemapFilter);
				ApplyTextureRemapCandidateOverrides(meshRemapPreviewRows, m_TextureRemapCandidateOverrides);
				const std::vector<MaterialTextureBatchAssignment> meshAutoRemapAssignments =
					BuildBatchAssignmentsFromTextureRemapPreview(meshRemapPreviewRows);
				const size_t meshAutoRemapSlotCount = CountMaterialTextureAssignments(meshAutoRemapAssignments);
				const size_t riskyRemapRowCount = CountRiskyTextureRemapRows(meshRemapPreviewRows);
				const bool canAutoRemapMesh =
					!meshAutoRemapAssignments.empty() &&
					static_cast<bool>(context.OnMaterialTextureBatchAssigned);
				ImGui::BeginDisabled(!canAutoRemapMesh);
				if (ImGui::SmallButton("Auto Remap All Missing"))
				{
					if (riskyRemapRowCount > 0)
					{
						ImGui::OpenPopup("Confirm Auto Remap All Missing");
					}
					else
					{
						context.OnMaterialTextureBatchAssigned(selectedEntity, meshAutoRemapAssignments);
					}
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				{
					if (canAutoRemapMesh)
					{
						ImGui::SetTooltip(riskyRemapRowCount > 0
							? "Some candidates are low confidence or ambiguous; confirmation is required."
							: "Assign the best suggested image candidate for every missing texture slot in this Mesh.");
					}
					else
					{
						ImGui::SetTooltip("No missing texture slots with matching candidates were found across this Mesh.");
					}
				}
				if (ImGui::BeginPopupModal("Confirm Auto Remap All Missing", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::TextWrapped(
						"%zu of %zu remap candidate%s need review before applying.",
						riskyRemapRowCount,
						meshRemapPreviewRows.size(),
						meshRemapPreviewRows.size() == 1 ? "" : "s");
					ImGui::TextDisabled("Low confidence or ambiguous matches may assign the wrong texture slot.");
					if (ImGui::BeginTable("AutoRemapRiskTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
					{
						ImGui::TableSetupColumn("Material");
						ImGui::TableSetupColumn("Slot");
						ImGui::TableSetupColumn("Confidence");
						ImGui::TableSetupColumn("Candidate");
						ImGui::TableHeadersRow();
						for (const TextureRemapPreviewRow& row : meshRemapPreviewRows)
						{
							if (row.Confidence != TextureRemapConfidence::Low &&
								row.Confidence != TextureRemapConfidence::Ambiguous)
							{
								continue;
							}
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::Text("Material[%zu] %s", row.MaterialIndex, row.MaterialName.empty() ? "<unnamed>" : row.MaterialName.c_str());
							ImGui::TableSetColumnIndex(1);
							ImGui::Text("%s", std::string(Asset::MaterialTextureSlotName(row.Slot)).c_str());
							ImGui::TableSetColumnIndex(2);
							ImGui::TextColored(
								TextureRemapConfidenceColor(row.Confidence),
								"%s (%d)",
								TextureRemapConfidenceName(row.Confidence),
								row.Candidate.Score);
							ImGui::TableSetColumnIndex(3);
							const std::string candidatePath = context.ProjectSnapshot && context.ProjectSnapshot->RootExists
								? RelativeDisplayPath(row.Candidate.Path, context.ProjectSnapshot->RootPath)
								: row.Candidate.Path.string();
							ImGui::TextWrapped("%s", candidatePath.c_str());
							if (!row.AlternativePath.empty())
							{
								const std::string alternativePath = context.ProjectSnapshot && context.ProjectSnapshot->RootExists
									? RelativeDisplayPath(row.AlternativePath, context.ProjectSnapshot->RootPath)
									: row.AlternativePath.string();
								ImGui::TextDisabled("Next: %s (%d)", alternativePath.c_str(), row.AlternativeScore);
							}
						}
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::BeginDisabled(!canAutoRemapMesh);
					if (ImGui::Button("Apply Remap"))
					{
						context.OnMaterialTextureBatchAssigned(selectedEntity, meshAutoRemapAssignments);
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::Button("Cancel"))
					{
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				if (!meshAutoRemapAssignments.empty())
				{
					ImGui::SameLine();
					ImGui::TextDisabled(
						"%zu slot%s across %zu material%s ready",
						meshAutoRemapSlotCount,
						meshAutoRemapSlotCount == 1 ? "" : "s",
						meshAutoRemapAssignments.size(),
						meshAutoRemapAssignments.size() == 1 ? "" : "s");
					if (riskyRemapRowCount > 0)
					{
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "%zu review", riskyRemapRowCount);
					}
					if (!m_TextureRemapCandidateOverrides.empty())
					{
						ImGui::SameLine();
						if (ImGui::SmallButton("Clear Candidate Choices"))
						{
							m_TextureRemapCandidateOverrides.clear();
						}
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("Forget manual candidate choices made in this Inspector session.");
						}
					}
					if (ImGui::TreeNodeEx("Remap Preview", ImGuiTreeNodeFlags_DefaultOpen))
					{
						for (const TextureRemapPreviewRow& row : meshRemapPreviewRows)
						{
							const std::string overrideKey = BuildTextureRemapOverrideKey(row.MaterialIndex, row.Slot, row.MissingPath);
							const std::string materialName = row.MaterialName.empty() ? "<unnamed>" : row.MaterialName;
							const std::string previewPath =
								context.ProjectSnapshot && context.ProjectSnapshot->RootExists
								? RelativeDisplayPath(row.Candidate.Path, context.ProjectSnapshot->RootPath)
								: row.Candidate.Path.string();
							ImGui::BulletText(
								"Material[%zu] %s / %s",
								row.MaterialIndex,
								materialName.c_str(),
								std::string(Asset::MaterialTextureSlotName(row.Slot)).c_str());
							ImGui::SameLine();
							ImGui::TextColored(
								TextureRemapConfidenceColor(row.Confidence),
								"[%s %d%s]",
								TextureRemapConfidenceName(row.Confidence),
								row.Candidate.Score,
								row.CandidateOverridden ? ", chosen" : "");
							ImGui::Indent(18.0f);
							ImGui::TextWrapped("%s", previewPath.c_str());
							if (!row.Candidates.empty())
							{
								ImGui::PushID(overrideKey.c_str());
								const std::string candidateLabel = std::format(
									"{} ({}, {})",
									row.Candidate.Path.filename().string(),
									row.Candidate.Reason,
									row.Candidate.Score);
								if (ImGui::BeginCombo("Candidate", candidateLabel.c_str()))
								{
									for (const TextureRemapCandidate& candidate : row.Candidates)
									{
										const bool selectedCandidate = SamePath(candidate.Path, row.Candidate.Path);
										const std::string optionLabel = std::format(
											"{} ({}, score {})",
											context.ProjectSnapshot && context.ProjectSnapshot->RootExists
												? RelativeDisplayPath(candidate.Path, context.ProjectSnapshot->RootPath)
												: candidate.Path.string(),
											candidate.Reason,
											candidate.Score);
										if (ImGui::Selectable(optionLabel.c_str(), selectedCandidate))
										{
											m_TextureRemapCandidateOverrides[overrideKey] = candidate.Path;
										}
										if (selectedCandidate)
										{
											ImGui::SetItemDefaultFocus();
										}
									}
									ImGui::EndCombo();
								}
								if (row.CandidateOverridden)
								{
									ImGui::SameLine();
									if (ImGui::SmallButton("Auto"))
									{
										m_TextureRemapCandidateOverrides.erase(overrideKey);
									}
									if (ImGui::IsItemHovered())
									{
										ImGui::SetTooltip("Return this row to the automatic best candidate.");
									}
								}
								ImGui::PopID();
							}
							if (!row.AlternativePath.empty() && row.Confidence == TextureRemapConfidence::Ambiguous)
							{
								const std::string alternativePath = context.ProjectSnapshot && context.ProjectSnapshot->RootExists
									? RelativeDisplayPath(row.AlternativePath, context.ProjectSnapshot->RootPath)
									: row.AlternativePath.string();
								ImGui::TextDisabled("Alternative: %s (%d)", alternativePath.c_str(), row.AlternativeScore);
							}
							ImGui::Unindent(18.0f);
						}
						ImGui::TreePop();
					}
				}

				const auto buildMaterialScalarBatchRecords = [&](size_t sourceMaterialIndex, const Asset::StaticMeshMaterial& sourceMaterial)
				{
					std::vector<MaterialEditRecord> records;
					if (!multiInspecting || !context.OnMaterialBatchEditCommitted)
					{
						return records;
					}

					for (EntityId entityId : inspectorSelection)
					{
						if (entityId == selectedEntity)
						{
							continue;
						}

						Asset::StaticMeshAsset* targetMesh = context.ActiveScene.GetMeshAsset(entityId);
						if (!targetMesh || sourceMaterialIndex >= targetMesh->Materials.size())
						{
							continue;
						}

						Asset::StaticMeshMaterial afterMaterial = targetMesh->Materials[sourceMaterialIndex];
						CopyMaterialScalarProperties(afterMaterial, sourceMaterial);
						if (!MaterialScalarPropertiesDiffer(targetMesh->Materials[sourceMaterialIndex], afterMaterial))
						{
							continue;
						}

						records.push_back(MaterialEditRecord{
							.Entity = entityId,
							.MaterialIndex = sourceMaterialIndex,
							.Before = targetMesh->Materials[sourceMaterialIndex],
							.After = std::move(afterMaterial)
						});
					}
					return records;
				};

				for (size_t materialIndex = 0; materialIndex < mesh->Materials.size(); ++materialIndex)
				{
					auto& material = mesh->Materials[materialIndex];
					std::string materialLabel = material.Name.empty() ? "Material" : material.Name;
					const Asset::StaticMeshMaterial* prefabMaterial = nullptr;
					if (prefabOverrideSourceLoaded &&
						prefabOverrideSource.HasMesh &&
						materialIndex < prefabOverrideSource.MaterialOverrides.size())
					{
						prefabMaterial = &prefabOverrideSource.MaterialOverrides[materialIndex];
					}
					const bool materialMissingFromPrefab = prefabOverrideSourceLoaded &&
						prefabOverrideSource.HasMesh &&
						materialIndex >= prefabOverrideSource.MaterialOverrides.size();
					const std::vector<std::string> scalarMaterialOverrides = prefabMaterial
						? CollectMaterialScalarOverrideLabels(material, *prefabMaterial)
						: std::vector<std::string>();
					const std::vector<Asset::MaterialTextureSlot> textureMaterialOverrides = prefabMaterial
						? CollectMaterialTextureOverrideSlots(material, *prefabMaterial)
						: std::vector<Asset::MaterialTextureSlot>();
					const bool materialHasPrefabOverride =
						materialMissingFromPrefab ||
						!scalarMaterialOverrides.empty() ||
						!textureMaterialOverrides.empty();
					if (materialHasPrefabOverride)
					{
						materialLabel.append(" [Override]");
					}
					materialLabel.append("##");
					materialLabel.append(std::to_string(materialIndex));
					const bool focusThisMaterial = hasFocusedMaterial && m_FocusedMaterialIndex == materialIndex;
					if (focusThisMaterial)
					{
						ImGui::SetNextItemOpen(true, ImGuiCond_Always);
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.30f, 1.0f));
					}
					const bool materialOpen = ImGui::TreeNode(materialLabel.c_str());
					if (focusThisMaterial)
					{
						if (ImGui::IsItemVisible())
						{
							ImGui::SetScrollHereY(0.25f);
						}
						ImGui::PopStyleColor();
						if (!m_FocusedMaterialFocusPinned)
						{
							--m_FocusedMaterialHighlightFrames;
							if (m_FocusedMaterialHighlightFrames <= 0)
							{
								m_FocusedMaterialEntity = InvalidEntityId;
								m_FocusedMaterialIndex = static_cast<size_t>(-1);
								m_FocusedMaterialTextureSlot = Asset::MaterialTextureSlot::Count;
								m_FocusedMaterialControl = MeshRestoreMaterialFocusKind::None;
							}
						}
					}
					if (materialOpen)
					{
						if (materialHasPrefabOverride)
						{
							ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "Prefab material override");
							if (materialMissingFromPrefab)
							{
								ImGui::TextWrapped("This material index is not present in the prefab source.");
								ImGui::TextDisabled("Use Apply Current to Prefab for full Mesh/material count changes.");
							}
							else
							{
								if (!scalarMaterialOverrides.empty())
								{
									std::string scalarSummary;
									for (const std::string& label : scalarMaterialOverrides)
									{
										if (!scalarSummary.empty())
										{
											scalarSummary.append(", ");
										}
										scalarSummary.append(label);
									}
									ImGui::TextWrapped("Changed properties: %s", scalarSummary.c_str());
								}
								if (!textureMaterialOverrides.empty())
								{
									std::string textureSummary;
									for (Asset::MaterialTextureSlot slot : textureMaterialOverrides)
									{
										if (!textureSummary.empty())
										{
											textureSummary.append(", ");
										}
										textureSummary.append(Asset::MaterialTextureSlotName(slot));
									}
									ImGui::TextWrapped("Changed texture slots: %s", textureSummary.c_str());
								}
							}

							const bool canRevertMaterial = prefabMaterial && static_cast<bool>(context.OnMaterialEditCommitted);
							ImGui::BeginDisabled(!canRevertMaterial);
							if (ImGui::SmallButton("Revert Material"))
							{
								context.OnMaterialEditCommitted(selectedEntity, materialIndex, material, *prefabMaterial);
							}
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canRevertMaterial
									? "Revert this material to the prefab source value with undo support."
									: "Material revert is unavailable for this row.");
							}
							ImGui::SameLine();
							const bool canApplyMaterial = prefabMaterial &&
								context.OnSavePrefabInspectionRoot &&
								!prefabOverrideSourcePath.empty();
							ImGui::BeginDisabled(!canApplyMaterial);
							if (ImGui::SmallButton("Apply Material To Prefab"))
							{
								ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabOverrideSource;
								updatedPrefabRoot.MaterialOverrides[materialIndex] = material;
								static_cast<void>(context.OnSavePrefabInspectionRoot(prefabOverrideSourcePath, updatedPrefabRoot));
							}
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canApplyMaterial
									? "Write only this material override back to the prefab source."
									: "Per-material apply is unavailable when the source material row is missing.");
							}
							ImGui::Separator();
						}

						int shadingModelIndex = material.ShadingModel == Asset::MaterialShadingModel::PBR
							? 1
							: material.ShadingModel == Asset::MaterialShadingModel::Unlit ? 2 : 0;
						const bool focusShadingModel = focusThisMaterial &&
							m_FocusedMaterialControl == MeshRestoreMaterialFocusKind::ShadingModel &&
							m_FocusedMaterialTextureSlot == Asset::MaterialTextureSlot::Count;
						if (focusShadingModel)
						{
							ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.30f, 1.0f));
						}
						if (ImGui::Combo("Shading Model", &shadingModelIndex, "Phong\0PBR\0Unlit\0"))
						{
							const auto model = shadingModelIndex == 1
								? Asset::MaterialShadingModel::PBR
								: shadingModelIndex == 2 ? Asset::MaterialShadingModel::Unlit : Asset::MaterialShadingModel::Phong;
							if (context.OnMaterialShadingModelChanged)
							{
								context.OnMaterialShadingModelChanged(selectedEntity, materialIndex, model);
							}
						}
						if (focusShadingModel)
						{
							if (ImGui::IsItemVisible())
							{
								ImGui::SetScrollHereY(0.5f);
							}
							ImGui::PopStyleColor();
						}

						auto trackMaterialControl = [&]()
						{
							if (ImGui::IsItemActivated())
							{
								m_MaterialEditingEntity = selectedEntity;
								m_MaterialEditingIndex = materialIndex;
								m_MaterialEditBefore = material;
							}
							if (ImGui::IsItemDeactivatedAfterEdit() &&
								m_MaterialEditingEntity == selectedEntity &&
								m_MaterialEditingIndex == materialIndex)
							{
								if (context.OnMaterialEditCommitted)
								{
									context.OnMaterialEditCommitted(selectedEntity, materialIndex, m_MaterialEditBefore, material);
								}
								m_MaterialEditingEntity = InvalidEntityId;
								m_MaterialEditingIndex = static_cast<size_t>(-1);
								m_MaterialEditBefore = {};
							}
						};
						const auto focusMaterialControl = [&](MeshRestoreMaterialFocusKind kind)
						{
							return focusThisMaterial &&
								m_FocusedMaterialControl == kind &&
								m_FocusedMaterialTextureSlot == Asset::MaterialTextureSlot::Count;
						};
						const auto beginFocusedControl = [&](MeshRestoreMaterialFocusKind kind)
						{
							if (focusMaterialControl(kind))
							{
								ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.30f, 1.0f));
								return true;
							}
							return false;
						};
						const auto endFocusedControl = [](bool focused)
						{
							if (!focused)
							{
								return;
							}
							if (ImGui::IsItemVisible())
							{
								ImGui::SetScrollHereY(0.5f);
							}
							ImGui::PopStyleColor();
						};

						bool materialChanged = false;
						bool focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::BaseColor);
						materialChanged |= ImGui::ColorEdit4("Base Color", &material.DiffuseColor.x);
						endFocusedControl(focusedControl);
						trackMaterialControl();
						focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::UseVertexColor);
						materialChanged |= ImGui::Checkbox("Use Vertex Color", &material.UseVertexColor);
						endFocusedControl(focusedControl);
						trackMaterialControl();
						focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::NormalYFlip);
						materialChanged |= ImGui::Checkbox("Normal Y Flip", &material.NormalYFlip);
						endFocusedControl(focusedControl);
						trackMaterialControl();
						focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::EmissiveColor);
						materialChanged |= ImGui::ColorEdit3("Emissive", &material.EmissiveColor.x);
						endFocusedControl(focusedControl);
						trackMaterialControl();
						focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::Opacity);
						materialChanged |= ImGui::DragFloat("Opacity", &material.Opacity, 0.01f, 0.0f, 1.0f);
						endFocusedControl(focusedControl);
						trackMaterialControl();
						if (material.ShadingModel == Asset::MaterialShadingModel::PBR)
						{
							focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::Metallic);
							materialChanged |= ImGui::DragFloat("Metallic", &material.MetallicFactor, 0.01f, 0.0f, 1.0f);
							endFocusedControl(focusedControl);
							trackMaterialControl();
							focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::Roughness);
							materialChanged |= ImGui::DragFloat("Roughness", &material.RoughnessFactor, 0.01f, 0.02f, 1.0f);
							endFocusedControl(focusedControl);
							trackMaterialControl();
						}
						else if (material.ShadingModel == Asset::MaterialShadingModel::Phong)
						{
							focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::SpecularColor);
							materialChanged |= ImGui::ColorEdit3("Specular", &material.SpecularColor.x);
							endFocusedControl(focusedControl);
							trackMaterialControl();
							focusedControl = beginFocusedControl(MeshRestoreMaterialFocusKind::Shininess);
							materialChanged |= ImGui::DragFloat("Shininess", &material.Shininess, 1.0f, 1.0f, 1024.0f);
							endFocusedControl(focusedControl);
							trackMaterialControl();
						}
						if (materialChanged && context.OnMaterialEdited)
						{
							material.DiffuseColor.x = std::clamp(material.DiffuseColor.x, 0.0f, 1.0f);
							material.DiffuseColor.y = std::clamp(material.DiffuseColor.y, 0.0f, 1.0f);
							material.DiffuseColor.z = std::clamp(material.DiffuseColor.z, 0.0f, 1.0f);
							material.DiffuseColor.w = std::clamp(material.DiffuseColor.w, 0.0f, 1.0f);
							material.Opacity = std::clamp(material.Opacity, 0.0f, 1.0f);
							material.MetallicFactor = std::clamp(material.MetallicFactor, 0.0f, 1.0f);
							material.RoughnessFactor = std::clamp(material.RoughnessFactor, 0.02f, 1.0f);
							material.Shininess = std::clamp(material.Shininess, 1.0f, 1024.0f);
							context.OnMaterialEdited(selectedEntity, materialIndex);
						}

						const std::vector<MaterialEditRecord> materialScalarBatchRecords =
							buildMaterialScalarBatchRecords(materialIndex, material);
						const bool canApplyMaterialScalarsToSelection =
							!materialScalarBatchRecords.empty() &&
							static_cast<bool>(context.OnMaterialBatchEditCommitted);
						if (multiInspecting)
						{
							ImGui::BeginDisabled(!canApplyMaterialScalarsToSelection);
							if (ImGui::SmallButton("Apply Scalars To Selected"))
							{
								context.OnMaterialBatchEditCommitted(materialScalarBatchRecords);
							}
							ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
							{
								ImGui::SetTooltip(canApplyMaterialScalarsToSelection
									? "Copy this material's shading model, colors, scalar factors, vertex-color flag, and normal flip to selected entities with the same material index. Texture slots stay per entity."
									: "No other selected entity has a different material scalar value at this material index.");
							}
							ImGui::SameLine();
							ImGui::TextDisabled(
								"%zu target%s, textures preserved",
								materialScalarBatchRecords.size(),
								materialScalarBatchRecords.size() == 1 ? "" : "s");
						}

						ImGui::Separator();
						ImGui::TextUnformatted("Texture Slots");
						const std::vector<MaterialTextureAssignment> autoRemapAssignments =
							BuildAutoTextureRemapAssignments(context.ProjectSnapshot, material, mesh->SourcePath, textureRemapFilter);
						const bool canAutoRemap =
							!autoRemapAssignments.empty() &&
							static_cast<bool>(context.OnMaterialTexturesAssigned);
						ImGui::BeginDisabled(!canAutoRemap);
						if (ImGui::SmallButton("Auto Remap Missing"))
						{
							context.OnMaterialTexturesAssigned(selectedEntity, materialIndex, autoRemapAssignments);
						}
						ImGui::EndDisabled();
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						{
							if (canAutoRemap)
							{
								ImGui::SetTooltip("Assign the best suggested image candidate for each missing texture slot in this material.");
							}
							else
							{
								ImGui::SetTooltip("No missing texture slots with matching candidates were found.");
							}
						}
						if (!autoRemapAssignments.empty())
						{
							ImGui::SameLine();
							ImGui::TextDisabled("%zu slot%s ready", autoRemapAssignments.size(), autoRemapAssignments.size() == 1 ? "" : "s");
						}
						for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
						{
							const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
							if (ShouldShowMaterialSlot(material.ShadingModel, slot))
							{
								const bool focusThisSlot = focusThisMaterial && m_FocusedMaterialTextureSlot == slot;
								DrawMaterialTextureSlotRow(context, selectedEntity, materialIndex, material, slot, focusThisSlot, mesh->SourcePath, textureRemapFilter);
								if (prefabMaterial && MaterialTextureSlotDiffers(material, *prefabMaterial, slot))
								{
									ImGui::SameLine();
									ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "[Override]");
									if (ImGui::IsItemHovered())
									{
										ImGui::BeginTooltip();
										ImGui::Text("%s texture differs from prefab source.", std::string(Asset::MaterialTextureSlotName(slot)).c_str());
										ImGui::Separator();
										ImGui::TextWrapped("Current: %s", MaterialTextureSourceText(material, slot).c_str());
										ImGui::TextWrapped("Prefab:  %s", MaterialTextureSourceText(*prefabMaterial, slot).c_str());
										ImGui::EndTooltip();
									}
								}
							}
						}
						ImGui::TreePop();
					}
				}
				if (prefabOverrideSourceLoaded &&
					prefabOverrideSource.HasMesh &&
					prefabOverrideSource.MaterialOverrides.size() > mesh->Materials.size())
				{
					ImGui::Separator();
					ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "Prefab has %zu additional material row(s).",
						prefabOverrideSource.MaterialOverrides.size() - mesh->Materials.size());
					ImGui::TextDisabled("Missing current material rows are protected; use Apply Current to Prefab only when replacing the whole prefab material set is intended.");
				}
			}
		}

		if (const PrefabInstanceComponent* prefab = context.ActiveScene.GetPrefabInstanceComponent(selectedEntity);
			prefab && (inspectorFilter.empty() || ContainsCaseInsensitive("Prefab Overrides Diff Instance Source Modified Added Removed", inspectorFilter)))
		{
			DrawPrefabOverrideDiff(context, selectedEntity, *prefab);
		}

		DrawReflectionQuickEdit(
			context,
			selectedEntity,
			inspectorDisplayOrder,
			inspectorSelection,
			inspectorFilter,
			prefabOverrideSourceLoaded ? &prefabOverrideSource : nullptr);
		DrawReflectionSchema(context, selectedEntity, inspectorDisplayOrder, inspectorFilter);

		ImGui::End();
	}

	void EditorLayer::DrawPrefabOverrideDiff(EditorContext& context, EntityId entityId, const PrefabInstanceComponent& prefab)
	{
		if (!ImGui::CollapsingHeader("Prefab Overrides"))
		{
			return;
		}

		if (prefab.PrefabPath.empty())
		{
			ImGui::TextDisabled("This Entity has a Prefab Instance component, but no prefab path is assigned.");
			return;
		}
		if (!prefab.TrackPrefabOverrides)
		{
			ImGui::TextDisabled("Override tracking is disabled on this Prefab Instance.");
			return;
		}
		if (!context.OnLoadPrefabForInspection)
		{
			ImGui::TextDisabled("Prefab inspection is not available in this editor context.");
			return;
		}

		ScenePersistence::LoadedSceneEntity prefabRoot;
		std::string errorMessage;
		if (!context.OnLoadPrefabForInspection(prefab.PrefabPath, prefabRoot, errorMessage))
		{
			ImGui::TextWrapped("Prefab load failed: %s", errorMessage.empty() ? "<unknown>" : errorMessage.c_str());
			ImGui::TextWrapped("Path: %s", prefab.PrefabPath.string().c_str());
			return;
		}

		struct OverrideRow
		{
			SceneComponentKind Kind = SceneComponentKind::Mesh;
			std::string Component;
			std::string Property;
			std::string PropertyName;
			std::string CurrentValue;
			std::string PrefabValue;
			bool RevertName = false;
			bool RevertComponent = false;
			bool CanRevert = false;
			bool CanApply = false;
		};

		struct MaterialOverrideRow
		{
			size_t MaterialIndex = static_cast<size_t>(-1);
			Asset::MaterialTextureSlot TextureSlot = Asset::MaterialTextureSlot::Count;
			std::string Difference;
			std::string CurrentValue;
			std::string PrefabValue;
			bool CanFocus = false;
		};

		std::vector<OverrideRow> rows;
		rows.reserve(32);
		std::vector<MaterialOverrideRow> materialRows;
		materialRows.reserve(32);
		std::vector<SceneComponentKind> revertableComponents;
		const auto addRevertableComponent = [&revertableComponents](SceneComponentKind kind)
		{
			if (!CanRevertPrefabOverrideComponent(kind))
			{
				return;
			}
			if (std::ranges::find(revertableComponents, kind) == revertableComponents.end())
			{
				revertableComponents.push_back(kind);
			}
		};

		const std::string* currentName = context.ActiveScene.GetEntityName(entityId);
		const std::string currentNameText = currentName && !currentName->empty() ? *currentName : std::string("<unnamed>");
		const bool nameOverridden = currentNameText != prefabRoot.Name;
		if (nameOverridden)
		{
			rows.push_back(OverrideRow{
				.Kind = SceneComponentKind::Mesh,
				.Component = "Entity",
				.Property = "Name",
				.PropertyName = "Name",
				.CurrentValue = currentNameText,
				.PrefabValue = prefabRoot.Name,
				.RevertName = true,
				.CanRevert = static_cast<bool>(context.OnRenameEntity),
				.CanApply = static_cast<bool>(context.OnSavePrefabInspectionRoot)
			});
		}

		for (const Reflection::ComponentDescriptor& descriptor : Reflection::GetSceneComponentDescriptors())
		{
			ScenePersistence::LoadedSceneEntity currentSnapshot;
			const bool currentHas = CaptureComponentSnapshot(context.ActiveScene, entityId, descriptor.Kind, currentSnapshot);
			const bool prefabHas = SnapshotHasComponent(descriptor.Kind, prefabRoot);
			if (!currentHas && !prefabHas)
			{
				continue;
			}
			if (currentHas != prefabHas)
			{
				addRevertableComponent(descriptor.Kind);
				rows.push_back(OverrideRow{
					.Kind = descriptor.Kind,
					.Component = std::string(descriptor.Name),
					.Property = "<component>",
					.PropertyName = "<component>",
					.CurrentValue = currentHas ? "Present" : "Missing",
					.PrefabValue = prefabHas ? "Present" : "Missing",
					.RevertComponent = true,
					.CanRevert = CanRevertPrefabOverrideComponent(descriptor.Kind),
					.CanApply = CanRevertPrefabOverrideComponent(descriptor.Kind) && static_cast<bool>(context.OnSavePrefabInspectionRoot)
				});
				continue;
			}

			bool currentEnabled = true;
			bool prefabEnabled = true;
			if (SnapshotComponentEnabled(descriptor.Kind, currentSnapshot, currentEnabled) &&
				SnapshotComponentEnabled(descriptor.Kind, prefabRoot, prefabEnabled) &&
				currentEnabled != prefabEnabled)
			{
				addRevertableComponent(descriptor.Kind);
				rows.push_back(OverrideRow{
					.Kind = descriptor.Kind,
					.Component = std::string(descriptor.Name),
					.Property = "Component Enabled",
					.PropertyName = "Component Enabled",
					.CurrentValue = FormatBool(currentEnabled),
					.PrefabValue = FormatBool(prefabEnabled),
					.CanRevert = CanRevertPrefabOverrideComponent(descriptor.Kind),
					.CanApply = CanRevertPrefabOverrideComponent(descriptor.Kind) && static_cast<bool>(context.OnSavePrefabInspectionRoot)
				});
			}

			for (const Reflection::PropertyDescriptor& property : descriptor.Properties)
			{
				const std::string currentValue = SnapshotPropertyValue(descriptor.Kind, property.Name, currentSnapshot);
				const std::string prefabValue = SnapshotPropertyValue(descriptor.Kind, property.Name, prefabRoot);
				if (currentValue != prefabValue)
				{
					addRevertableComponent(descriptor.Kind);
					rows.push_back(OverrideRow{
						.Kind = descriptor.Kind,
						.Component = std::string(descriptor.Name),
						.Property = std::string(property.Name),
						.PropertyName = std::string(property.Name),
						.CurrentValue = currentValue.empty() ? "<empty>" : currentValue,
						.PrefabValue = prefabValue.empty() ? "<empty>" : prefabValue,
						.CanRevert = CanRevertPrefabOverrideComponent(descriptor.Kind),
						.CanApply = CanRevertPrefabOverrideComponent(descriptor.Kind) && static_cast<bool>(context.OnSavePrefabInspectionRoot)
					});
				}
			}
		}

		if (const Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(entityId);
			mesh && prefabRoot.HasMesh)
		{
			const size_t currentMaterialCount = mesh->Materials.size();
			const size_t prefabMaterialCount = prefabRoot.MaterialOverrides.size();
			const size_t materialCount = (std::max)(currentMaterialCount, prefabMaterialCount);
			for (size_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
			{
				if (materialIndex >= currentMaterialCount)
				{
					materialRows.push_back(MaterialOverrideRow{
						.MaterialIndex = materialIndex,
						.Difference = "Missing current material row",
						.CurrentValue = "<missing>",
						.PrefabValue = materialIndex < prefabMaterialCount && !prefabRoot.MaterialOverrides[materialIndex].Name.empty()
							? prefabRoot.MaterialOverrides[materialIndex].Name
							: std::string("<unnamed>"),
						.CanFocus = false
					});
					continue;
				}
				if (materialIndex >= prefabMaterialCount)
				{
					materialRows.push_back(MaterialOverrideRow{
						.MaterialIndex = materialIndex,
						.Difference = "Extra current material row",
						.CurrentValue = mesh->Materials[materialIndex].Name.empty() ? std::string("<unnamed>") : mesh->Materials[materialIndex].Name,
						.PrefabValue = "<missing>",
						.CanFocus = true
					});
					continue;
				}

				const Asset::StaticMeshMaterial& currentMaterial = mesh->Materials[materialIndex];
				const Asset::StaticMeshMaterial& prefabMaterial = prefabRoot.MaterialOverrides[materialIndex];
				const std::vector<std::string> scalarLabels = CollectMaterialScalarOverrideLabels(currentMaterial, prefabMaterial);
				if (!scalarLabels.empty())
				{
					std::string summary;
					for (const std::string& label : scalarLabels)
					{
						if (!summary.empty())
						{
							summary.append(", ");
						}
						summary.append(label);
					}
					materialRows.push_back(MaterialOverrideRow{
						.MaterialIndex = materialIndex,
						.Difference = "Scalar values",
						.CurrentValue = summary,
						.PrefabValue = "Prefab scalar values differ",
						.CanFocus = true
					});
				}

				for (Asset::MaterialTextureSlot slot : CollectMaterialTextureOverrideSlots(currentMaterial, prefabMaterial))
				{
					materialRows.push_back(MaterialOverrideRow{
						.MaterialIndex = materialIndex,
						.TextureSlot = slot,
						.Difference = std::format("Texture: {}", Asset::MaterialTextureSlotName(slot)),
						.CurrentValue = MaterialTextureSourceText(currentMaterial, slot),
						.PrefabValue = MaterialTextureSourceText(prefabMaterial, slot),
						.CanFocus = true
					});
				}
			}
		}

		ImGui::TextWrapped("Source: %s", prefab.PrefabPath.string().c_str());
		ImGui::Text("Overrides: %zu", rows.size() + materialRows.size());
		ImGui::BeginDisabled(!context.OnApplyEntityToPrefab);
		if (ImGui::Button("Apply Current to Prefab"))
		{
			static_cast<void>(context.OnApplyEntityToPrefab(entityId));
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Writes the current Entity back to the prefab source. The PrefabInstance link itself is not written into the prefab asset.");
		}
		if (rows.empty() && materialRows.empty())
		{
			ImGui::TextDisabled("No reflected property overrides were detected.");
			return;
		}

		const auto revertComponent = [&context, entityId, &prefabRoot](SceneComponentKind kind)
		{
			if (!CanRevertPrefabOverrideComponent(kind))
			{
				return;
			}
			if (SnapshotHasComponent(kind, prefabRoot))
			{
				if (context.OnComponentPaste)
				{
					context.OnComponentPaste(entityId, kind, prefabRoot);
				}
			}
			else if (context.OnComponentRemoved)
			{
				context.OnComponentRemoved(entityId, kind);
			}
		};
		const auto revertRow = [&context, entityId, &prefabRoot, &revertComponent](const OverrideRow& row)
		{
			if (!row.CanRevert)
			{
				return;
			}
			if (row.RevertName)
			{
				if (context.OnRenameEntity)
				{
					context.OnRenameEntity(entityId, prefabRoot.Name);
				}
				return;
			}
			if (row.RevertComponent)
			{
				revertComponent(row.Kind);
				return;
			}
			if (!context.OnComponentPaste || !CanRevertPrefabOverrideComponent(row.Kind))
			{
				return;
			}

			ScenePersistence::LoadedSceneEntity currentSnapshot;
			if (!CaptureComponentSnapshot(context.ActiveScene, entityId, row.Kind, currentSnapshot))
			{
				return;
			}
			if (CopySnapshotPropertyFromSource(row.Kind, row.PropertyName, currentSnapshot, prefabRoot))
			{
				context.OnComponentPaste(entityId, row.Kind, currentSnapshot);
			}
		};
		const auto applyRow = [&context, entityId, &prefab, &prefabRoot, &currentNameText](const OverrideRow& row)
		{
			if (!row.CanApply || !context.OnSavePrefabInspectionRoot)
			{
				return;
			}

			ScenePersistence::LoadedSceneEntity updatedPrefabRoot = prefabRoot;
			if (row.RevertName)
			{
				updatedPrefabRoot.Name = currentNameText;
				static_cast<void>(context.OnSavePrefabInspectionRoot(prefab.PrefabPath, updatedPrefabRoot));
				return;
			}
			if (row.RevertComponent)
			{
				ScenePersistence::LoadedSceneEntity currentSnapshot;
				const bool currentHas = CaptureComponentSnapshot(context.ActiveScene, entityId, row.Kind, currentSnapshot);
				const bool changed = currentHas
					? CopySnapshotComponentFromSource(row.Kind, updatedPrefabRoot, currentSnapshot)
					: RemoveSnapshotComponent(row.Kind, updatedPrefabRoot);
				if (changed)
				{
					static_cast<void>(context.OnSavePrefabInspectionRoot(prefab.PrefabPath, updatedPrefabRoot));
				}
				return;
			}

			ScenePersistence::LoadedSceneEntity currentSnapshot;
			if (!CaptureComponentSnapshot(context.ActiveScene, entityId, row.Kind, currentSnapshot))
			{
				return;
			}
			if (CopySnapshotPropertyFromSource(row.Kind, row.PropertyName, updatedPrefabRoot, currentSnapshot))
			{
				static_cast<void>(context.OnSavePrefabInspectionRoot(prefab.PrefabPath, updatedPrefabRoot));
			}
		};

		if (nameOverridden && context.OnRenameEntity)
		{
			if (ImGui::Button("Revert Name"))
			{
				context.OnRenameEntity(entityId, prefabRoot.Name);
			}
			ImGui::SameLine();
		}
		ImGui::BeginDisabled(revertableComponents.empty() && !nameOverridden);
		if (ImGui::Button("Revert All Supported"))
		{
			if (nameOverridden && context.OnRenameEntity)
			{
				context.OnRenameEntity(entityId, prefabRoot.Name);
			}
			for (SceneComponentKind kind : revertableComponents)
			{
				revertComponent(kind);
			}
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Reverts reflected values except Mesh and the PrefabInstance link itself.");
		}

		if (!revertableComponents.empty())
		{
			ImGui::TextDisabled("Component revert:");
			for (SceneComponentKind kind : revertableComponents)
			{
				const Reflection::ComponentDescriptor* descriptor = Reflection::FindSceneComponentDescriptor(kind);
				const std::string label = std::format("Revert {}##PrefabOverrideRevert{}",
					descriptor ? std::string(descriptor->Name) : ComponentKindName(kind),
					static_cast<int>(std::to_underlying(kind)));
				if (ImGui::SmallButton(label.c_str()))
				{
					revertComponent(kind);
				}
				ImGui::SameLine();
			}
			ImGui::NewLine();
		}
		if (std::ranges::any_of(rows, [](const OverrideRow& row)
			{
				return row.Component == "Mesh" || row.Component == "Prefab Instance";
			}))
		{
			ImGui::TextDisabled("Mesh and PrefabInstance link overrides are shown for diagnosis but protected from Revert v1.");
		}

		if (!rows.empty() && ImGui::BeginTable("PrefabOverrideDiffTable", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch, 0.18f);
			ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.18f);
			ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthStretch, 0.28f);
			ImGui::TableSetupColumn("Prefab", ImGuiTableColumnFlags_WidthStretch, 0.28f);
			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 126.0f);
			ImGui::TableHeadersRow();

			const size_t maxRows = (std::min)(rows.size(), static_cast<size_t>(128));
			for (size_t rowIndex = 0; rowIndex < maxRows; ++rowIndex)
			{
				const OverrideRow& row = rows[rowIndex];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextWrapped("%s", row.Component.c_str());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", row.Property.c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextWrapped("%s", row.CurrentValue.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextWrapped("%s", row.PrefabValue.c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::BeginDisabled(!row.CanRevert);
				ImGui::PushID(static_cast<int>(rowIndex));
				if (ImGui::SmallButton("Revert"))
				{
					revertRow(row);
				}
				ImGui::SameLine();
				ImGui::EndDisabled();
				ImGui::BeginDisabled(!row.CanApply);
				if (ImGui::SmallButton("Apply"))
				{
					applyRow(row);
				}
				ImGui::PopID();
				ImGui::EndDisabled();
			}
			ImGui::EndTable();
			if (rows.size() > maxRows)
			{
				ImGui::TextDisabled("Showing first %zu override rows.", maxRows);
			}
		}

		if (!materialRows.empty())
		{
			ImGui::Separator();
			if (ImGui::TreeNodeEx("Material Overrides", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::TextDisabled("Material rows are protected from direct Revert here. Use Focus to jump to the full Materials editor.");
				if (ImGui::BeginTable("PrefabMaterialOverrideDiffTable", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 80.0f);
					ImGui::TableSetupColumn("Difference", ImGuiTableColumnFlags_WidthStretch, 0.22f);
					ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthStretch, 0.32f);
					ImGui::TableSetupColumn("Prefab", ImGuiTableColumnFlags_WidthStretch, 0.32f);
					ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 74.0f);
					ImGui::TableHeadersRow();

					const size_t maxMaterialRows = (std::min)(materialRows.size(), static_cast<size_t>(128));
					for (size_t rowIndex = 0; rowIndex < maxMaterialRows; ++rowIndex)
					{
						const MaterialOverrideRow& row = materialRows[rowIndex];
						ImGui::PushID(static_cast<int>(rowIndex));
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text("#%zu", row.MaterialIndex);
						ImGui::TableSetColumnIndex(1);
						ImGui::TextWrapped("%s", row.Difference.c_str());
						ImGui::TableSetColumnIndex(2);
						ImGui::TextWrapped("%s", row.CurrentValue.c_str());
						ImGui::TableSetColumnIndex(3);
						ImGui::TextWrapped("%s", row.PrefabValue.c_str());
						ImGui::TableSetColumnIndex(4);
						ImGui::BeginDisabled(!row.CanFocus);
						if (ImGui::SmallButton("Focus"))
						{
							m_FocusedMaterialEntity = entityId;
							m_FocusedMaterialIndex = row.MaterialIndex;
							m_FocusedMaterialTextureSlot = row.TextureSlot;
							m_FocusedMaterialControl = MeshRestoreMaterialFocusKind::None;
							m_FocusedMaterialHighlightFrames = 180;
							m_FocusedMaterialFocusPinned = false;
						}
						ImGui::EndDisabled();
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						{
							ImGui::SetTooltip(row.CanFocus
								? "Open and highlight this material override in the full Materials section."
								: "This material row does not exist on the current Entity.");
						}
						ImGui::PopID();
					}
					ImGui::EndTable();

					if (materialRows.size() > maxMaterialRows)
					{
						ImGui::TextDisabled("Showing first %zu material override rows.", maxMaterialRows);
					}
				}
				ImGui::TreePop();
			}
		}
	}

	void EditorLayer::DrawReflectionQuickEdit(
		EditorContext& context,
		EntityId entityId,
		const std::vector<SceneComponentKind>& componentOrder,
		const std::vector<EntityId>& inspectorSelection,
		std::string_view inspectorFilter,
		const ScenePersistence::LoadedSceneEntity* prefabOverrideSource)
	{
		auto descriptorMatchesFilter = [inspectorFilter](const Reflection::ComponentDescriptor& descriptor)
		{
			if (inspectorFilter.empty() ||
				ContainsCaseInsensitive("Reflection Quick Edit Property Drawer Descriptor Metadata", inspectorFilter) ||
				ContainsCaseInsensitive(descriptor.Name, inspectorFilter))
			{
				return true;
			}
			for (const Reflection::PropertyDescriptor& property : descriptor.Properties)
			{
				if (ContainsCaseInsensitive(property.Name, inspectorFilter) ||
					ContainsCaseInsensitive(Reflection::ToString(property.ValueKind), inspectorFilter))
				{
					return true;
				}
			}
			return false;
		};

		auto supportsQuickEdit = [](SceneComponentKind kind) noexcept
		{
			switch (kind)
			{
			case SceneComponentKind::Mesh:
			case SceneComponentKind::Animator:
			case SceneComponentKind::Camera:
			case SceneComponentKind::Light:
			case SceneComponentKind::RigidBody:
			case SceneComponentKind::Collider:
			case SceneComponentKind::PhysicsMaterial:
			case SceneComponentKind::PrefabInstance:
			case SceneComponentKind::SceneReference:
			case SceneComponentKind::Script:
			case SceneComponentKind::Sprite2D:
			case SceneComponentKind::UiElement:
			case SceneComponentKind::AudioSource:
			case SceneComponentKind::NavigationAgent:
			case SceneComponentKind::NetworkIdentity:
				return true;
			default:
				return false;
			}
		};

		auto requiresPhysicsActorRebuild = [](SceneComponentKind kind) noexcept
		{
			return kind == SceneComponentKind::RigidBody ||
				kind == SceneComponentKind::Collider ||
				kind == SceneComponentKind::PhysicsMaterial;
		};

		std::vector<const Reflection::ComponentDescriptor*> visibleDescriptors;
		visibleDescriptors.reserve(componentOrder.size());
		for (const SceneComponentKind kind : componentOrder)
		{
			if (!supportsQuickEdit(kind) || !HasInspectableComponent(context.ActiveScene, entityId, kind))
			{
				continue;
			}

			const Reflection::ComponentDescriptor* descriptor = Reflection::FindSceneComponentDescriptor(kind);
			if (descriptor && descriptorMatchesFilter(*descriptor))
			{
				visibleDescriptors.push_back(descriptor);
			}
		}

		const bool shouldShowForFilter = inspectorFilter.empty() ||
			ContainsCaseInsensitive("Reflection Quick Edit Property Drawer Descriptor Metadata", inspectorFilter) ||
			!visibleDescriptors.empty();
		if (!shouldShowForFilter)
		{
			return;
		}

		if (!ImGui::CollapsingHeader("Reflection Quick Edit"))
		{
			return;
		}

		ImGui::TextDisabled("Experimental v1: descriptor-driven edits for safe data components. Drop Project assets onto path fields.");
		if (visibleDescriptors.empty())
		{
			ImGui::TextDisabled(inspectorFilter.empty()
				? "No quick-editable reflected components are available for this Entity."
				: "No quick-editable reflected component matches the current Inspector search.");
			return;
		}

		auto drawAssetPathField = [this, &context](const char* label, std::filesystem::path& path, std::initializer_list<std::string_view> allowedExtensions)
		{
			auto extensionAllowed = [allowedExtensions](const std::filesystem::path& candidate)
			{
				if (allowedExtensions.size() == 0)
				{
					return true;
				}

				const std::string extension = ToLower(candidate.extension().string());
				return std::ranges::any_of(allowedExtensions, [&extension](std::string_view allowed)
					{
						return extension == allowed;
					});
			};

			bool changed = false;
			ImGui::TextUnformatted(label);
			ImGui::SameLine();
			const auto buttonWidth = [](const char* text)
			{
				return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
			};
			const float pickButtonWidth = buttonWidth("Pick");
			const float clearButtonWidth = path.empty() ? 0.0f : buttonWidth("Clear");
			ImGui::SetNextItemWidth(-(pickButtonWidth + clearButtonWidth));

			std::array<char, 512> buffer = {};
			const std::string displayPath = path.empty() ? std::string("<not assigned>") : path.string();
			const size_t copyLength = (std::min)(displayPath.size(), buffer.size() - 1);
			std::copy_n(displayPath.data(), copyLength, buffer.data());
			ImGui::InputText("##AssetPath", buffer.data(), buffer.size(), ImGuiInputTextFlags_ReadOnly);
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathPayload))
				{
					if (payload->Data && payload->DataSize > 0)
					{
						const char* pathText = static_cast<const char*>(payload->Data);
						const std::filesystem::path droppedPath = std::filesystem::path(pathText).lexically_normal();
						if (extensionAllowed(droppedPath))
						{
							path = droppedPath;
							changed = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted("Drop a Project asset here.");
				if (allowedExtensions.size() > 0)
				{
					std::string extensions;
					for (std::string_view extension : allowedExtensions)
					{
						if (!extensions.empty())
						{
							extensions.append(", ");
						}
						extensions.append(extension);
					}
					ImGui::Text("Accepted: %s", extensions.c_str());
				}
				ImGui::EndTooltip();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Pick"))
			{
				ImGui::OpenPopup("AssetPathPicker");
			}
			if (!path.empty())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear"))
				{
					path.clear();
					changed = true;
				}
			}
			if (ImGui::BeginPopup("AssetPathPicker"))
			{
				ImGui::Text("Pick %s", label);
				ImGui::SetNextItemWidth(320.0f);
				ImGui::InputText("Search", m_ReflectionAssetPickerFilter.data(), m_ReflectionAssetPickerFilter.size());
				const std::string_view filter = TextFilter(m_ReflectionAssetPickerFilter);

				const auto snapshot = context.ProjectSnapshot;
				if (!snapshot)
				{
					ImGui::TextDisabled("Project snapshot is loading.");
				}
				else if (!snapshot->RootExists)
				{
					ImGui::TextWrapped("%s", snapshot->Status.c_str());
				}
				else
				{
					struct AssetPickerRow
					{
						const Asset::AssetFileEntry* Entry = nullptr;
						std::string RelativePath;
					};
					std::vector<AssetPickerRow> rows;
					rows.reserve(64);

					const auto collectRows = [&](const auto& self, const Asset::AssetFileEntry& entry) -> void
						{
							if (entry.Kind != Asset::AssetFileKind::Directory && extensionAllowed(entry.Path))
							{
								const std::string relativePath = RelativeDisplayPath(entry.Path, snapshot->RootPath);
								const std::string searchText = entry.Name + " " + relativePath + " " + entry.Extension;
								if (ContainsCaseInsensitive(searchText, filter))
								{
									rows.push_back(AssetPickerRow{ .Entry = &entry, .RelativePath = relativePath });
								}
							}
							for (const Asset::AssetFileEntry& child : entry.Children)
							{
								self(self, child);
							}
						};
					for (const Asset::AssetFileEntry& entry : snapshot->Children)
					{
						collectRows(collectRows, entry);
					}
					std::ranges::sort(rows, [](const AssetPickerRow& lhs, const AssetPickerRow& rhs)
						{
							return lhs.RelativePath < rhs.RelativePath;
						});

					ImGui::Text("%zu matching asset(s)", rows.size());
					ImGui::Separator();
					if (ImGui::BeginChild("AssetPathPickerResults", ImVec2(420.0f, 260.0f), true))
					{
						if (rows.empty())
						{
							ImGui::TextDisabled(filter.empty() ? "No matching assets found." : "No assets match the current search.");
						}
						const size_t maxRows = (std::min)(rows.size(), static_cast<size_t>(256));
						for (size_t rowIndex = 0; rowIndex < maxRows; ++rowIndex)
						{
							const AssetPickerRow& row = rows[rowIndex];
							const bool selected = row.Entry && SamePath(path, row.Entry->Path);
							ImGui::PushID(static_cast<int>(rowIndex));
							if (ImGui::Selectable(row.RelativePath.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
							{
								path = row.Entry->Path.lexically_normal();
								changed = true;
								ImGui::CloseCurrentPopup();
							}
							ImGui::PopID();
						}
					}
					ImGui::EndChild();
				}
				ImGui::EndPopup();
			}
			return changed;
		};

		auto drawEditableString = [](const char* label, std::string& value)
		{
			std::array<char, 256> buffer = {};
			const size_t copyLength = (std::min)(value.size(), buffer.size() - 1);
			std::copy_n(value.data(), copyLength, buffer.data());
			if (ImGui::InputText(label, buffer.data(), buffer.size()))
			{
				value = buffer.data();
				return true;
			}
			if (value.size() >= buffer.size() - 1)
			{
				ImGui::TextDisabled("Value is truncated in this v1 editor field.");
			}
			return false;
		};

		const auto buildQuickMaterialScalarBatchRecords = [&](size_t sourceMaterialIndex, const Asset::StaticMeshMaterial& sourceMaterial)
		{
			std::vector<MaterialEditRecord> records;
			if (!context.OnMaterialBatchEditCommitted)
			{
				return records;
			}

			for (EntityId selected : inspectorSelection)
			{
				if (selected == entityId || selected == InvalidEntityId || !context.ActiveScene.ContainsEntity(selected))
				{
					continue;
				}

				Asset::StaticMeshAsset* targetMesh = context.ActiveScene.GetMeshAsset(selected);
				if (!targetMesh || sourceMaterialIndex >= targetMesh->Materials.size())
				{
					continue;
				}

				Asset::StaticMeshMaterial afterMaterial = targetMesh->Materials[sourceMaterialIndex];
				CopyMaterialScalarProperties(afterMaterial, sourceMaterial);
				if (!MaterialScalarPropertiesDiffer(targetMesh->Materials[sourceMaterialIndex], afterMaterial))
				{
					continue;
				}

				records.push_back(MaterialEditRecord{
					.Entity = selected,
					.MaterialIndex = sourceMaterialIndex,
					.Before = targetMesh->Materials[sourceMaterialIndex],
					.After = std::move(afterMaterial)
				});
			}
			return records;
		};

		bool anyChanged = false;
		for (const Reflection::ComponentDescriptor* descriptor : visibleDescriptors)
		{
			ImGui::PushID(static_cast<int>(std::to_underlying(descriptor->Kind)));
			const bool enabled = IsInspectableComponentEnabled(context.ActiveScene, entityId, descriptor->Kind);
			const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
			const bool open = ImGui::TreeNodeEx("##QuickEditComponent", flags, "%.*s%s",
				static_cast<int>(descriptor->Name.size()),
				descriptor->Name.data(),
				enabled ? "" : " (disabled)");
			if (open)
			{
				ImGui::BeginDisabled(!enabled);
				bool componentChanged = false;
				ScenePersistence::LoadedSceneEntity quickEditCurrentSnapshot;
				const bool canComparePrefabProperties =
					prefabOverrideSource &&
					SnapshotHasComponent(descriptor->Kind, *prefabOverrideSource) &&
					CaptureComponentSnapshot(context.ActiveScene, entityId, descriptor->Kind, quickEditCurrentSnapshot);

				for (const Reflection::PropertyDescriptor& property : descriptor->Properties)
				{
					ImGui::PushID(property.Name.data());
					const std::string label(property.Name);
					const std::string prefabCurrentValue = canComparePrefabProperties
						? SnapshotPropertyValue(descriptor->Kind, property.Name, quickEditCurrentSnapshot)
						: std::string();
					const std::string prefabSourceValue = canComparePrefabProperties
						? SnapshotPropertyValue(descriptor->Kind, property.Name, *prefabOverrideSource)
						: std::string();
					const bool propertyHasPrefabOverride =
						canComparePrefabProperties &&
						prefabCurrentValue != prefabSourceValue;
					switch (descriptor->Kind)
					{
					case SceneComponentKind::Mesh:
						if (MeshComponent* meshComponent = context.ActiveScene.GetMeshComponent(entityId))
						{
							Asset::StaticMeshAsset* meshAsset = meshComponent->Asset.get();
							if (!meshAsset)
							{
								ImGui::TextDisabled("%s: empty MeshComponent", label.c_str());
								break;
							}

							if (property.Name == "Asset")
							{
								ImGui::TextUnformatted("Asset");
								ImGui::SameLine();
								std::array<char, 512> buffer = {};
								const std::string assetPath = meshAsset->SourcePath.empty()
									? std::string("<generated/runtime mesh>")
									: meshAsset->SourcePath.string();
								const size_t copyLength = (std::min)(assetPath.size(), buffer.size() - 1);
								std::copy_n(assetPath.data(), copyLength, buffer.data());
								ImGui::SetNextItemWidth(-140.0f);
								ImGui::InputText("##MeshAssetPath", buffer.data(), buffer.size(), ImGuiInputTextFlags_ReadOnly);
								if (!meshAsset->SourcePath.empty())
								{
									ImGui::SameLine();
									if (ImGui::SmallButton("Open") && context.OnAssetOpen)
									{
										context.OnAssetOpen(meshAsset->SourcePath);
									}
									ImGui::SameLine();
									if (ImGui::SmallButton("Reveal") && context.OnAssetReveal)
									{
										context.OnAssetReveal(meshAsset->SourcePath);
									}
								}
							}
							else if (property.Name == "PrimitiveKind")
							{
								ImGui::Text("%s: %s", label.c_str(), PrimitiveMeshKindName(meshAsset->PrimitiveKind));
							}
							else if (property.Name == "VertexCount")
							{
								ImGui::Text("%s: %zu", label.c_str(), meshAsset->Vertices.size());
							}
							else if (property.Name == "IndexCount")
							{
								ImGui::Text("%s: %zu", label.c_str(), meshAsset->Indices.size());
							}
							else if (property.Name == "SubmeshCount")
							{
								ImGui::Text("%s: %zu", label.c_str(), meshAsset->Submeshes.size());
							}
							else if (property.Name == "MaterialCount")
							{
								ImGui::Text("%s: %zu", label.c_str(), meshAsset->Materials.size());
							}
							else if (property.Name == "AnimationCount")
							{
								ImGui::Text("%s: %zu", label.c_str(), meshAsset->Animations.size());
							}
							else if (property.Name == "Materials")
							{
								if (meshAsset->Materials.empty())
								{
									ImGui::TextDisabled("Materials: none");
									break;
								}

								ImGui::Text("Materials: %zu", meshAsset->Materials.size());
								if (ImGui::BeginTable("QuickEditMeshMaterials", 6, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
								{
									ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36.0f);
									ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.28f);
									ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, 72.0f);
									ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 54.0f);
									ImGui::TableSetupColumn("Textures", ImGuiTableColumnFlags_WidthStretch, 0.22f);
									ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 64.0f);
									ImGui::TableHeadersRow();

									const size_t maxMaterialRows = (std::min)(meshAsset->Materials.size(), static_cast<size_t>(64));
									for (size_t materialIndex = 0; materialIndex < maxMaterialRows; ++materialIndex)
									{
										const Asset::StaticMeshMaterial& material = meshAsset->Materials[materialIndex];
										size_t sourcedSlotCount = 0;
										std::string sourcedSlotSummary;
										for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
										{
											const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
											if (Asset::GetMaterialTextureBinding(material, slot).HasSource())
											{
												++sourcedSlotCount;
												if (!sourcedSlotSummary.empty())
												{
													sourcedSlotSummary.append(", ");
												}
												sourcedSlotSummary.append(Asset::MaterialTextureSlotName(slot));
											}
										}

										ImGui::PushID(static_cast<int>(materialIndex));
										ImGui::TableNextRow();
										ImGui::TableSetColumnIndex(0);
										ImGui::Text("%zu", materialIndex);
										ImGui::TableSetColumnIndex(1);
										ImGui::TextUnformatted(material.Name.empty() ? "<unnamed>" : material.Name.c_str());
										ImGui::TableSetColumnIndex(2);
										ImGui::TextUnformatted(std::string(Asset::MaterialShadingModelName(material.ShadingModel)).c_str());
										ImGui::TableSetColumnIndex(3);
										ImGui::ColorButton(
											"##MaterialBaseColor",
											ImVec4(material.DiffuseColor.x, material.DiffuseColor.y, material.DiffuseColor.z, material.DiffuseColor.w),
											ImGuiColorEditFlags_NoTooltip,
											ImVec2(34.0f, 16.0f));
										if (ImGui::IsItemHovered())
										{
											ImGui::SetTooltip(
												"Base Color: %.3f, %.3f, %.3f, %.3f",
												material.DiffuseColor.x,
												material.DiffuseColor.y,
												material.DiffuseColor.z,
												material.DiffuseColor.w);
										}
										ImGui::TableSetColumnIndex(4);
										ImGui::Text("%zu slot%s", sourcedSlotCount, sourcedSlotCount == 1 ? "" : "s");
										if (ImGui::IsItemHovered() && !sourcedSlotSummary.empty())
										{
											ImGui::SetTooltip("%s", sourcedSlotSummary.c_str());
										}
										ImGui::TableSetColumnIndex(5);
										if (ImGui::SmallButton("Focus"))
										{
											m_FocusedMaterialEntity = entityId;
											m_FocusedMaterialIndex = materialIndex;
											m_FocusedMaterialTextureSlot = Asset::MaterialTextureSlot::Count;
											m_FocusedMaterialControl = MeshRestoreMaterialFocusKind::None;
											m_FocusedMaterialHighlightFrames = 90;
											m_FocusedMaterialFocusPinned = false;
										}
										if (ImGui::IsItemHovered())
										{
											ImGui::SetTooltip("Open and highlight this material in the full Materials section.");
										}
										ImGui::PopID();
									}
									ImGui::EndTable();

									if (meshAsset->Materials.size() > maxMaterialRows)
									{
										ImGui::TextDisabled("Showing first %zu material rows.", maxMaterialRows);
									}
								}

								if (ImGui::TreeNodeEx("Material Scalar Quick Edit", ImGuiTreeNodeFlags_DefaultOpen))
								{
									ImGui::TextDisabled("Texture slots stay in the full Materials section; this drawer edits material scalar values only.");
									const size_t maxScalarRows = (std::min)(meshAsset->Materials.size(), static_cast<size_t>(16));
									for (size_t materialIndex = 0; materialIndex < maxScalarRows; ++materialIndex)
									{
										Asset::StaticMeshMaterial& material = meshAsset->Materials[materialIndex];
										const std::string scalarLabel = std::format(
											"Material[{}] {}##QuickMaterialScalar{}",
											materialIndex,
											material.Name.empty() ? "<unnamed>" : material.Name,
											materialIndex);
										if (!ImGui::TreeNode(scalarLabel.c_str()))
										{
											continue;
										}

										auto trackQuickMaterialControl = [&]()
										{
											if (ImGui::IsItemActivated())
											{
												m_MaterialEditingEntity = entityId;
												m_MaterialEditingIndex = materialIndex;
												m_MaterialEditBefore = material;
											}
											if (ImGui::IsItemDeactivatedAfterEdit() &&
												m_MaterialEditingEntity == entityId &&
												m_MaterialEditingIndex == materialIndex)
											{
												if (context.OnMaterialEditCommitted)
												{
													context.OnMaterialEditCommitted(entityId, materialIndex, m_MaterialEditBefore, material);
												}
												m_MaterialEditingEntity = InvalidEntityId;
												m_MaterialEditingIndex = static_cast<size_t>(-1);
												m_MaterialEditBefore = {};
											}
										};

										int shadingModelIndex = material.ShadingModel == Asset::MaterialShadingModel::PBR
											? 1
											: material.ShadingModel == Asset::MaterialShadingModel::Unlit ? 2 : 0;
										if (ImGui::Combo("Shading Model", &shadingModelIndex, "Phong\0PBR\0Unlit\0"))
										{
											const auto model = shadingModelIndex == 1
												? Asset::MaterialShadingModel::PBR
												: shadingModelIndex == 2 ? Asset::MaterialShadingModel::Unlit : Asset::MaterialShadingModel::Phong;
											if (context.OnMaterialShadingModelChanged)
											{
												context.OnMaterialShadingModelChanged(entityId, materialIndex, model);
											}
										}

										bool materialChanged = false;
										materialChanged |= ImGui::ColorEdit4("Base Color", &material.DiffuseColor.x);
										trackQuickMaterialControl();
										materialChanged |= ImGui::Checkbox("Use Vertex Color", &material.UseVertexColor);
										trackQuickMaterialControl();
										materialChanged |= ImGui::Checkbox("Normal Y Flip", &material.NormalYFlip);
										trackQuickMaterialControl();
										materialChanged |= ImGui::ColorEdit3("Emissive", &material.EmissiveColor.x);
										trackQuickMaterialControl();
										materialChanged |= ImGui::DragFloat("Opacity", &material.Opacity, 0.01f, 0.0f, 1.0f);
										trackQuickMaterialControl();
										if (material.ShadingModel == Asset::MaterialShadingModel::PBR)
										{
											materialChanged |= ImGui::DragFloat("Metallic", &material.MetallicFactor, 0.01f, 0.0f, 1.0f);
											trackQuickMaterialControl();
											materialChanged |= ImGui::DragFloat("Roughness", &material.RoughnessFactor, 0.01f, 0.02f, 1.0f);
											trackQuickMaterialControl();
										}
										else if (material.ShadingModel == Asset::MaterialShadingModel::Phong)
										{
											materialChanged |= ImGui::ColorEdit3("Specular", &material.SpecularColor.x);
											trackQuickMaterialControl();
											materialChanged |= ImGui::DragFloat("Shininess", &material.Shininess, 1.0f, 1.0f, 1024.0f);
											trackQuickMaterialControl();
										}

										if (materialChanged && context.OnMaterialEdited)
										{
											material.DiffuseColor.x = std::clamp(material.DiffuseColor.x, 0.0f, 1.0f);
											material.DiffuseColor.y = std::clamp(material.DiffuseColor.y, 0.0f, 1.0f);
											material.DiffuseColor.z = std::clamp(material.DiffuseColor.z, 0.0f, 1.0f);
											material.DiffuseColor.w = std::clamp(material.DiffuseColor.w, 0.0f, 1.0f);
											material.Opacity = std::clamp(material.Opacity, 0.0f, 1.0f);
											material.MetallicFactor = std::clamp(material.MetallicFactor, 0.0f, 1.0f);
											material.RoughnessFactor = std::clamp(material.RoughnessFactor, 0.02f, 1.0f);
											material.Shininess = std::clamp(material.Shininess, 1.0f, 1024.0f);
											context.OnMaterialEdited(entityId, materialIndex);
										}

										const std::vector<MaterialEditRecord> quickMaterialBatchRecords =
											buildQuickMaterialScalarBatchRecords(materialIndex, material);
										if (inspectorSelection.size() > 1)
										{
											ImGui::BeginDisabled(quickMaterialBatchRecords.empty());
											if (ImGui::SmallButton("Apply Scalars To Selected"))
											{
												context.OnMaterialBatchEditCommitted(quickMaterialBatchRecords);
											}
											ImGui::EndDisabled();
											if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
											{
												ImGui::SetTooltip(quickMaterialBatchRecords.empty()
													? "No other selected entity has a different material scalar value at this material index."
													: "Copy this material's scalar values to selected entities with the same material index. Texture bindings stay per entity.");
											}
											ImGui::SameLine();
											ImGui::TextDisabled(
												"%zu target%s, textures preserved",
												quickMaterialBatchRecords.size(),
												quickMaterialBatchRecords.size() == 1 ? "" : "s");
										}

										ImGui::TreePop();
									}
									if (meshAsset->Materials.size() > maxScalarRows)
									{
										ImGui::TextDisabled("Showing scalar controls for first %zu material rows. Use the full Materials section for the complete list.", maxScalarRows);
									}
									ImGui::TreePop();
								}

								if (ImGui::TreeNode("Material Texture Slot Actions"))
								{
									ImGui::TextDisabled("Uses the same Browse, Clear, and Project drag/drop assignment path as the full Materials section.");
									ImGui::PushID("QuickMaterialTextureSlotActions");
									const size_t maxTextureRows = (std::min)(meshAsset->Materials.size(), static_cast<size_t>(8));
									for (size_t materialIndex = 0; materialIndex < maxTextureRows; ++materialIndex)
									{
										const Asset::StaticMeshMaterial& material = meshAsset->Materials[materialIndex];
										const std::string textureLabel = std::format(
											"Material[{}] {}##QuickMaterialTextures{}",
											materialIndex,
											material.Name.empty() ? "<unnamed>" : material.Name,
											materialIndex);
										if (!ImGui::TreeNode(textureLabel.c_str()))
										{
											continue;
										}

										for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
										{
											const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
											if (!ShouldShowMaterialSlot(material.ShadingModel, slot))
											{
												continue;
											}

											const Asset::StaticMeshMaterial* prefabMaterial =
												prefabOverrideSource &&
												prefabOverrideSource->HasMesh &&
												materialIndex < prefabOverrideSource->MaterialOverrides.size()
												? &prefabOverrideSource->MaterialOverrides[materialIndex]
												: nullptr;
											const bool focusThisSlot =
												m_FocusedMaterialEntity == entityId &&
												m_FocusedMaterialIndex == materialIndex &&
												m_FocusedMaterialTextureSlot == slot;
											DrawMaterialTextureSlotRow(context, entityId, materialIndex, material, slot, focusThisSlot, meshAsset->SourcePath, {});
											if (prefabMaterial && MaterialTextureSlotDiffers(material, *prefabMaterial, slot))
											{
												ImGui::SameLine();
												ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "[Override]");
												if (ImGui::IsItemHovered())
												{
													ImGui::BeginTooltip();
													ImGui::Text("%s texture differs from prefab source.", std::string(Asset::MaterialTextureSlotName(slot)).c_str());
													ImGui::Separator();
													ImGui::TextWrapped("Current: %s", MaterialTextureSourceText(material, slot).c_str());
													ImGui::TextWrapped("Prefab:  %s", MaterialTextureSourceText(*prefabMaterial, slot).c_str());
													ImGui::EndTooltip();
												}
											}
										}
										ImGui::TreePop();
									}
									if (meshAsset->Materials.size() > maxTextureRows)
									{
										ImGui::TextDisabled("Showing texture actions for first %zu material rows. Use the full Materials section for the complete list.", maxTextureRows);
									}
									ImGui::PopID();
									ImGui::TreePop();
								}
							}
						}
						break;
					case SceneComponentKind::Animator:
						if (AnimatorComponent* animator = context.ActiveScene.GetAnimatorComponent(entityId))
						{
							const Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(entityId);
							const bool hasClips = mesh && mesh->IsAnimated && !mesh->Animations.empty();
							if (!hasClips)
							{
								ImGui::TextDisabled("%s: animated mesh required", label.c_str());
							}
							else if (property.Name == "CurrentClipIndex")
							{
								const uint32_t lastClipIndex = static_cast<uint32_t>(mesh->Animations.size() - 1);
								animator->CurrentClipIndex = (std::min)(animator->CurrentClipIndex, lastClipIndex);
								const Asset::AnimationClip& currentClip = mesh->Animations[animator->CurrentClipIndex];
								const std::string currentClipLabel = currentClip.Name.empty()
									? std::format("Clip {}", animator->CurrentClipIndex)
									: currentClip.Name;
								if (ImGui::BeginCombo(label.c_str(), currentClipLabel.c_str()))
								{
									for (uint32_t clipIndex = 0; clipIndex < mesh->Animations.size(); ++clipIndex)
									{
										const Asset::AnimationClip& clip = mesh->Animations[clipIndex];
										const std::string clipLabel = clip.Name.empty()
											? std::format("Clip {}", clipIndex)
											: clip.Name;
										const bool selectedClip = clipIndex == animator->CurrentClipIndex;
										if (ImGui::Selectable(clipLabel.c_str(), selectedClip))
										{
											animator->CurrentClipIndex = clipIndex;
											animator->TimeSeconds = 0.0f;
											componentChanged = true;
										}
										if (selectedClip)
										{
											ImGui::SetItemDefaultFocus();
										}
									}
									ImGui::EndCombo();
								}
							}
							else if (property.Name == "TimeSeconds")
							{
								animator->CurrentClipIndex = (std::min)(animator->CurrentClipIndex, static_cast<uint32_t>(mesh->Animations.size() - 1));
								const Asset::AnimationClip& currentClip = mesh->Animations[animator->CurrentClipIndex];
								const double ticksPerSecond = currentClip.TicksPerSecond > 0.0 ? currentClip.TicksPerSecond : 25.0;
								const float durationSeconds = currentClip.DurationTicks > 0.0
									? static_cast<float>(currentClip.DurationTicks / ticksPerSecond)
									: 0.0f;
								if (durationSeconds > 0.0f)
								{
									animator->TimeSeconds = (std::clamp)(animator->TimeSeconds, 0.0f, durationSeconds);
									componentChanged |= ImGui::SliderFloat(label.c_str(), &animator->TimeSeconds, 0.0f, durationSeconds, "%.3f sec");
								}
								else
								{
									ImGui::TextDisabled("%s: invalid duration", label.c_str());
								}
							}
							else if (property.Name == "Speed")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &animator->Speed, 0.01f, 0.0f, 10.0f, "%.2f");
							}
							else if (property.Name == "Playing")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &animator->Playing);
							}
							else if (property.Name == "Loop")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &animator->Loop);
							}
						}
						break;
					case SceneComponentKind::Camera:
						if (CameraComponent* camera = context.ActiveScene.GetCameraComponent(entityId))
						{
							if (property.Name == "FovY")
							{
								float fovDegrees = DirectX::XMConvertToDegrees(camera->FovY);
								if (ImGui::DragFloat(label.c_str(), &fovDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
								{
									camera->FovY = DirectX::XMConvertToRadians((std::clamp)(fovDegrees, 1.0f, 179.0f));
									componentChanged = true;
								}
							}
							else if (property.Name == "NearZ")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &camera->NearZ, 0.01f, 0.001f, 100.0f, "%.3f");
							}
							else if (property.Name == "FarZ")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &camera->FarZ, 1.0f, 1.0f, 100000.0f, "%.1f");
							}
							else if (property.Name == "IsGameCamera")
							{
								ImGui::Text("%s: %s", label.c_str(), camera->IsGameCamera ? "true" : "false");
								ImGui::SameLine();
								ImGui::TextDisabled("(managed by Engine)");
							}
						}
						break;
					case SceneComponentKind::Light:
						if (LightComponent* light = context.ActiveScene.GetLightComponent(entityId))
						{
							if (property.Name == "Type")
							{
								int typeIndex = std::to_underlying(light->Type);
								if (ImGui::Combo(label.c_str(), &typeIndex, "Directional\0Point\0Spot\0"))
								{
									light->Type = static_cast<LightType>((std::clamp)(typeIndex, 0, 2));
									componentChanged = true;
								}
							}
							else if (property.Name == "Color")
							{
								componentChanged |= ImGui::ColorEdit3(label.c_str(), &light->Color.x);
							}
							else if (property.Name == "Intensity")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &light->Intensity, 0.05f, 0.0f, 100.0f);
							}
							else if (property.Name == "Range")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &light->Range, 1.0f, 0.0f, 10000.0f);
							}
							else if (property.Name == "SpotAngle")
							{
								float spotAngleDegrees = DirectX::XMConvertToDegrees(light->SpotAngle);
								if (ImGui::DragFloat(label.c_str(), &spotAngleDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
								{
									light->SpotAngle = DirectX::XMConvertToRadians((std::clamp)(spotAngleDegrees, 1.0f, 179.0f));
									componentChanged = true;
								}
							}
							else if (property.Name == "Enabled")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &light->Enabled);
							}
							else if (property.Name == "CastShadows")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &light->CastShadows);
							}
						}
						break;
					case SceneComponentKind::RigidBody:
						if (RigidBodyComponent* rigidBody = context.ActiveScene.GetRigidBodyComponent(entityId))
						{
							if (property.Name == "Type")
							{
								int bodyTypeIndex = static_cast<int>(Physics::ToIndex(rigidBody->Type));
								if (ImGui::Combo(label.c_str(), &bodyTypeIndex, "Static\0Dynamic\0Kinematic\0"))
								{
									rigidBody->Type = static_cast<Physics::RigidBodyType>((std::clamp)(bodyTypeIndex, 0, 2));
									componentChanged = true;
								}
							}
							else if (property.Name == "Mass")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &rigidBody->Mass, 0.05f, 0.001f, 10000.0f);
							}
							else if (property.Name == "UseGravity")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &rigidBody->UseGravity);
							}
							else if (property.Name == "LinearVelocity")
							{
								componentChanged |= ImGui::DragFloat3(label.c_str(), &rigidBody->LinearVelocity.x, 0.05f);
							}
							else if (property.Name == "AngularVelocity")
							{
								componentChanged |= ImGui::DragFloat3(label.c_str(), &rigidBody->AngularVelocity.x, 0.05f);
							}
						}
						break;
					case SceneComponentKind::Collider:
						if (ColliderComponent* collider = context.ActiveScene.GetColliderComponent(entityId))
						{
							if (property.Name == "Shape")
							{
								int shapeIndex = static_cast<int>(Physics::ToIndex(collider->Shape));
								if (ImGui::Combo(label.c_str(), &shapeIndex, "Box\0Sphere\0Capsule\0Plane\0"))
								{
									collider->Shape = static_cast<Physics::ColliderShape>((std::clamp)(shapeIndex, 0, 3));
									componentChanged = true;
								}
							}
							else if (property.Name == "Size")
							{
								componentChanged |= ImGui::DragFloat3(label.c_str(), &collider->Size.x, 0.05f, 0.001f, 10000.0f);
							}
							else if (property.Name == "Radius")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &collider->Radius, 0.01f, 0.001f, 10000.0f);
							}
							else if (property.Name == "Height")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &collider->Height, 0.01f, 0.001f, 10000.0f);
							}
							else if (property.Name == "Offset")
							{
								componentChanged |= ImGui::DragFloat3(label.c_str(), &collider->Offset.x, 0.01f);
							}
							else if (property.Name == "IsTrigger")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &collider->IsTrigger);
							}
						}
						break;
					case SceneComponentKind::PhysicsMaterial:
						if (PhysicsMaterialComponent* physicsMaterial = context.ActiveScene.GetPhysicsMaterialComponent(entityId))
						{
							if (property.Name == "StaticFriction")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &physicsMaterial->StaticFriction, 0.01f, 0.0f, 10.0f);
							}
							else if (property.Name == "DynamicFriction")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &physicsMaterial->DynamicFriction, 0.01f, 0.0f, 10.0f);
							}
							else if (property.Name == "Restitution")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &physicsMaterial->Restitution, 0.01f, 0.0f, 1.0f);
							}
						}
						break;
					case SceneComponentKind::PrefabInstance:
						if (PrefabInstanceComponent* prefab = context.ActiveScene.GetPrefabInstanceComponent(entityId))
						{
							if (property.Name == "PrefabPath")
							{
								componentChanged |= drawAssetPathField(label.c_str(), prefab->PrefabPath, { ".prefab" });
							}
							else if (property.Name == "SourceName")
							{
								componentChanged |= drawEditableString(label.c_str(), prefab->SourceName);
							}
							else if (property.Name == "TrackPrefabOverrides")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &prefab->TrackPrefabOverrides);
							}
						}
						break;
					case SceneComponentKind::SceneReference:
						if (SceneReferenceComponent* sceneReference = context.ActiveScene.GetSceneReferenceComponent(entityId))
						{
							if (property.Name == "ScenePath")
							{
								componentChanged |= drawAssetPathField(label.c_str(), sceneReference->ScenePath, { ".scene" });
							}
							else if (property.Name == "LoadAdditively")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &sceneReference->LoadAdditively);
							}
							else if (property.Name == "AutoLoad")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &sceneReference->AutoLoad);
							}
						}
						break;
					case SceneComponentKind::Script:
						if (ScriptComponent* script = context.ActiveScene.GetScriptComponent(entityId))
						{
							if (property.Name == "ScriptPath")
							{
								componentChanged |= drawAssetPathField(label.c_str(), script->ScriptPath, { ".cpp", ".h", ".hpp", ".lua", ".cs", ".gd" });
							}
							else if (property.Name == "ClassName")
							{
								componentChanged |= drawEditableString(label.c_str(), script->ClassName);
							}
							else if (property.Name == "Language")
							{
								int languageIndex = std::to_underlying(script->Language);
								if (ImGui::Combo(label.c_str(), &languageIndex, "Native\0Lua\0CSharp-like\0GDScript-like\0"))
								{
									script->Language = static_cast<ScriptLanguage>((std::clamp)(languageIndex, 0, 3));
									componentChanged = true;
								}
							}
							else if (property.Name == "RunInEditor")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &script->RunInEditor);
							}
						}
						break;
					case SceneComponentKind::Sprite2D:
						if (Sprite2DComponent* sprite = context.ActiveScene.GetSprite2DComponent(entityId))
						{
							if (property.Name == "TexturePath")
							{
								componentChanged |= drawAssetPathField(label.c_str(), sprite->TexturePath, { ".png", ".jpg", ".jpeg", ".tga", ".bmp" });
							}
							else if (property.Name == "Color")
							{
								componentChanged |= ImGui::ColorEdit4(label.c_str(), &sprite->Color.x);
							}
							else if (property.Name == "Size")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &sprite->Size.x, 0.01f, 0.001f, 10000.0f);
							}
							else if (property.Name == "Pivot")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &sprite->Pivot.x, 0.01f, 0.0f, 1.0f);
							}
							else if (property.Name == "SortingLayer")
							{
								componentChanged |= ImGui::DragInt(label.c_str(), &sprite->SortingLayer, 1.0f);
							}
							else if (property.Name == "OrderInLayer")
							{
								componentChanged |= ImGui::DragInt(label.c_str(), &sprite->OrderInLayer, 1.0f);
							}
						}
						break;
					case SceneComponentKind::UiElement:
						if (UiElementComponent* ui = context.ActiveScene.GetUiElementComponent(entityId))
						{
							if (property.Name == "Kind")
							{
								int kindIndex = std::to_underlying(ui->Kind);
								if (ImGui::Combo(label.c_str(), &kindIndex, "Panel\0Label\0Button\0Image\0"))
								{
									ui->Kind = static_cast<UiElementKind>((std::clamp)(kindIndex, 0, 3));
									componentChanged = true;
								}
							}
							else if (property.Name == "Text")
							{
								componentChanged |= drawEditableString(label.c_str(), ui->Text);
							}
							else if (property.Name == "AnchorMin")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &ui->AnchorMin.x, 0.01f, 0.0f, 1.0f);
							}
							else if (property.Name == "AnchorMax")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &ui->AnchorMax.x, 0.01f, 0.0f, 1.0f);
							}
							else if (property.Name == "Position")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &ui->Position.x, 0.5f);
							}
							else if (property.Name == "Size")
							{
								componentChanged |= ImGui::DragFloat2(label.c_str(), &ui->Size.x, 0.5f, 1.0f, 10000.0f);
							}
							else if (property.Name == "Color")
							{
								componentChanged |= ImGui::ColorEdit4(label.c_str(), &ui->Color.x);
							}
						}
						break;
					case SceneComponentKind::AudioSource:
						if (AudioSourceComponent* audio = context.ActiveScene.GetAudioSourceComponent(entityId))
						{
							if (property.Name == "ClipPath")
							{
								componentChanged |= drawAssetPathField(label.c_str(), audio->ClipPath, { ".wav", ".ogg", ".mp3", ".flac" });
							}
							else if (property.Name == "Volume")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &audio->Volume, 0.01f, 0.0f, 1.0f);
							}
							else if (property.Name == "Pitch")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &audio->Pitch, 0.01f, 0.1f, 4.0f);
							}
							else if (property.Name == "Loop")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &audio->Loop);
							}
							else if (property.Name == "PlayOnStart")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &audio->PlayOnStart);
							}
							else if (property.Name == "Spatialize")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &audio->Spatialize);
							}
						}
						break;
					case SceneComponentKind::NavigationAgent:
						if (NavigationAgentComponent* navigation = context.ActiveScene.GetNavigationAgentComponent(entityId))
						{
							if (property.Name == "Radius")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &navigation->Radius, 0.01f, 0.001f, 1000.0f);
							}
							else if (property.Name == "Height")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &navigation->Height, 0.01f, 0.001f, 1000.0f);
							}
							else if (property.Name == "Speed")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &navigation->Speed, 0.01f, 0.0f, 1000.0f);
							}
							else if (property.Name == "Acceleration")
							{
								componentChanged |= ImGui::DragFloat(label.c_str(), &navigation->Acceleration, 0.01f, 0.0f, 10000.0f);
							}
							else if (property.Name == "Target")
							{
								componentChanged |= ImGui::DragFloat3(label.c_str(), &navigation->Target.x, 0.05f);
							}
							else if (property.Name == "HasTarget")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &navigation->HasTarget);
							}
						}
						break;
					case SceneComponentKind::NetworkIdentity:
						if (NetworkIdentityComponent* network = context.ActiveScene.GetNetworkIdentityComponent(entityId))
						{
							if (property.Name == "NetworkId")
							{
								ImGui::Text("%s: %llu", label.c_str(), static_cast<unsigned long long>(network->NetworkId));
							}
							else if (property.Name == "PrefabKey")
							{
								componentChanged |= drawEditableString(label.c_str(), network->PrefabKey);
							}
							else if (property.Name == "ReplicateTransform")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &network->ReplicateTransform);
							}
							else if (property.Name == "ServerAuthoritative")
							{
								componentChanged |= ImGui::Checkbox(label.c_str(), &network->ServerAuthoritative);
							}
						}
						break;
					default:
						break;
					}
					if (propertyHasPrefabOverride)
					{
						ImGui::SameLine();
						ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.32f, 1.0f), "[Override]");
						if (ImGui::IsItemHovered())
						{
							ImGui::BeginTooltip();
							ImGui::TextUnformatted("This property differs from its prefab source.");
							ImGui::Separator();
							ImGui::TextWrapped("Current: %s", prefabCurrentValue.empty() ? "<empty>" : prefabCurrentValue.c_str());
							ImGui::TextWrapped("Prefab:  %s", prefabSourceValue.empty() ? "<empty>" : prefabSourceValue.c_str());
							ImGui::EndTooltip();
						}
					}
					ImGui::PopID();
				}

				if (componentChanged)
				{
					anyChanged = true;
					if (requiresPhysicsActorRebuild(descriptor->Kind) && context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(entityId);
					}
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		if (anyChanged && context.OnSceneEdited)
		{
			context.OnSceneEdited();
		}
	}

	void EditorLayer::DrawReflectionSchema(
		EditorContext& context,
		EntityId entityId,
		const std::vector<SceneComponentKind>& componentOrder,
		std::string_view inspectorFilter)
	{
		auto descriptorMatchesFilter = [inspectorFilter](const Reflection::ComponentDescriptor& descriptor)
		{
			if (inspectorFilter.empty() ||
				ContainsCaseInsensitive("Reflection Schema Descriptor Metadata Property Serialization Drawer", inspectorFilter) ||
				ContainsCaseInsensitive(descriptor.Name, inspectorFilter))
			{
				return true;
			}
			for (const Reflection::PropertyDescriptor& property : descriptor.Properties)
			{
				if (ContainsCaseInsensitive(property.Name, inspectorFilter) ||
					ContainsCaseInsensitive(Reflection::ToString(property.ValueKind), inspectorFilter))
				{
					return true;
				}
			}
			return false;
		};

		std::vector<const Reflection::ComponentDescriptor*> visibleDescriptors;
		visibleDescriptors.reserve(componentOrder.size());
		size_t missingDescriptorCount = 0;
		size_t propertyCount = 0;
		for (const SceneComponentKind kind : componentOrder)
		{
			if (!HasInspectableComponent(context.ActiveScene, entityId, kind))
			{
				continue;
			}

			const Reflection::ComponentDescriptor* descriptor = Reflection::FindSceneComponentDescriptor(kind);
			if (!descriptor)
			{
				++missingDescriptorCount;
				continue;
			}
			if (!descriptorMatchesFilter(*descriptor))
			{
				continue;
			}

			visibleDescriptors.push_back(descriptor);
			propertyCount += descriptor->Properties.size();
		}

		const bool shouldShowForFilter = inspectorFilter.empty() ||
			ContainsCaseInsensitive("Reflection Schema Descriptor Metadata Property Serialization Drawer", inspectorFilter) ||
			!visibleDescriptors.empty() ||
			missingDescriptorCount > 0;
		if (!shouldShowForFilter)
		{
			return;
		}

		if (!ImGui::CollapsingHeader("Reflection Schema"))
		{
			return;
		}

		ImGui::TextDisabled(
			"Read-only v1: descriptor coverage for the selected Entity. Manual property UI is still authoritative.");
		ImGui::Text(
			"Descriptors: %zu / %zu | Reflected properties: %zu | Missing descriptors: %zu",
			visibleDescriptors.size(),
			componentOrder.size(),
			propertyCount,
			missingDescriptorCount);

		if (visibleDescriptors.empty())
		{
			ImGui::TextDisabled(inspectorFilter.empty()
				? "No reflected component descriptors are available for this Entity."
				: "No reflected component descriptor matches the current Inspector search.");
			return;
		}

		if (ImGui::BeginTable("ReflectionSchemaTable", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch, 0.22f);
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.18f);
			ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 0.22f);
			ImGui::TableSetupColumn("Properties", ImGuiTableColumnFlags_WidthStretch, 0.38f);
			ImGui::TableHeadersRow();

			for (const Reflection::ComponentDescriptor* descriptor : visibleDescriptors)
			{
				const bool enabled = IsInspectableComponentEnabled(context.ActiveScene, entityId, descriptor->Kind);
				std::string flags;
				flags.append(descriptor->CanAdd ? "Add" : "-");
				flags.append(" / ");
				flags.append(descriptor->CanRemove ? "Remove" : "-");
				flags.append(" / ");
				flags.append(descriptor->CanDisable ? "Disable" : "-");

				std::string properties;
				for (const Reflection::PropertyDescriptor& property : descriptor->Properties)
				{
					if (!properties.empty())
					{
						properties.append(", ");
					}
					properties.append(property.Name);
					properties.append(":");
					properties.append(Reflection::ToString(property.ValueKind));
				}

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextWrapped("%.*s", static_cast<int>(descriptor->Name.size()), descriptor->Name.data());
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", enabled ? "Enabled" : "Disabled");
				ImGui::TableSetColumnIndex(2);
				ImGui::TextWrapped("%s", flags.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextWrapped("%s", properties.empty() ? "<no properties>" : properties.c_str());
			}

			ImGui::EndTable();
		}
	}

	void EditorLayer::OpenProjectCreateAssetDialog(ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory)
	{
		m_PendingProjectCreateKind = kind;
		m_PendingProjectCreateDirectory = targetDirectory;
		m_ProjectCreateNameBuffer.fill('\0');

		const std::string_view defaultName = ProjectCreateDefaultName(kind);
		const size_t copyLength = (std::min)(defaultName.size(), m_ProjectCreateNameBuffer.size() - 1);
		std::copy_n(defaultName.data(), copyLength, m_ProjectCreateNameBuffer.data());

		m_ProjectCreateStatus.clear();
		m_ShouldOpenProjectCreateAssetModal = true;
	}

	void EditorLayer::DrawProjectCreateAssetModal(EditorContext& context)
	{
		if (m_ShouldOpenProjectCreateAssetModal)
		{
			ImGui::OpenPopup("Create Project Asset");
			m_ShouldOpenProjectCreateAssetModal = false;
		}

		bool popupOpen = true;
		if (!ImGui::BeginPopupModal("Create Project Asset", &popupOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			return;
		}

		const std::shared_ptr<const Asset::AssetFileSnapshot> snapshot = context.ProjectSnapshot;
		const std::filesystem::path rootPath = snapshot ? snapshot->RootPath : std::filesystem::path{};
		const std::string targetLabel = rootPath.empty()
			? m_PendingProjectCreateDirectory.string()
			: RelativeDisplayPath(m_PendingProjectCreateDirectory, rootPath);
		const std::string_view kindName = ProjectCreateAssetKindName(m_PendingProjectCreateKind);
		const std::string_view extension = ProjectCreateExtension(m_PendingProjectCreateKind);

		ImGui::Text("Type: %.*s", static_cast<int>(kindName.size()), kindName.data());
		ImGui::TextWrapped("Target: %s", targetLabel.empty() ? "<Assets>" : targetLabel.c_str());
		if (!extension.empty())
		{
			ImGui::TextDisabled("Extension: %.*s", static_cast<int>(extension.size()), extension.data());
		}
		ImGui::Separator();

		ImGui::SetNextItemWidth(320.0f);
		if (ImGui::InputText("Name", m_ProjectCreateNameBuffer.data(), m_ProjectCreateNameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			const std::string requestedName = TrimCopy(m_ProjectCreateNameBuffer.data());
			if (requestedName.empty())
			{
				m_ProjectCreateStatus = "Name cannot be empty.";
			}
			else
			{
				if (context.OnCreateNamedProjectAsset)
				{
					context.OnCreateNamedProjectAsset(m_PendingProjectCreateKind, m_PendingProjectCreateDirectory, requestedName);
				}
				else if (context.OnCreateProjectAsset)
				{
					context.OnCreateProjectAsset(m_PendingProjectCreateKind, m_PendingProjectCreateDirectory);
				}
				m_ProjectCreateStatus.clear();
				ImGui::CloseCurrentPopup();
			}
		}

		if (!m_ProjectCreateStatus.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.46f, 0.34f, 1.0f), "%s", m_ProjectCreateStatus.c_str());
		}

		const std::string requestedName = TrimCopy(m_ProjectCreateNameBuffer.data());
		ImGui::BeginDisabled(requestedName.empty() || (!context.OnCreateNamedProjectAsset && !context.OnCreateProjectAsset));
		if (ImGui::Button("Create", ImVec2(96.0f, 0.0f)))
		{
			if (context.OnCreateNamedProjectAsset)
			{
				context.OnCreateNamedProjectAsset(m_PendingProjectCreateKind, m_PendingProjectCreateDirectory, requestedName);
			}
			else if (context.OnCreateProjectAsset)
			{
				context.OnCreateProjectAsset(m_PendingProjectCreateKind, m_PendingProjectCreateDirectory);
			}
			m_ProjectCreateStatus.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(96.0f, 0.0f)))
		{
			m_ProjectCreateStatus.clear();
			ImGui::CloseCurrentPopup();
		}

		if (!popupOpen)
		{
			m_ProjectCreateStatus.clear();
		}
		ImGui::EndPopup();
	}

	void EditorLayer::DrawProject(EditorContext& context)
	{
		EnsureProjectStateLoaded(context);

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Project", 8.0f, viewport->Size.y - 250.0f, viewport->Size.x * 0.42f, 240.0f);
		ImGui::Begin("Project");

		if (ImGui::Button("Refresh") && context.OnProjectRefresh)
		{
			context.OnProjectRefresh();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(context.ProjectRefreshInProgress ? "Scanning..." : "Ready");
		ImGui::SameLine();
		if (ImGui::Checkbox("Two Column", &m_ProjectTwoColumnLayout))
		{
			SaveProjectState(context);
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("Scope Folder", &m_ProjectFolderScopeEnabled))
		{
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show only the selected folder's contents in the Project browser.");
		}

		const auto snapshot = context.ProjectSnapshot;
		if (!snapshot)
		{
			ImGui::TextUnformatted("Project snapshot is loading.");
			ImGui::End();
			return;
		}

		if (!snapshot->RootExists)
		{
			ImGui::Text("%s", snapshot->Status.c_str());
			ImGui::End();
			return;
		}

		std::error_code favoriteError;
		const size_t previousFavoriteCount = m_ProjectFavoritePaths.size();
		std::erase_if(m_ProjectFavoritePaths, [&favoriteError](const std::filesystem::path& path)
			{
				favoriteError.clear();
				return !std::filesystem::exists(path, favoriteError);
			});
		if (previousFavoriteCount != m_ProjectFavoritePaths.size())
		{
			SaveProjectState(context);
		}
		const size_t previousRecentAssetCount = m_ProjectRecentAssetPaths.size();
		std::erase_if(m_ProjectRecentAssetPaths, [&favoriteError](const std::filesystem::path& path)
			{
				favoriteError.clear();
				return !std::filesystem::is_regular_file(path, favoriteError);
			});
		if (previousRecentAssetCount != m_ProjectRecentAssetPaths.size())
		{
			SaveProjectState(context);
		}
		if (!m_SelectedAssetPath.empty() && !std::filesystem::exists(m_SelectedAssetPath, favoriteError))
		{
			m_SelectedAssetPath.clear();
		}

		const std::string rootLabel = snapshot->RootPath.filename().empty()
			? snapshot->RootPath.string()
			: snapshot->RootPath.filename().string();
		const auto selectedTargetDirectory = [&]() -> std::filesystem::path
		{
			std::error_code errorCode;
			if (!m_SelectedAssetPath.empty())
			{
				if (std::filesystem::is_directory(m_SelectedAssetPath, errorCode))
				{
					return m_SelectedAssetPath;
				}
				if (std::filesystem::is_regular_file(m_SelectedAssetPath, errorCode))
				{
					return m_SelectedAssetPath.parent_path();
				}
			}
			return snapshot->RootPath;
		};
		const auto findEntryByPath = [](const auto& self, const std::vector<Asset::AssetFileEntry>& entries, const std::filesystem::path& path) -> const Asset::AssetFileEntry*
		{
			for (const Asset::AssetFileEntry& entry : entries)
			{
				if (SamePath(entry.Path, path))
				{
					return &entry;
				}
				if (!entry.Children.empty())
				{
					if (const Asset::AssetFileEntry* child = self(self, entry.Children, path))
					{
						return child;
					}
				}
			}
			return nullptr;
		};

		ImGui::SameLine();
		if (ImGui::Button("Create"))
		{
			ImGui::OpenPopup("ProjectCreateAssetPopup");
		}
		if (ImGui::BeginPopup("ProjectCreateAssetPopup"))
		{
			const std::filesystem::path targetDirectory = selectedTargetDirectory();
			ImGui::TextDisabled("Target: %s", RelativeDisplayPath(targetDirectory, snapshot->RootPath).c_str());
			ImGui::Separator();
			const auto createAsset = [&](ProjectCreateAssetKind kind)
			{
				OpenProjectCreateAssetDialog(kind, targetDirectory);
			};
			if (ImGui::MenuItem("Folder"))
			{
				createAsset(ProjectCreateAssetKind::Folder);
			}
			if (ImGui::MenuItem("Scene"))
			{
				createAsset(ProjectCreateAssetKind::Scene);
			}
			if (ImGui::MenuItem("Material"))
			{
				createAsset(ProjectCreateAssetKind::Material);
			}
			if (ImGui::MenuItem("Skybox"))
			{
				createAsset(ProjectCreateAssetKind::Skybox);
			}
			if (ImGui::MenuItem("Script"))
			{
				createAsset(ProjectCreateAssetKind::Script);
			}
			if (ImGui::MenuItem("Prefab"))
			{
				createAsset(ProjectCreateAssetKind::Prefab);
			}
			ImGui::EndPopup();
		}
		DrawProjectCreateAssetModal(context);

		const auto drawProjectBreadcrumb = [&]()
		{
			const std::filesystem::path rootPath = snapshot->RootPath.lexically_normal();
			const std::filesystem::path currentDirectory = selectedTargetDirectory().lexically_normal();
			std::error_code relativeError;
			const std::filesystem::path relativeDirectory = std::filesystem::relative(currentDirectory, rootPath, relativeError);
			ImGui::TextDisabled("Path");
			ImGui::SameLine();
			if (ImGui::SmallButton(rootLabel.c_str()))
			{
				m_SelectedAssetPath = rootPath;
			}
			if (!relativeError && !relativeDirectory.empty() && relativeDirectory != ".")
			{
				std::filesystem::path accumulatedPath = rootPath;
				for (const std::filesystem::path& segment : relativeDirectory)
				{
					if (segment.empty() || segment == ".")
					{
						continue;
					}
					accumulatedPath /= segment;
					const std::string segmentLabel = segment.string();
					ImGui::SameLine();
					ImGui::TextDisabled(">");
					ImGui::SameLine();
					ImGui::PushID(accumulatedPath.string().c_str());
					if (ImGui::SmallButton(segmentLabel.c_str()))
					{
						m_SelectedAssetPath = accumulatedPath;
					}
					ImGui::PopID();
				}
			}

			if (!SamePath(currentDirectory, rootPath))
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Up"))
				{
					const std::filesystem::path parentPath = currentDirectory.parent_path();
					m_SelectedAssetPath = parentPath.empty() ? rootPath : parentPath;
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Select parent folder.");
				}
			}
		};
		drawProjectBreadcrumb();

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##ProjectFilter", "Search assets...", m_ProjectFilter.data(), m_ProjectFilter.size());
		const std::string_view currentProjectFilter = TextFilter(m_ProjectFilter);
		int projectQuickFilterIndex = static_cast<int>(std::to_underlying(m_ProjectQuickFilter));
		ImGui::SetNextItemWidth(132.0f);
		if (ImGui::Combo("##ProjectQuickFilter", &projectQuickFilterIndex, "All\0Favorites\0Folders\0Models\0Images\0Scenes\0Materials\0Prefabs\0Source\0Text\0"))
		{
			const int projectQuickFilterMax = static_cast<int>(std::to_underlying(ProjectQuickFilter::Text));
			projectQuickFilterIndex = std::clamp(projectQuickFilterIndex, 0, projectQuickFilterMax);
			m_ProjectQuickFilter = static_cast<ProjectQuickFilter>(projectQuickFilterIndex);
			SaveProjectState(context);
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Project asset type filter: %s", ProjectQuickFilterName(m_ProjectQuickFilter));
		}
		ImGui::SameLine();
		if (ImGui::Button("Save Search"))
		{
			const std::string search(currentProjectFilter);
			if (!search.empty() && std::ranges::find(m_ProjectSavedSearches, search) == m_ProjectSavedSearches.end())
			{
				m_ProjectSavedSearches.push_back(search);
				SaveProjectState(context);
			}
		}
		ImGui::SameLine();
		const bool hasProjectFilters = !currentProjectFilter.empty() || m_ProjectQuickFilter != ProjectQuickFilter::All;
		ImGui::BeginDisabled(!hasProjectFilters);
		if (ImGui::Button("Clear Filters"))
		{
			m_ProjectFilter.fill('\0');
			m_ProjectQuickFilter = ProjectQuickFilter::All;
			SaveProjectState(context);
		}
		ImGui::EndDisabled();
		if (!m_ProjectSavedSearches.empty())
		{
			ImGui::SameLine();
			if (ImGui::BeginCombo("##SavedProjectSearches", "Saved Searches"))
			{
				for (size_t searchIndex = 0; searchIndex < m_ProjectSavedSearches.size(); ++searchIndex)
				{
					const std::string& search = m_ProjectSavedSearches[searchIndex];
					ImGui::PushID(static_cast<int>(searchIndex));
					if (ImGui::Selectable(search.c_str()))
					{
						SetTextBuffer(m_ProjectFilter, search);
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("x"))
					{
						m_ProjectSavedSearches.erase(m_ProjectSavedSearches.begin() + static_cast<std::ptrdiff_t>(searchIndex));
						SaveProjectState(context);
						ImGui::PopID();
						break;
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}
		const std::string_view projectFilter = TextFilter(m_ProjectFilter);
		const auto drawProjectActiveFilters = [&]()
		{
			const bool hasScopeFilter = m_ProjectFolderScopeEnabled;
			const bool hasQuickFilter = m_ProjectQuickFilter != ProjectQuickFilter::All;
			const bool hasTextFilter = !projectFilter.empty();
			if (!hasScopeFilter && !hasQuickFilter && !hasTextFilter)
			{
				return;
			}

			ImGui::TextDisabled("Active");
			if (hasScopeFilter)
			{
				const std::string scopeLabel = std::format("Scope: {}", RelativeDisplayPath(selectedTargetDirectory(), snapshot->RootPath));
				ImGui::SameLine();
				ImGui::TextUnformatted(scopeLabel.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("x##ClearProjectScopeFilter"))
				{
					m_ProjectFolderScopeEnabled = false;
					SaveProjectState(context);
				}
			}
			if (hasQuickFilter)
			{
				const std::string typeLabel = std::format("Type: {}", ProjectQuickFilterName(m_ProjectQuickFilter));
				ImGui::SameLine();
				ImGui::TextUnformatted(typeLabel.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("x##ClearProjectTypeFilter"))
				{
					m_ProjectQuickFilter = ProjectQuickFilter::All;
					SaveProjectState(context);
				}
			}
			if (hasTextFilter)
			{
				const std::string textLabel = std::format("Text: {}", std::string(projectFilter));
				ImGui::SameLine();
				ImGui::TextUnformatted(textLabel.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("x##ClearProjectTextFilter"))
				{
					m_ProjectFilter.fill('\0');
					SaveProjectState(context);
				}
			}
			if (hasScopeFilter || hasQuickFilter || hasTextFilter)
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear All##ClearAllProjectActiveFilters"))
				{
					m_ProjectFolderScopeEnabled = false;
					m_ProjectQuickFilter = ProjectQuickFilter::All;
					m_ProjectFilter.fill('\0');
					SaveProjectState(context);
				}
			}
		};
		drawProjectActiveFilters();

		const auto drawProjectBrowser = [&]()
		{
			if (!m_ProjectFolderScopeEnabled &&
				(m_ProjectQuickFilter == ProjectQuickFilter::All || m_ProjectQuickFilter == ProjectQuickFilter::Favorites) &&
				ImGui::CollapsingHeader("Favorites", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (m_ProjectFavoritePaths.empty())
				{
					ImGui::TextDisabled("No favorites yet.");
				}
				else
				{
					size_t visibleFavoriteCount = 0;
					for (const std::filesystem::path& favoritePath : m_ProjectFavoritePaths)
					{
						const std::string relativeFavoritePath = RelativeDisplayPath(favoritePath, snapshot->RootPath);
						if (!projectFilter.empty() &&
							!ContainsCaseInsensitive(relativeFavoritePath + " " + favoritePath.filename().string(), projectFilter))
						{
							continue;
						}
						++visibleFavoriteCount;
						const bool favoriteSelected = SamePath(m_SelectedAssetPath, favoritePath);
						const std::string label = std::format("* {}##{}", relativeFavoritePath, favoritePath.string());
						if (ImGui::Selectable(label.c_str(), favoriteSelected))
						{
							m_SelectedAssetPath = favoritePath;
							AddRecentProjectAssetPath(favoritePath, context, true);
						}
						if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
						{
							AddRecentProjectAssetPath(favoritePath, context, true);
							context.OnAssetOpen(favoritePath);
						}
					}
					if (visibleFavoriteCount == 0)
					{
						ImGui::TextDisabled("No favorites match the current search.");
					}
				}
			}

			const std::filesystem::path scopedDirectory = selectedTargetDirectory().lexically_normal();
			const bool scopedToSubfolder = m_ProjectFolderScopeEnabled && !SamePath(scopedDirectory, snapshot->RootPath);
			if (!m_ProjectFolderScopeEnabled)
			{
				DrawRecentProjectAssets(*snapshot, context, projectFilter);
			}

			const Asset::AssetFileEntry* scopedEntry = scopedToSubfolder
				? findEntryByPath(findEntryByPath, snapshot->Children, scopedDirectory)
				: nullptr;
			const std::string scopedRootLabel = scopedToSubfolder
				? std::format("{}##ProjectScopedRoot", RelativeDisplayPath(scopedDirectory, snapshot->RootPath))
				: rootLabel;
			const bool rootOpen = ImGui::TreeNodeEx(scopedRootLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow);
			if (rootOpen)
			{
				const std::vector<Asset::AssetFileEntry>* rootChildren = scopedEntry ? &scopedEntry->Children : &snapshot->Children;
				if (scopedToSubfolder && scopedEntry == nullptr)
				{
					ImGui::TextDisabled("Scoped folder is not available in the current project snapshot.");
				}
				for (const auto& entry : *rootChildren)
				{
					DrawProjectEntryRecursive(entry, context);
				}
				ImGui::TreePop();
			}
		};

		if (m_ProjectTwoColumnLayout)
		{
			if (ImGui::BeginTable("ProjectTwoColumnBrowser", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
			{
				ImGui::TableSetupColumn("Browser", ImGuiTableColumnFlags_WidthStretch, 0.58f);
				ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.42f);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::BeginChild("ProjectTree", ImVec2(0.0f, 0.0f), true);
				drawProjectBrowser();
				ImGui::EndChild();
				ImGui::TableSetColumnIndex(1);
				ImGui::BeginChild("ProjectDetails", ImVec2(0.0f, 0.0f), true);
				DrawSelectedAssetDetails(*snapshot, context);
				ImGui::EndChild();
				ImGui::EndTable();
			}
		}
		else
		{
			ImGui::BeginChild("ProjectTree", ImVec2(0.0f, -86.0f), true);
			drawProjectBrowser();
			ImGui::EndChild();
			DrawSelectedAssetDetails(*snapshot, context);
		}
		ImGui::End();
	}

	void EditorLayer::DrawBenchmark(EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Benchmark", viewport->Size.x * 0.43f, viewport->Size.y - 250.0f, viewport->Size.x * 0.32f, 240.0f);
		ImGui::Begin("Benchmark");
		if (context.SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark)
		{
			context.BenchmarkRunner.DrawImGui();
		}
		else
		{
			ImGui::Text("Sample Mode: %s", SampleModeName(context.SampleMode));
		}
		ImGui::End();
	}

	void EditorLayer::DrawProfiler(EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Profiler", viewport->Size.x * 0.43f, viewport->Size.y - 250.0f, viewport->Size.x * 0.32f, 240.0f);
		ImGui::Begin("Profiler");

		const uint64_t frameIndex = context.RenderFrameStats.FrameIndex;
		if (!m_ProfilerPaused && frameIndex != m_ProfilerLastFrameIndex)
		{
			const float frameMs = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime * 1000.0f : 0.0f;
			const float renderCpuMs = static_cast<float>(context.RenderGraphStats.TotalCpuMs);
			const float drawCalls = static_cast<float>(context.RenderFrameStats.DrawCallCount);
			const float triangleK = static_cast<float>(context.RenderFrameStats.SubmittedTriangleCount) / 1000.0f;
			const auto pushSample = [this](std::array<float, 180>& values, float value)
				{
					if (m_ProfilerSampleCount < values.size())
					{
						values[m_ProfilerSampleCount] = value;
						return;
					}

					std::move(values.begin() + 1, values.end(), values.begin());
					values.back() = value;
				};

			pushSample(m_ProfilerFrameMsHistory, frameMs);
			pushSample(m_ProfilerRenderCpuMsHistory, renderCpuMs);
			pushSample(m_ProfilerDrawCallHistory, drawCalls);
			pushSample(m_ProfilerTriangleKHistory, triangleK);
			if (m_ProfilerSampleCount < m_ProfilerFrameMsHistory.size())
			{
				++m_ProfilerSampleCount;
			}
			m_ProfilerLastFrameIndex = frameIndex;
		}

		ImGui::Checkbox("Pause", &m_ProfilerPaused);
		ImGui::SameLine();
		if (ImGui::Button("Reset"))
		{
			m_ProfilerFrameMsHistory.fill(0.0f);
			m_ProfilerRenderCpuMsHistory.fill(0.0f);
			m_ProfilerDrawCallHistory.fill(0.0f);
			m_ProfilerTriangleKHistory.fill(0.0f);
			m_ProfilerSampleCount = 0;
			m_ProfilerLastFrameIndex = 0;
		}

		const float latestFrameMs = m_ProfilerSampleCount > 0 ? m_ProfilerFrameMsHistory[m_ProfilerSampleCount - 1] : 0.0f;
		const float latestRenderCpuMs = m_ProfilerSampleCount > 0 ? m_ProfilerRenderCpuMsHistory[m_ProfilerSampleCount - 1] : 0.0f;
		const float fps = latestFrameMs > 0.0001f ? 1000.0f / latestFrameMs : 0.0f;
		ImGui::SeparatorText("Frame");
		ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frameIndex));
		ImGui::Text("Frame Time: %.2f ms (%.1f FPS)", latestFrameMs, fps);
		ImGui::Text("RenderGraph CPU: %.3f ms", latestRenderCpuMs);
		ImGui::Text("Draw Calls: %u | Triangles: %llu | Instances: %llu",
			context.RenderFrameStats.DrawCallCount,
			static_cast<unsigned long long>(context.RenderFrameStats.SubmittedTriangleCount),
			static_cast<unsigned long long>(context.RenderFrameStats.SubmittedInstanceCount));

		if (ImGui::CollapsingHeader("Viewport Tools"))
		{
			ImGui::Text("Measure Target: %s", SceneMeasureTargetName(m_MeasureTarget));
			if (m_LastMeshMeasureTriangleCount > 0)
			{
				const char* cacheMode = m_LastMeshMeasureUsedDynamicAcceleration
					? "Dynamic frame-local"
					: (m_LastMeshMeasureUsedAcceleration ? "Static" : "None");
				ImGui::Text("Mesh Surface: %s", m_LastMeshMeasureHit ? "Hit" : (m_LastMeshMeasureBoundsRejected ? "Bounds rejected" : "Miss"));
				ImGui::Text("Triangles Tested: %zu / %zu", m_LastMeshMeasureTrianglesTested, m_LastMeshMeasureTriangleCount);
				ImGui::Text("Cache: %s%s | Entries: %zu",
					cacheMode,
					m_LastMeshMeasureCacheRebuilt ? " rebuilt" : "",
					m_LastMeshMeasureCacheTriangleCount);
				ImGui::Text("Raycast: %.3f ms | Cache Build: %.3f ms",
					m_LastMeshMeasureRaycastMs,
					m_LastMeshMeasureCacheBuildMs);
				ImGui::TextDisabled("%s%s",
					m_LastMeshMeasureUsedBudget ? "Triangle budget active" : "Full tested/cached range",
					m_LastMeshMeasureUsedDynamicAcceleration ? " | animated/skinned frame cache" : "");
			}
			else
			{
				ImGui::TextDisabled("No Mesh Surface measure sample yet.");
			}
		}

		const auto maxOf = [this](const std::array<float, 180>& values, float minimum)
			{
				float maxValue = minimum;
				for (size_t index = 0; index < m_ProfilerSampleCount; ++index)
				{
					maxValue = (std::max)(maxValue, values[index]);
				}
				return maxValue;
			};

		if (m_ProfilerSampleCount > 1)
		{
			ImGui::SeparatorText("History");
			ImGui::PlotLines("Frame ms", m_ProfilerFrameMsHistory.data(), static_cast<int>(m_ProfilerSampleCount), 0, nullptr, 0.0f, maxOf(m_ProfilerFrameMsHistory, 33.3f), ImVec2(0.0f, 52.0f));
			ImGui::PlotLines("Render CPU ms", m_ProfilerRenderCpuMsHistory.data(), static_cast<int>(m_ProfilerSampleCount), 0, nullptr, 0.0f, maxOf(m_ProfilerRenderCpuMsHistory, 8.0f), ImVec2(0.0f, 52.0f));
			ImGui::PlotLines("Draw calls", m_ProfilerDrawCallHistory.data(), static_cast<int>(m_ProfilerSampleCount), 0, nullptr, 0.0f, maxOf(m_ProfilerDrawCallHistory, 10.0f), ImVec2(0.0f, 44.0f));
			ImGui::PlotLines("Triangles K", m_ProfilerTriangleKHistory.data(), static_cast<int>(m_ProfilerSampleCount), 0, nullptr, 0.0f, maxOf(m_ProfilerTriangleKHistory, 1.0f), ImVec2(0.0f, 44.0f));
		}

		if (ImGui::CollapsingHeader("RenderGraph Passes", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const Rendering::RenderGraphStats& graphStats = context.RenderGraphStats;
			ImGui::Text("Passes: %zu / %zu enabled | Timed: %zu", graphStats.EnabledPassCount, graphStats.PassCount, graphStats.TimedPassCount);
			ImGui::Text("Geometry %.3f ms | Lighting %.3f ms | Post %.3f ms | Editor %.3f ms",
				graphStats.GeometryCpuMs,
				graphStats.LightingCpuMs,
				graphStats.PostProcessCpuMs,
				graphStats.EditorCpuMs);
			if (context.RenderGraphPasses && ImGui::BeginTable("ProfilerRenderGraphPasses", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Pass");
				ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 108.0f);
				ImGui::TableSetupColumn("CPU ms", ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableHeadersRow();
				for (const Rendering::RenderGraphPass& pass : *context.RenderGraphPasses)
				{
					if (!pass.Enabled)
					{
						continue;
					}
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(pass.Name.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(Rendering::ToString(pass.Kind));
					ImGui::TableSetColumnIndex(2);
					if (pass.HasCpuTiming)
					{
						ImGui::Text("%.3f", pass.CpuMilliseconds);
					}
					else
					{
						ImGui::TextDisabled("-");
					}
				}
				ImGui::EndTable();
			}
		}

		if (ImGui::CollapsingHeader("Systems"))
		{
			ImGui::Text("Jobs: %u worker(s), frame %llu",
				context.JobStats.WorkerCount,
				static_cast<unsigned long long>(context.JobStats.FrameIndex));
			ImGui::Text("Memory Frame Arena: %.2f / %.2f MB",
				static_cast<double>(context.MemoryStats.FrameArenaCurrent) / (1024.0 * 1024.0),
				static_cast<double>(context.MemoryStats.FrameArenaCapacity) / (1024.0 * 1024.0));
			ImGui::Text("Resources: %zu loaded, %zu failed",
				context.ResourceStats.LoadedCount,
				context.ResourceStats.FailedCount);
			ImGui::Text("Scripts: %zu active, %zu started, %zu scheduled job(s)",
				context.ScriptStats.ActiveScriptCount,
				context.ScriptStats.StartedScriptCount,
				context.ScriptStats.ScheduledJobCount);
		}

		if (ImGui::CollapsingHeader("Autosave", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool autosaveEnabled = context.AutosaveEnabled;
			if (ImGui::Checkbox("Enabled", &autosaveEnabled) && context.OnAutosaveEnabledChanged)
			{
				context.OnAutosaveEnabledChanged(autosaveEnabled);
			}
			ImGui::SameLine();
			ImGui::TextDisabled(context.CanEditProjectScene ? "Project Scene" : "Project Scene only");

			float autosaveInterval = context.AutosaveIntervalSeconds;
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::DragFloat("Interval", &autosaveInterval, 5.0f, 15.0f, 900.0f, "%.0f sec", ImGuiSliderFlags_AlwaysClamp)
				&& context.OnAutosaveIntervalChanged)
			{
				context.OnAutosaveIntervalChanged(autosaveInterval);
			}

			float autosaveProgress = 0.0f;
			if (context.AutosaveEnabled && context.AutosaveIntervalSeconds > 0.0f && context.IsSceneDirty)
			{
				autosaveProgress = std::clamp(context.AutosaveElapsedSeconds / context.AutosaveIntervalSeconds, 0.0f, 1.0f);
			}

			std::string progressLabel = "Scene clean";
			if (!context.AutosaveEnabled)
			{
				progressLabel = "Disabled";
			}
			else if (!context.CanEditProjectScene)
			{
				progressLabel = "Project Scene only";
			}
			else if (context.IsSceneDirty)
			{
				progressLabel = std::format("{:.0f}/{:.0f} sec", context.AutosaveElapsedSeconds, context.AutosaveIntervalSeconds);
			}

			ImGui::ProgressBar(autosaveProgress, ImVec2(-1.0f, 0.0f), progressLabel.c_str());
			ImGui::Text("Dirty: %s", context.IsSceneDirty ? "Yes" : "No");
			if (!context.AutosaveStatusMessage.empty())
			{
				const ImVec4 statusColor = context.AutosaveLastSucceeded
					? ImVec4(0.45f, 0.95f, 0.55f, 1.0f)
					: ImVec4(0.95f, 0.78f, 0.38f, 1.0f);
				ImGui::TextColored(statusColor, "%s", context.AutosaveStatusMessage.c_str());
			}
			if (!context.AutosavePath.empty())
			{
				ImGui::TextWrapped("Last: %s", context.AutosavePath.string().c_str());
			}
		}

		ImGui::End();
	}

	void EditorLayer::DrawUnsavedSceneList(EditorContext& context)
	{
		if (!ImGui::CollapsingHeader("Unsaved Scenes", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (!context.CanEditProjectScene)
		{
			ImGui::TextDisabled("Unsaved scene tracking is active for Project Scene mode.");
			return;
		}

		const std::string sceneName = context.CurrentScenePath.empty()
			? std::string("<untitled>")
			: context.CurrentScenePath.filename().string();
		const std::string scenePath = context.CurrentScenePath.empty()
			? std::string("<no scene path>")
			: context.CurrentScenePath.string();
		if (!context.IsSceneDirty)
		{
			ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "No unsaved scenes.");
			ImGui::TextDisabled("Current: %s", sceneName.c_str());
			return;
		}

		if (ImGui::BeginTable("UnsavedSceneListTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Scene");
			ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 76.0f);
			ImGui::TableSetupColumn("Autosave", ImGuiTableColumnFlags_WidthFixed, 92.0f);
			ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 146.0f);
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextWrapped("%s", sceneName.c_str());
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", scenePath.c_str());
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "Dirty");
			ImGui::TableSetColumnIndex(2);
			if (context.AutosavePath.empty())
			{
				ImGui::TextDisabled("none");
			}
			else
			{
				std::error_code errorCode;
				const bool autosaveExists = std::filesystem::is_regular_file(context.AutosavePath, errorCode);
				ImGui::TextColored(
					autosaveExists ? ImVec4(0.45f, 0.95f, 0.55f, 1.0f) : ImVec4(0.95f, 0.78f, 0.38f, 1.0f),
					"%s",
					autosaveExists ? "ready" : "missing");
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", context.AutosavePath.string().c_str());
				}
			}
			ImGui::TableSetColumnIndex(3);
			if (ImGui::SmallButton("Save") && context.OnSaveScene)
			{
				context.OnSaveScene();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Save As") && context.OnSaveSceneAs)
			{
				context.OnSaveSceneAs();
			}
			ImGui::EndTable();
		}

		if (!context.AutosaveStatusMessage.empty())
		{
			ImGui::TextDisabled("%s", context.AutosaveStatusMessage.c_str());
		}
	}

	void EditorLayer::EnsureExportProfileDefaults(const EditorContext& context)
	{
		const std::filesystem::path rootPath = context.ProjectRootPath.lexically_normal();
		if (m_ExportProfileProjectRootPath == rootPath && m_ExportOutputDirectoryBuffer[0] != '\0')
		{
			return;
		}

		m_ExportProfileProjectRootPath = rootPath;
		m_ExportCopyAssets = true;
		m_ExportCopyScenes = true;
		m_ExportWriteManifest = true;
		m_ExportRevealAfterBuild = true;

		const std::filesystem::path defaultOutput = rootPath.empty()
			? std::filesystem::path("Builds") / "Windows"
			: rootPath / "Builds" / "Windows";
		const std::string outputText = defaultOutput.lexically_normal().string();
		m_ExportOutputDirectoryBuffer.fill('\0');
		const size_t copyLength = (std::min)(outputText.size(), m_ExportOutputDirectoryBuffer.size() - 1);
		std::copy_n(outputText.data(), copyLength, m_ExportOutputDirectoryBuffer.data());
	}

	Editor::ExportProfileSettings EditorLayer::BuildExportProfileSettings(const EditorContext& context) const
	{
		ExportProfileSettings settings;
		settings.OutputDirectory = std::filesystem::path(m_ExportOutputDirectoryBuffer.data());
		if (!settings.OutputDirectory.empty() && !settings.OutputDirectory.is_absolute() && !context.ProjectRootPath.empty())
		{
			settings.OutputDirectory = context.ProjectRootPath / settings.OutputDirectory;
		}
		settings.OutputDirectory = settings.OutputDirectory.lexically_normal();
		settings.CopyAssets = m_ExportCopyAssets;
		settings.CopyScenes = m_ExportCopyScenes;
		settings.WriteManifest = m_ExportWriteManifest;
		settings.RevealAfterExport = m_ExportRevealAfterBuild;
		return settings;
	}

	void EditorLayer::DrawExportProfile(EditorContext& context)
	{
		EnsureExportProfileDefaults(context);
		if (!ImGui::CollapsingHeader("Package / Export Profile", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (!context.CanEditProjectScene)
		{
			ImGui::TextDisabled("Export profiles are active for Project Scene mode.");
			return;
		}

		ImGui::Text("Profile: Windows Runtime Package");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint(
			"##ExportOutputDirectory",
			"Output directory...",
			m_ExportOutputDirectoryBuffer.data(),
			m_ExportOutputDirectoryBuffer.size());
		if (ImGui::Button("Use Default Output"))
		{
			m_ExportProfileProjectRootPath.clear();
			EnsureExportProfileDefaults(context);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reveal Last") && !m_LastExportOutputDirectory.empty() && context.OnAssetReveal)
		{
			context.OnAssetReveal(m_LastExportOutputDirectory);
		}

		ImGui::Checkbox("Copy Assets", &m_ExportCopyAssets);
		ImGui::SameLine();
		ImGui::Checkbox("Copy Scenes", &m_ExportCopyScenes);
		ImGui::Checkbox("Write runtime-package.json", &m_ExportWriteManifest);
		ImGui::SameLine();
		ImGui::Checkbox("Reveal after export", &m_ExportRevealAfterBuild);

		const ExportProfileSettings settings = BuildExportProfileSettings(context);
		ImGui::TextWrapped("Output: %s", settings.OutputDirectory.empty() ? "<none>" : settings.OutputDirectory.string().c_str());
		ImGui::TextDisabled(
			"Runtime launch path: EnginePlatformer.exe --runtime-package \"%s\"",
			(settings.OutputDirectory / "runtime-package.json").string().c_str());

		const bool canExport = static_cast<bool>(context.OnExportProjectProfile) || static_cast<bool>(context.OnExportProject);
		ImGui::BeginDisabled(!canExport || settings.OutputDirectory.empty());
		if (ImGui::Button("Export Package"))
		{
			bool success = false;
			if (context.OnExportProjectProfile)
			{
				success = context.OnExportProjectProfile(settings);
			}
			else if (context.OnExportProject)
			{
				context.OnExportProject();
				success = true;
			}

			m_LastExportOutputDirectory = settings.OutputDirectory;
			if (success)
			{
				size_t fileCount = 0;
				std::error_code errorCode;
				if (std::filesystem::is_directory(settings.OutputDirectory, errorCode))
				{
					for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(settings.OutputDirectory, errorCode))
					{
						if (errorCode)
						{
							break;
						}
						if (entry.is_regular_file(errorCode))
						{
							++fileCount;
						}
					}
				}
				m_LastExportWrittenFileCount = fileCount;
				m_LastExportStatusMessage = std::format("Export succeeded: {} file(s).", m_LastExportWrittenFileCount);
			}
			else
			{
				m_LastExportWrittenFileCount = 0;
				m_LastExportStatusMessage = "Export failed. Check Console log for details.";
			}
		}
		ImGui::EndDisabled();

		if (!m_LastExportStatusMessage.empty())
		{
			ImGui::TextColored(
				m_LastExportWrittenFileCount > 0 ? ImVec4(0.45f, 0.95f, 0.55f, 1.0f) : ImVec4(0.95f, 0.78f, 0.38f, 1.0f),
				"%s",
				m_LastExportStatusMessage.c_str());
		}
	}

	void EditorLayer::SaveCurrentEditorLayout(const EditorContext& context)
	{
		size_t iniSize = 0;
		const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);
		if (!iniData || iniSize == 0)
		{
			m_EditorLayoutStatusMessage = "Editor layout save failed: ImGui returned no layout data.";
			return;
		}

		m_ProjectEditorLayoutIni.assign(iniData, iniSize);
		m_ProjectEditorLayoutRestored = true;
		SaveProjectState(context);
		m_EditorLayoutStatusMessage = std::format("Editor layout saved: {} bytes.", m_ProjectEditorLayoutIni.size());
	}

	void EditorLayer::RestoreSavedEditorLayout()
	{
		if (m_ProjectEditorLayoutIni.empty())
		{
			m_EditorLayoutStatusMessage = "No saved editor layout for this project.";
			return;
		}

		ImGui::LoadIniSettingsFromMemory(m_ProjectEditorLayoutIni.data(), m_ProjectEditorLayoutIni.size());
		m_DefaultLayoutBuilt = true;
		m_ProjectEditorLayoutRestored = true;
		m_EditorLayoutStatusMessage = "Saved editor layout restored.";
	}

	void EditorLayer::ResetEditorLayoutToDefault()
	{
		m_DefaultLayoutBuilt = false;
		m_EditorLayoutStatusMessage = "Default editor layout rebuild requested.";
	}

	void EditorLayer::DrawEditorLayoutTools(EditorContext& context)
	{
		if (!ImGui::CollapsingHeader("Editor Layout"))
		{
			return;
		}

		if (ImGui::Button("Save Layout"))
		{
			SaveCurrentEditorLayout(context);
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(m_ProjectEditorLayoutIni.empty());
		if (ImGui::Button("Restore Saved"))
		{
			RestoreSavedEditorLayout();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Reset Default"))
		{
			ResetEditorLayoutToDefault();
		}

		ImGui::Text("Saved layout: %s", m_ProjectEditorLayoutIni.empty() ? "No" : "Yes");
		if (!m_ProjectEditorLayoutIni.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu bytes)", m_ProjectEditorLayoutIni.size());
		}
		ImGui::Text("Auto restored: %s", m_ProjectEditorLayoutRestored ? "Yes" : "No");
		if (!m_EditorLayoutStatusMessage.empty())
		{
			ImGui::TextDisabled("%s", m_EditorLayoutStatusMessage.c_str());
		}
	}

	void EditorLayer::RefreshSourceControlStatus(const EditorContext& context)
	{
		SourceControlSummary summary;
		summary.RootPath = context.ProjectRootPath.empty() ? std::filesystem::current_path() : context.ProjectRootPath;
		summary.LastRefreshTime = ImGui::GetTime();

		std::error_code errorCode;
		if (summary.RootPath.empty() || !std::filesystem::exists(summary.RootPath, errorCode))
		{
			summary.Message = "Source control skipped: project root does not exist.";
			m_SourceControlSummary = std::move(summary);
			return;
		}

		int exitCode = -1;
		const std::string command = std::format("git -C {} -c core.quotePath=false status --porcelain=v1 -b 2>nul", QuoteCommandArgument(summary.RootPath));
		const std::vector<std::string> lines = ReadCommandLines(command, exitCode);
		if (exitCode != 0)
		{
			summary.Message = "Git status unavailable. The project may not be a Git repository, or git is not on PATH.";
			m_SourceControlSummary = std::move(summary);
			return;
		}

		summary.IsGitRepository = true;
		for (const std::string& line : lines)
		{
			if (line.rfind("## ", 0) == 0)
			{
				ParseSourceControlBranchLine(line, summary);
				continue;
			}

			if (line.size() < 2)
			{
				continue;
			}

			const char indexStatus = line[0];
			const char workTreeStatus = line[1];
			if (SourceControlLineHasStagedChange(line))
			{
				++summary.StagedCount;
			}
			if (SourceControlLineHasUnstagedChange(line))
			{
				++summary.UnstagedCount;
			}
			const auto hasStatus = [indexStatus, workTreeStatus](char status)
				{
					return indexStatus == status || workTreeStatus == status;
				};

			if (indexStatus == '?' && workTreeStatus == '?')
			{
				++summary.UntrackedCount;
			}
			else if (SourceControlLineIsConflict(line))
			{
				++summary.ConflictedCount;
			}
			else
			{
				if (hasStatus('R') || hasStatus('C'))
				{
					++summary.RenamedCount;
				}
				if (hasStatus('A'))
				{
					++summary.AddedCount;
				}
				if (hasStatus('D'))
				{
					++summary.DeletedCount;
				}
				if (hasStatus('M') || hasStatus('T'))
				{
					++summary.ModifiedCount;
				}
				if (!hasStatus('R') && !hasStatus('C') && !hasStatus('A') && !hasStatus('D') && !hasStatus('M') && !hasStatus('T'))
				{
					++summary.ModifiedCount;
				}
			}

			summary.ChangedFiles.push_back(line);
		}

		summary.IsClean = summary.ChangedFiles.empty();
		summary.Message = summary.IsClean
			? "Working tree clean."
			: std::format("{} changed file(s).", summary.ChangedFiles.size());
		m_SourceControlSummary = std::move(summary);
	}

	bool EditorLayer::RunSourceControlCommand(const EditorContext& context, std::string_view gitArguments, std::string_view successMessage)
	{
		const std::filesystem::path rootPath = context.ProjectRootPath.empty() ? std::filesystem::current_path() : context.ProjectRootPath;
		std::error_code errorCode;
		if (rootPath.empty() || !std::filesystem::exists(rootPath, errorCode))
		{
			m_SourceControlOperationStatus = "Git command skipped: project root does not exist.";
			return false;
		}

		int exitCode = -1;
		const std::string command = std::format("git -C {} {} 2>&1", QuoteCommandArgument(rootPath), std::string(gitArguments));
		const std::vector<std::string> lines = ReadCommandLines(command, exitCode);
		std::string output;
		for (const std::string& line : lines)
		{
			if (!output.empty())
			{
				output.append(" ");
			}
			output.append(line);
			if (output.size() > 320)
			{
				output.resize(320);
				output.append("...");
				break;
			}
		}

		const bool succeeded = exitCode == 0;
		m_SourceControlOperationStatus = succeeded
			? std::string(successMessage)
			: std::format("Git command failed (exit {}): {}", exitCode, output.empty() ? "no output" : output);
		RefreshSourceControlStatus(context);
		return succeeded;
	}

	void EditorLayer::DrawSourceControlStatus(EditorContext& context)
	{
		if (m_SourceControlSummary.RootPath != context.ProjectRootPath)
		{
			m_SourceControlSummary = {};
		}

		if (m_SourceControlSummary.LastRefreshTime < 0.0)
		{
			RefreshSourceControlStatus(context);
		}

		if (!ImGui::CollapsingHeader("Source Control", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (ImGui::Button("Refresh Git Status"))
		{
			RefreshSourceControlStatus(context);
		}

		const SourceControlSummary& summary = m_SourceControlSummary;
		ImGui::TextWrapped("Root: %s", summary.RootPath.empty() ? "<none>" : summary.RootPath.string().c_str());
		if (!summary.IsGitRepository)
		{
			ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.38f, 1.0f), "%s", summary.Message.c_str());
			return;
		}

		ImGui::Text("Branch: %s", summary.Branch.c_str());
		if (summary.HasUpstream)
		{
			ImGui::Text("Upstream: %s | Ahead %d | Behind %d", summary.Upstream.c_str(), summary.AheadCount, summary.BehindCount);
		}
		else
		{
			ImGui::TextDisabled("Upstream: <not configured>");
		}
		ImGui::TextColored(
			summary.IsClean ? ImVec4(0.45f, 0.95f, 0.55f, 1.0f) : ImVec4(0.95f, 0.78f, 0.38f, 1.0f),
			"%s",
			summary.Message.c_str());
		ImGui::Text(
			"Modified %zu | Added %zu | Deleted %zu | Renamed %zu | Untracked %zu | Conflicts %zu",
			summary.ModifiedCount,
			summary.AddedCount,
			summary.DeletedCount,
			summary.RenamedCount,
			summary.UntrackedCount,
			summary.ConflictedCount);
		ImGui::Text("Staged %zu | Unstaged %zu", summary.StagedCount, summary.UnstagedCount);
		if (summary.LastRefreshTime >= 0.0)
		{
			ImGui::TextDisabled("Last refresh: %.1f sec editor time", summary.LastRefreshTime);
		}
		if (!m_SourceControlOperationStatus.empty())
		{
			ImGui::TextWrapped("%s", m_SourceControlOperationStatus.c_str());
		}

		ImGui::SeparatorText("Actions");
		ImGui::BeginDisabled(summary.IsClean || summary.ConflictedCount > 0);
		if (ImGui::Button("Stage All"))
		{
			static_cast<void>(RunSourceControlCommand(context, "add -A -- .", "Staged all working tree changes."));
			return;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(summary.StagedCount == 0);
		if (ImGui::Button("Unstage All"))
		{
			static_cast<void>(RunSourceControlCommand(context, "restore --staged -- .", "Unstaged all indexed changes."));
			return;
		}
		ImGui::EndDisabled();

		ImGui::InputText("Commit Message", m_SourceControlCommitMessageBuffer.data(), m_SourceControlCommitMessageBuffer.size());
		const std::string commitMessage = TrimCopy(std::string_view(m_SourceControlCommitMessageBuffer.data()));
		ImGui::BeginDisabled(summary.StagedCount == 0 || commitMessage.empty() || summary.ConflictedCount > 0);
		if (ImGui::Button("Commit Staged"))
		{
			const std::string arguments = std::format("commit -m {}", QuoteShellArgument(std::string_view(commitMessage)));
			if (RunSourceControlCommand(context, arguments, "Committed staged changes."))
			{
				m_SourceControlCommitMessageBuffer.fill('\0');
			}
			return;
		}
		ImGui::EndDisabled();
		if (summary.ConflictedCount > 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Resolve conflicts before staging or committing from the editor.");
		}
		ImGui::SameLine();
		const bool detachedBranch = summary.Branch == "<detached>" || summary.Branch.empty();
		const bool canPushBranch = !detachedBranch && summary.ConflictedCount == 0 && summary.BehindCount == 0;
		ImGui::BeginDisabled(!canPushBranch || !summary.HasUpstream);
		if (ImGui::Button("Push Branch"))
		{
			m_SourceControlPushSetUpstream = false;
			ImGui::OpenPopup("Confirm Git Push");
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip(summary.HasUpstream
				? "Push committed changes to the configured upstream."
				: "No upstream is configured. Use Set Upstream + Push.");
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!canPushBranch || summary.HasUpstream);
		if (ImGui::Button("Set Upstream + Push"))
		{
			m_SourceControlPushSetUpstream = true;
			ImGui::OpenPopup("Confirm Git Push");
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Run git push -u origin <branch>. Disabled when an upstream already exists.");
		}
		if (summary.BehindCount > 0)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "Push is disabled because this branch is behind upstream. Pull/rebase outside the editor first.");
		}
		if (summary.AheadCount > 0)
		{
			ImGui::TextDisabled("%d committed change(s) are ahead of upstream.", summary.AheadCount);
		}
		else if (summary.HasUpstream)
		{
			ImGui::TextDisabled("No ahead commits reported by git status.");
		}
		if (ImGui::BeginPopupModal("Confirm Git Push", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Run Git push from the project root?");
			ImGui::Text("Branch: %s", summary.Branch.c_str());
			if (m_SourceControlPushSetUpstream)
			{
				ImGui::Text("Command: git push -u origin %s", summary.Branch.c_str());
			}
			else
			{
				ImGui::Text("Upstream: %s", summary.Upstream.empty() ? "<default>" : summary.Upstream.c_str());
				ImGui::TextUnformatted("Command: git push");
			}
			if (!summary.IsClean)
			{
				ImGui::TextDisabled("Note: uncommitted changes are not pushed; only commits are sent.");
			}
			ImGui::Separator();
			if (ImGui::Button("Push"))
			{
				const std::string arguments = m_SourceControlPushSetUpstream
					? std::format("push -u origin {}", QuoteShellArgument(std::string_view(summary.Branch)))
					: std::string("push");
				static_cast<void>(RunSourceControlCommand(context, arguments, "Pushed branch to remote."));
				ImGui::CloseCurrentPopup();
				return;
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (summary.ConflictedCount > 0 && ImGui::TreeNodeEx("Conflict Resolver", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextWrapped("Resolve conflict markers in each file, then use Mark Resolved. This editor action only runs git add for the selected file.");
			for (size_t fileIndex = 0; fileIndex < summary.ChangedFiles.size(); ++fileIndex)
			{
				const std::string& changedLine = summary.ChangedFiles[fileIndex];
				if (!SourceControlLineIsConflict(changedLine))
				{
					continue;
				}

				const std::string changedPath = SourceControlPathFromPorcelainLine(changedLine);
				const std::filesystem::path absolutePath = summary.RootPath / std::filesystem::path(changedPath);
				ImGui::PushID(static_cast<int>(fileIndex));
				ImGui::TextWrapped("%s", changedLine.c_str());
				ImGui::BeginDisabled(changedPath.empty() || !context.OnAssetOpen);
				if (ImGui::SmallButton("Open"))
				{
					context.OnAssetOpen(absolutePath);
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(changedPath.empty() || !context.OnAssetReveal);
				if (ImGui::SmallButton("Reveal"))
				{
					context.OnAssetReveal(absolutePath);
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(changedPath.empty());
				if (ImGui::SmallButton("Mark Resolved"))
				{
					const std::string arguments = std::format("add -- {}", QuoteShellArgument(std::string_view(changedPath)));
					static_cast<void>(RunSourceControlCommand(context, arguments, std::format("Marked {} as resolved.", changedPath)));
					ImGui::EndDisabled();
					ImGui::PopID();
					ImGui::TreePop();
					return;
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::TreePop();
		}

		if (!summary.ChangedFiles.empty() && ImGui::TreeNodeEx("Changed Files", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const size_t maxVisibleFiles = (std::min)(summary.ChangedFiles.size(), static_cast<size_t>(48));
			for (size_t fileIndex = 0; fileIndex < maxVisibleFiles; ++fileIndex)
			{
				const std::string& changedLine = summary.ChangedFiles[fileIndex];
				const std::string changedPath = SourceControlPathFromPorcelainLine(changedLine);
				const bool hasStagedChange = SourceControlLineHasStagedChange(changedLine);
				const bool hasUnstagedChange = SourceControlLineHasUnstagedChange(changedLine);
				ImGui::PushID(static_cast<int>(fileIndex));
				ImGui::TextWrapped("%s", changedLine.c_str());
				ImGui::SameLine();
				ImGui::BeginDisabled(!hasUnstagedChange || changedPath.empty() || summary.ConflictedCount > 0);
				if (ImGui::SmallButton("Stage"))
				{
					const std::string arguments = std::format("add -- {}", QuoteShellArgument(std::string_view(changedPath)));
					static_cast<void>(RunSourceControlCommand(context, arguments, std::format("Staged {}.", changedPath)));
					ImGui::EndDisabled();
					ImGui::PopID();
					ImGui::TreePop();
					return;
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!hasStagedChange || changedPath.empty());
				if (ImGui::SmallButton("Unstage"))
				{
					const std::string arguments = std::format("restore --staged -- {}", QuoteShellArgument(std::string_view(changedPath)));
					static_cast<void>(RunSourceControlCommand(context, arguments, std::format("Unstaged {}.", changedPath)));
					ImGui::EndDisabled();
					ImGui::PopID();
					ImGui::TreePop();
					return;
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			if (summary.ChangedFiles.size() > maxVisibleFiles)
			{
				ImGui::TextDisabled("... %zu more file(s)", summary.ChangedFiles.size() - maxVisibleFiles);
			}
			ImGui::TreePop();
		}
	}

	void EditorLayer::ExecuteConsoleCommand(EditorContext& context, std::string_view commandView)
	{
		const std::string command = TrimCopy(commandView);
		if (command.empty())
		{
			return;
		}

		const auto appendHistory = [this](std::string line)
			{
				m_ConsoleCommandHistory.push_back(std::move(line));
				constexpr size_t maxHistoryLines = 96;
				if (m_ConsoleCommandHistory.size() > maxHistoryLines)
				{
					m_ConsoleCommandHistory.erase(
						m_ConsoleCommandHistory.begin(),
						m_ConsoleCommandHistory.begin() + static_cast<std::ptrdiff_t>(m_ConsoleCommandHistory.size() - maxHistoryLines));
				}
			};

		appendHistory(std::format("> {}", command));
		const std::vector<std::string> args = TokenizeCommand(command);
		if (args.empty())
		{
			return;
		}

		const std::string verb = ToLower(args.front());
		const auto finish = [&](std::string message)
			{
				appendHistory(std::move(message));
			};
		const auto disabled = [&finish](std::string_view label)
			{
				finish(std::format("{} is not available in the current editor state.", label));
			};
		const auto parseFloat = [](const std::string& text, float& value) -> bool
			{
				try
				{
					size_t parsed = 0;
					value = std::stof(text, &parsed);
					return parsed == text.size();
				}
				catch (...)
				{
					return false;
				}
			};
		const auto parseEntityId = [](const std::string& text, EntityId& entityId) -> bool
			{
				try
				{
					size_t parsed = 0;
					const unsigned long value = std::stoul(text, &parsed, 10);
					if (parsed != text.size())
					{
						return false;
					}
					entityId = static_cast<EntityId>(value);
					return true;
				}
				catch (...)
				{
					return false;
				}
			};

		if (verb == "clear" || verb == "cls")
		{
			m_ConsoleCommandHistory.clear();
			return;
		}
		if (verb == "help" || verb == "?")
		{
			finish("Commands: help, shortcuts, clear, status, save, saveas, open, reveal, refresh, export, play, stop, pause, resume, step, resetplay, frame, undo, redo.");
			finish("Editor: unsaved, content, layout save|restore|reset|status.");
			finish("Rendering: api dx12|vulkan, render forward|deferred|forward+.");
			finish("Scene: create empty|camera|light|cube|sphere|capsule|plane, select <entityId>, load <modelPath>.");
			finish("Autosave: autosave on|off|status|interval <seconds>.");
			finish("Source Control: git status|stage all|unstage all|commit <message>|push|resolve <path>, scm refresh.");
			return;
		}
		if (verb == "shortcuts" || verb == "keys")
		{
			OpenShortcutReference();
			finish("Keyboard Shortcut Reference opened.");
			return;
		}
		if (verb == "status")
		{
			finish(std::format("Project: {} | Scene: {} | Dirty: {}",
				context.ProjectName,
				context.CurrentScenePath.empty() ? std::string("<none>") : context.CurrentScenePath.string(),
				context.IsSceneDirty ? "yes" : "no"));
			finish(std::format("API: {} | Render: {} | Play: {}",
				GraphicsApiName(context.CurrentApi),
				RenderModeToString(context.CurrentRenderMode),
				context.PlayState == EditorPlayState::Play
					? "play"
					: context.PlayState == EditorPlayState::Paused ? "paused" : "edit"));
			return;
		}
		if (verb == "save")
		{
			if (context.CanEditProjectScene && context.OnSaveScene)
			{
				context.OnSaveScene();
				finish("Save Scene requested.");
			}
			else
			{
				disabled("Save Scene");
			}
			return;
		}
		if (verb == "saveas")
		{
			if (context.CanEditProjectScene && context.OnSaveSceneAs)
			{
				context.OnSaveSceneAs();
				finish("Save Scene As requested.");
			}
			else
			{
				disabled("Save Scene As");
			}
			return;
		}
		if (verb == "open")
		{
			if (context.CanEditProjectScene && context.OnOpenSceneDialog)
			{
				context.OnOpenSceneDialog();
				finish("Open Scene dialog requested.");
			}
			else
			{
				disabled("Open Scene");
			}
			return;
		}
		if (verb == "reveal")
		{
			if (context.OnRevealProject)
			{
				context.OnRevealProject();
				finish("Reveal Project requested.");
			}
			else
			{
				disabled("Reveal Project");
			}
			return;
		}
		if (verb == "refresh")
		{
			if (context.OnProjectRefresh)
			{
				context.OnProjectRefresh();
				finish("Project refresh requested.");
			}
			else
			{
				disabled("Project Refresh");
			}
			return;
		}
		if (verb == "export")
		{
			if (context.CanEditProjectScene && (context.OnExportProjectProfile || context.OnExportProject))
			{
				if (context.OnExportProjectProfile)
				{
					EnsureExportProfileDefaults(context);
					const ExportProfileSettings settings = BuildExportProfileSettings(context);
					const bool success = context.OnExportProjectProfile(settings);
					m_LastExportOutputDirectory = settings.OutputDirectory;
					m_LastExportStatusMessage = success ? "Export succeeded." : "Export failed. Check Console log for details.";
					finish(success
						? std::format("Project export completed: {}", settings.OutputDirectory.string())
						: "Project export failed.");
				}
				else
				{
					context.OnExportProject();
					finish("Project export requested.");
				}
			}
			else
			{
				disabled("Export Project");
			}
			return;
		}
		if (verb == "play" || verb == "stop")
		{
			if (context.CanControlPlayMode && context.OnPlayModeChanged)
			{
				context.OnPlayModeChanged(verb == "play");
				finish(verb == "play" ? "Play mode requested." : "Stop play mode requested.");
			}
			else
			{
				disabled("Play Mode");
			}
			return;
		}
		if (verb == "pause" || verb == "resume")
		{
			if (context.CanControlPlayMode && context.OnPlayPausedChanged)
			{
				context.OnPlayPausedChanged(verb == "pause");
				finish(verb == "pause" ? "Play pause requested." : "Play resume requested.");
			}
			else
			{
				disabled(verb == "pause" ? "Pause Play Mode" : "Resume Play Mode");
			}
			return;
		}
		if (verb == "step")
		{
			if (context.CanControlPlayMode && context.PlayState == EditorPlayState::Paused && context.OnPlayStep)
			{
				context.OnPlayStep();
				finish("Play single-frame step requested.");
			}
			else
			{
				disabled("Step Play Mode");
			}
			return;
		}
		if (verb == "resetplay" || verb == "playreset")
		{
			if (context.CanControlPlayMode && context.ActiveSceneIsRuntimeClone && context.OnResetPlayRuntimeScene)
			{
				context.OnResetPlayRuntimeScene();
				finish("Play runtime reset requested.");
			}
			else
			{
				disabled("Reset Play Runtime");
			}
			return;
		}
		if (verb == "frame")
		{
			if (context.OnFrameSelected)
			{
				context.OnFrameSelected();
				finish("Frame Selected requested.");
			}
			else
			{
				disabled("Frame Selected");
			}
			return;
		}
		if (verb == "undo" || verb == "redo")
		{
			if (verb == "undo" && context.CanUndo && context.OnUndo)
			{
				context.OnUndo();
				finish("Undo requested.");
			}
			else if (verb == "redo" && context.CanRedo && context.OnRedo)
			{
				context.OnRedo();
				finish("Redo requested.");
			}
			else
			{
				disabled(verb == "undo" ? "Undo" : "Redo");
			}
			return;
		}
		if (verb == "unsaved")
		{
			if (!context.CanEditProjectScene)
			{
				finish("Unsaved scene tracking is active for Project Scene mode.");
			}
			else if (context.IsSceneDirty)
			{
				finish(std::format("Unsaved scene: {}",
					context.CurrentScenePath.empty() ? std::string("<untitled>") : context.CurrentScenePath.string()));
				if (!context.AutosavePath.empty())
				{
					finish(std::format("Autosave snapshot: {}", context.AutosavePath.string()));
				}
			}
			else
			{
				finish("No unsaved scenes.");
			}
			return;
		}
		if (verb == "content" || verb == "drawer")
		{
			OpenContentDrawer();
			finish("Content Drawer opened.");
			return;
		}
		if (verb == "layout")
		{
			const std::string action = args.size() > 1 ? ToLower(args[1]) : "status";
			if (action == "save")
			{
				SaveCurrentEditorLayout(context);
				finish(m_EditorLayoutStatusMessage);
			}
			else if (action == "restore" || action == "load")
			{
				RestoreSavedEditorLayout();
				finish(m_EditorLayoutStatusMessage);
			}
			else if (action == "reset" || action == "default")
			{
				ResetEditorLayoutToDefault();
				finish(m_EditorLayoutStatusMessage);
			}
			else if (action == "status")
			{
				finish(std::format("Editor layout: saved={} restored={} bytes={}",
					m_ProjectEditorLayoutIni.empty() ? "no" : "yes",
					m_ProjectEditorLayoutRestored ? "yes" : "no",
					m_ProjectEditorLayoutIni.size()));
			}
			else
			{
				finish("Usage: layout save|restore|reset|status");
			}
			return;
		}
		if (verb == "api")
		{
			if (args.size() < 2 || !context.OnGraphicsApiChanged)
			{
				finish("Usage: api dx12|vulkan");
				return;
			}

			const std::string apiName = ToLower(args[1]);
			if (apiName == "dx12" || apiName == "directx12" || apiName == "d3d12")
			{
				context.OnGraphicsApiChanged(GraphicsAPI::DirectX12);
				finish("Graphics API switch requested: DirectX12.");
			}
			else if (apiName == "vulkan" || apiName == "vk")
			{
				context.OnGraphicsApiChanged(GraphicsAPI::Vulkan);
				finish("Graphics API switch requested: Vulkan.");
			}
			else
			{
				finish("Usage: api dx12|vulkan");
			}
			return;
		}
		if (verb == "render")
		{
			if (args.size() < 2 || !context.OnRenderModeChanged)
			{
				finish("Usage: render forward|deferred|forward+");
				return;
			}

			const std::string renderMode = ToLower(args[1]);
			if (renderMode == "forward")
			{
				context.OnRenderModeChanged(RenderMode::Forward);
				finish("Render mode requested: Forward.");
			}
			else if (renderMode == "deferred")
			{
				context.OnRenderModeChanged(RenderMode::Deferred);
				finish("Render mode requested: Deferred.");
			}
			else if (renderMode == "forward+" || renderMode == "forwardplus" || renderMode == "fplus")
			{
				context.OnRenderModeChanged(RenderMode::ForwardPlus);
				finish("Render mode requested: Forward+.");
			}
			else
			{
				finish("Usage: render forward|deferred|forward+");
			}
			return;
		}
		if (verb == "autosave")
		{
			if (args.size() < 2)
			{
				finish("Usage: autosave on|off|status|interval <seconds>");
				return;
			}

			const std::string mode = ToLower(args[1]);
			if (mode == "on" || mode == "enable")
			{
				if (context.OnAutosaveEnabledChanged)
				{
					context.OnAutosaveEnabledChanged(true);
					finish("Autosave enabled.");
				}
				else
				{
					disabled("Autosave");
				}
			}
			else if (mode == "off" || mode == "disable")
			{
				if (context.OnAutosaveEnabledChanged)
				{
					context.OnAutosaveEnabledChanged(false);
					finish("Autosave disabled.");
				}
				else
				{
					disabled("Autosave");
				}
			}
			else if (mode == "status")
			{
				finish(std::format("Autosave: {} | interval {:.0f}s | elapsed {:.0f}s | last {}",
					context.AutosaveEnabled ? "on" : "off",
					context.AutosaveIntervalSeconds,
					context.AutosaveElapsedSeconds,
					context.AutosavePath.empty() ? std::string("<none>") : context.AutosavePath.string()));
			}
			else if (mode == "interval")
			{
				float interval = 0.0f;
				if (args.size() < 3 || !parseFloat(args[2], interval))
				{
					finish("Usage: autosave interval <seconds>");
					return;
				}
				if (context.OnAutosaveIntervalChanged)
				{
					context.OnAutosaveIntervalChanged(interval);
					finish(std::format("Autosave interval requested: {:.0f}s.", interval));
				}
				else
				{
					disabled("Autosave Interval");
				}
			}
			else
			{
				finish("Usage: autosave on|off|status|interval <seconds>");
			}
			return;
		}
		if (verb == "git" || verb == "scm")
		{
			const std::string action = args.size() > 1 ? ToLower(args[1]) : "status";
			if (action == "status" || action == "refresh")
			{
				RefreshSourceControlStatus(context);
				const SourceControlSummary& summary = m_SourceControlSummary;
				if (!summary.IsGitRepository)
				{
					finish(summary.Message);
				}
				else
				{
					finish(std::format("Git {}{}: {} | modified {} added {} deleted {} renamed {} untracked {} conflicts {}",
						summary.Branch,
						summary.HasUpstream
							? std::format(" -> {} ahead {} behind {}", summary.Upstream, summary.AheadCount, summary.BehindCount)
							: std::string(""),
						summary.Message,
						summary.ModifiedCount,
						summary.AddedCount,
						summary.DeletedCount,
						summary.RenamedCount,
						summary.UntrackedCount,
						summary.ConflictedCount));
				}
			}
			else if (action == "stage" && args.size() > 2 && ToLower(args[2]) == "all")
			{
				static_cast<void>(RunSourceControlCommand(context, "add -A -- .", "Staged all working tree changes."));
				finish(m_SourceControlOperationStatus);
			}
			else if (action == "unstage" && args.size() > 2 && ToLower(args[2]) == "all")
			{
				static_cast<void>(RunSourceControlCommand(context, "restore --staged -- .", "Unstaged all indexed changes."));
				finish(m_SourceControlOperationStatus);
			}
			else if (action == "commit")
			{
				std::string message;
				for (size_t index = 2; index < args.size(); ++index)
				{
					if (!message.empty())
					{
						message.push_back(' ');
					}
					message.append(args[index]);
				}
				message = TrimCopy(message);
				if (message.empty())
				{
					finish("Usage: git commit <message>");
				}
				else
				{
					const std::string arguments = std::format("commit -m {}", QuoteShellArgument(std::string_view(message)));
					static_cast<void>(RunSourceControlCommand(context, arguments, "Committed staged changes."));
					finish(m_SourceControlOperationStatus);
				}
			}
			else if (action == "push")
			{
				RefreshSourceControlStatus(context);
				const SourceControlSummary& summary = m_SourceControlSummary;
				if (!summary.IsGitRepository)
				{
					finish(summary.Message);
				}
				else if (summary.ConflictedCount > 0)
				{
					finish("Git push skipped: resolve conflicts first.");
				}
				else if (summary.BehindCount > 0)
				{
					finish("Git push skipped: branch is behind upstream. Pull/rebase outside the editor first.");
				}
				else if (args.size() > 2 && ToLower(args[2]) == "set-upstream")
				{
					if (summary.Branch.empty() || summary.Branch == "<detached>")
					{
						finish("Git push skipped: detached branch cannot set upstream.");
					}
					else
					{
						const std::string arguments = std::format("push -u origin {}", QuoteShellArgument(std::string_view(summary.Branch)));
						static_cast<void>(RunSourceControlCommand(context, arguments, "Pushed branch and configured upstream."));
						finish(m_SourceControlOperationStatus);
					}
				}
				else
				{
					static_cast<void>(RunSourceControlCommand(context, "push", "Pushed branch to remote."));
					finish(m_SourceControlOperationStatus);
				}
			}
			else if (action == "resolve")
			{
				std::string path;
				for (size_t index = 2; index < args.size(); ++index)
				{
					if (!path.empty())
					{
						path.push_back(' ');
					}
					path.append(args[index]);
				}
				path = TrimCopy(path);
				if (path.empty())
				{
					finish("Usage: git resolve <path>");
				}
				else
				{
					const std::string arguments = std::format("add -- {}", QuoteShellArgument(std::string_view(path)));
					static_cast<void>(RunSourceControlCommand(context, arguments, std::format("Marked {} as resolved.", path)));
					finish(m_SourceControlOperationStatus);
				}
			}
			else
			{
				finish("Usage: git status | git stage all | git unstage all | git commit <message> | git push [set-upstream] | git resolve <path> | scm refresh");
			}
			return;
		}
		if (verb == "create")
		{
			if (args.size() < 2)
			{
				finish("Usage: create empty|camera|light|cube|sphere|capsule|plane");
				return;
			}

			const EntityId defaultParent = (m_DefaultParentEntity != InvalidEntityId && context.ActiveScene.ContainsEntity(m_DefaultParentEntity))
				? m_DefaultParentEntity
				: InvalidEntityId;
			const std::string kind = ToLower(args[1]);
			if (kind == "empty" || kind == "entity")
			{
				if (context.CanEditProjectScene && context.OnCreateEmptyEntity)
				{
					context.OnCreateEmptyEntity(defaultParent);
					finish("Empty entity created.");
				}
				else
				{
					disabled("Create Empty Entity");
				}
			}
			else if (kind == "camera")
			{
				if (context.CanEditProjectScene && context.OnCreateCameraEntity)
				{
					context.OnCreateCameraEntity(defaultParent);
					finish("Camera entity created.");
				}
				else
				{
					disabled("Create Camera");
				}
			}
			else if (kind == "light")
			{
				if (context.CanEditProjectScene && context.OnCreateLightEntity)
				{
					context.OnCreateLightEntity(defaultParent);
					finish("Light entity created.");
				}
				else
				{
					disabled("Create Light");
				}
			}
			else
			{
				const std::unordered_map<std::string, Asset::PrimitiveMeshKind> primitiveKinds = {
					{ "cube", Asset::PrimitiveMeshKind::Cube },
					{ "sphere", Asset::PrimitiveMeshKind::Sphere },
					{ "capsule", Asset::PrimitiveMeshKind::Capsule },
					{ "plane", Asset::PrimitiveMeshKind::Plane },
				};
				const auto primitiveIt = primitiveKinds.find(kind);
				if (primitiveIt != primitiveKinds.end() && context.CanEditProjectScene && context.OnCreatePrimitive)
				{
					context.OnCreatePrimitive(primitiveIt->second, defaultParent);
					finish(std::format("{} primitive created.", kind));
				}
				else
				{
					finish("Usage: create empty|camera|light|cube|sphere|capsule|plane");
				}
			}
			return;
		}
		if (verb == "select")
		{
			EntityId entityId = InvalidEntityId;
			if (args.size() < 2 || !parseEntityId(args[1], entityId))
			{
				finish("Usage: select <entityId>");
				return;
			}
			if (context.ActiveScene.ContainsEntity(entityId))
			{
				context.ActiveScene.SetSelectedEntity(entityId);
				finish(std::format("Selected entity {}.", entityId));
			}
			else
			{
				finish(std::format("Entity {} does not exist.", entityId));
			}
			return;
		}
		if (verb == "load")
		{
			if (args.size() < 2 || !context.OnModelDrop)
			{
				finish("Usage: load <modelPath>");
				return;
			}

			std::filesystem::path modelPath(args[1]);
			if (!modelPath.is_absolute() && context.ProjectSnapshot && context.ProjectSnapshot->RootExists)
			{
				modelPath = context.ProjectSnapshot->RootPath / modelPath;
			}
			if (!Asset::IsModelAssetPath(modelPath))
			{
				finish("Load supports model assets only: .fbx, .obj, .gltf, .glb.");
				return;
			}
			context.OnModelDrop(modelPath, AssetDropTarget::Game);
			finish(std::format("Model import requested: {}", modelPath.string()));
			return;
		}

		finish(std::format("Unknown command '{}'. Type 'help' for available commands.", args.front()));
	}

	void EditorLayer::DrawConsole(EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Console", viewport->Size.x * 0.76f, viewport->Size.y - 250.0f, viewport->Size.x * 0.23f, 240.0f);
		ImGui::Begin("Console");
		ImGui::Text("API: %s", GraphicsApiName(context.CurrentApi));
		ImGui::Text("Project: %s", context.ProjectName.c_str());
		if (!context.ProjectRootPath.empty())
		{
			ImGui::TextWrapped("Root: %s", context.ProjectRootPath.string().c_str());
		}
		ImGui::Text("Render Mode: %s", RenderModeToString(context.CurrentRenderMode).data());
		ImGui::Text("Sample Mode: %s", SampleModeName(context.SampleMode));
		if (const std::string* selectedEntityName = context.ActiveScene.GetEntityName(context.ActiveScene.GetSelectedEntity()))
		{
			ImGui::Text("Selected Entity: %s", selectedEntityName->c_str());
		}
		if (!m_SelectedAssetPath.empty())
		{
			ImGui::Text("Selected Asset: %s", m_SelectedAssetPath.filename().string().c_str());
		}
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##ConsoleLogFilter", "Filter console logs...", m_ConsoleLogFilter.data(), m_ConsoleLogFilter.size());
		ImGui::SeparatorText("Command");
		ImGui::SetNextItemWidth(-86.0f);
		const bool commandSubmitted = ImGui::InputTextWithHint(
			"##ConsoleCommandInput",
			"help, status, save, play, render deferred, create cube...",
			m_ConsoleCommandBuffer.data(),
			m_ConsoleCommandBuffer.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		const bool commandButtonPressed = ImGui::Button("Run", ImVec2(78.0f, 0.0f));
		if (commandSubmitted || commandButtonPressed)
		{
			ExecuteConsoleCommand(context, std::string_view(m_ConsoleCommandBuffer.data()));
			m_ConsoleCommandBuffer.fill('\0');
			ImGui::SetKeyboardFocusHere(-1);
		}
		if (!m_ConsoleCommandHistory.empty())
		{
			if (ImGui::BeginChild("ConsoleCommandHistory", ImVec2(0.0f, 112.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
			{
				for (const std::string& line : m_ConsoleCommandHistory)
				{
					ImGui::TextWrapped("%s", line.c_str());
				}
				if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
				{
					ImGui::SetScrollHereY(1.0f);
				}
			}
			ImGui::EndChild();
		}
		DrawUnsavedSceneList(context);
		DrawEditorLayoutTools(context);
		DrawExportProfile(context);
		DrawSourceControlStatus(context);
		if (ImGui::CollapsingHeader("Renderer Roadmap Health", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("RendererRoadmapHealthTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.0f);
				ImGui::TableSetupColumn("Item");
				ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Evidence");
				ImGui::TableHeadersRow();

				DrawRoadmapHealthRow(
					1,
					"Resource system",
					context.ResourceStats.GroupCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Warning,
					std::format("{} group(s), {} declared, {} loaded, {} failed",
						context.ResourceStats.GroupCount,
						context.ResourceStats.ResourceCount,
						context.ResourceStats.LoadedCount,
						context.ResourceStats.FailedCount));
				DrawRoadmapHealthRow(
					2,
					"Material and shader variants",
					context.ShaderVariantStats.RequestCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Ready,
					std::format("{} material(s), {} variant(s), {} request(s)",
						context.MaterialStats.MaterialCount,
						context.ShaderVariantStats.VariantCount,
						context.ShaderVariantStats.RequestCount));
				DrawRoadmapHealthRow(
					3,
					"RenderGraph frame graph",
					context.RenderGraphStats.PassCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Warning,
					std::format("{} / {} pass(es) enabled for {}",
						context.RenderGraphStats.EnabledPassCount,
						context.RenderGraphStats.PassCount,
						RenderModeToString(context.CurrentRenderMode)));
				DrawRoadmapHealthRow(
					4,
					"Shadow pass",
					context.ShadowStats.Enabled ? (context.ShadowStats.HasDirectionalCaster ? RoadmapHealthState::Active : RoadmapHealthState::Ready) : RoadmapHealthState::Idle,
					std::format("{} shadow map, caster {}, {} draw call(s)",
						context.ShadowStats.Enabled ? std::to_string(context.ShadowStats.MapSize) : std::string("disabled"),
						context.ShadowStats.HasDirectionalCaster ? "yes" : "no",
						context.RenderFrameStats.ShadowDrawCallCount));
				DrawRoadmapHealthRow(
					5,
					"HDR and tone mapping",
					(context.RenderGraphStats.UsesHdr && context.PostProcessStats.UsesHdrTarget && context.PostProcessStats.ToneMappingEnabled)
						? RoadmapHealthState::Active
						: RoadmapHealthState::Warning,
					std::format("HDR {}, tone map {}, exposure {:.2f}",
						context.PostProcessStats.UsesHdrTarget ? "yes" : "no",
						context.PostProcessStats.ToneMappingEnabled ? context.PostProcessStats.ToneMapper : std::string_view("off"),
						context.PostProcessStats.Exposure));
				DrawRoadmapHealthRow(
					6,
					"Forward 8 lights / Deferred unlimited",
					(context.ForwardLightLimit == 8 && context.DeferredLightBufferCapacity >= context.DeferredLightCount)
						? RoadmapHealthState::Ready
						: RoadmapHealthState::Warning,
					std::format("Forward uses {} / {}, Deferred active {} with {} slot buffer",
						context.ForwardLightUsedCount,
						context.ForwardLightLimit,
						context.DeferredLightCount,
						context.DeferredLightBufferCapacity));
				DrawRoadmapHealthRow(
					7,
					"Deferred tiled light culling",
					context.CurrentRenderMode == RenderMode::Deferred
						? (context.DeferredTileViewportCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Ready)
						: RoadmapHealthState::Idle,
					std::format("{} viewport pass(es), {} tile(s), {} light reference(s)",
						context.DeferredTileViewportCount,
						context.DeferredTileCountTotal,
						context.DeferredTileLightReferenceCount));
				DrawRoadmapHealthRow(
					8,
					"Per-pass CPU timings",
					context.RenderGraphStats.TimedPassCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Ready,
					std::format("{} timed pass(es), {:.3f} ms exclusive sum",
						context.RenderGraphStats.TimedPassCount,
						context.RenderGraphStats.TotalCpuMs));
				DrawRoadmapHealthRow(
					9,
					"Render frame counters",
					context.RenderFrameStats.FrameIndex > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Ready,
					std::format("{} draw call(s), {} triangle(s), {} instance(s)",
						context.RenderFrameStats.DrawCallCount,
						context.RenderFrameStats.SubmittedTriangleCount,
						context.RenderFrameStats.SubmittedInstanceCount));
				DrawRoadmapHealthRow(
					10,
					"Viewport frustum culling",
					context.ViewFrustumCullingEnabled
						? (context.RenderFrameStats.ViewCullingRequestCount > 0 ? RoadmapHealthState::Active : RoadmapHealthState::Ready)
						: RoadmapHealthState::Idle,
					std::format("{} request(s), {} test(s), {} culled result(s)",
						context.RenderFrameStats.ViewCullingRequestCount,
						context.RenderFrameStats.ViewCullingTestCount,
						context.RenderFrameStats.ViewCulledEntityCount));

				ImGui::EndTable();
			}
		}
		if (ImGui::CollapsingHeader("Memory"))
		{
			const auto& memoryStats = context.MemoryStats;
			ImGui::Text(
				"Frame Arena: %.2f / %.2f MB (Peak %.2f MB)",
				static_cast<double>(memoryStats.FrameArenaCurrent) / (1024.0 * 1024.0),
				static_cast<double>(memoryStats.FrameArenaCapacity) / (1024.0 * 1024.0),
				static_cast<double>(memoryStats.FrameArenaPeak) / (1024.0 * 1024.0));
			ImGui::Text(
				"Small Pool: %s <= %zu B, %.2f KB live (Peak %.2f KB)",
				memoryStats.SmallPoolRoutingEnabled ? "On" : "Off",
				memoryStats.SmallPoolMaxBlockSize,
				static_cast<double>(memoryStats.SmallPoolCurrentBytes) / 1024.0,
				static_cast<double>(memoryStats.SmallPoolPeakBytes) / 1024.0);
			ImGui::Text("Live tracked allocations: %zu", memoryStats.LiveAllocationCount);
			if (ImGui::BeginTable("MemoryStatsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Tag");
				ImGui::TableSetupColumn("Current");
				ImGui::TableSetupColumn("Peak");
				ImGui::TableSetupColumn("Allocs");
				ImGui::TableSetupColumn("Frees");
				ImGui::TableHeadersRow();
				for (size_t tagIndex = 0; tagIndex < Memory::kMemoryTagCount; ++tagIndex)
				{
					const auto tag = static_cast<Memory::MemoryTag>(tagIndex);
					const Memory::MemoryStats& stats = memoryStats.Tags[tagIndex];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(Memory::ToString(tag).data());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.2f MB", static_cast<double>(stats.CurrentBytes) / (1024.0 * 1024.0));
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%.2f MB", static_cast<double>(stats.PeakBytes) / (1024.0 * 1024.0));
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%zu", stats.AllocationCount);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%zu", stats.FreeCount);
				}
				ImGui::EndTable();
			}
			if (memoryStats.BenchmarkRowCount > 0 && ImGui::TreeNode("Startup Allocator Benchmark"))
			{
				if (ImGui::BeginTable("MemoryBenchmarkTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
				{
					ImGui::TableSetupColumn("Case");
					ImGui::TableSetupColumn("ns/op");
					ImGui::TableSetupColumn("Speedup");
					ImGui::TableSetupColumn("Iterations");
					ImGui::TableHeadersRow();
					for (size_t rowIndex = 0; rowIndex < memoryStats.BenchmarkRowCount; ++rowIndex)
					{
						const Memory::MemoryBenchmarkRow& row = memoryStats.BenchmarkRows[rowIndex];
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextUnformatted(row.Name.data());
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%.2f", row.NanosecondsPerOperation);
						ImGui::TableSetColumnIndex(2);
						ImGui::Text("%.2fx", row.SpeedupVsBaseline);
						ImGui::TableSetColumnIndex(3);
						ImGui::Text("%zu", row.Iterations);
					}
					ImGui::EndTable();
				}
				ImGui::TreePop();
			}
		}
		if (ImGui::CollapsingHeader("Jobs"))
		{
			const auto& jobStats = context.JobStats;
			ImGui::Text("Workers: %u", jobStats.WorkerCount);
			ImGui::Text("Frame Index: %llu", static_cast<unsigned long long>(jobStats.FrameIndex));
			ImGui::Text(
				"ParallelFor: %s | Sequential <= %zu items | Target jobs/worker %zu",
				jobStats.AdaptiveParallelForEnabled ? "Adaptive" : "Fixed",
				jobStats.ParallelForSequentialThreshold,
				jobStats.TargetJobsPerWorker);
			if (jobStats.SelectedBenchmarkChunkSize > 0)
			{
				ImGui::Text("Selected benchmark chunk: %zu", jobStats.SelectedBenchmarkChunkSize);
			}
			if (jobStats.BenchmarkRowCount > 0 && ImGui::BeginTable("JobBenchmarkTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Case");
				ImGui::TableSetupColumn("Items");
				ImGui::TableSetupColumn("Chunk");
				ImGui::TableSetupColumn("ms");
				ImGui::TableSetupColumn("Speedup");
				ImGui::TableHeadersRow();
				for (size_t rowIndex = 0; rowIndex < jobStats.BenchmarkRowCount; ++rowIndex)
				{
					const Jobs::JobBenchmarkRow& row = jobStats.BenchmarkRows[rowIndex];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(row.Name);
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%zu", row.WorkItemCount);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%zu", row.ChunkSize);
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.4f", row.Milliseconds);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%.2fx", row.SpeedupVsSequential);
				}
				ImGui::EndTable();
			}
		}
		if (ImGui::CollapsingHeader("Scripting"))
		{
			const auto& scriptStats = context.ScriptStats;
			ImGui::Text("Native scripts registered: %zu", scriptStats.RegisteredNativeScriptCount);
			ImGui::Text("Active script components: %zu", scriptStats.ActiveScriptCount);
			ImGui::Text("Started runtime instances: %zu", scriptStats.StartedScriptCount);
			ImGui::Text("Scheduled script jobs this frame: %zu", scriptStats.ScheduledJobCount);
			if (scriptStats.MissingNativeScriptCount > 0)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "Missing native classes: %zu", scriptStats.MissingNativeScriptCount);
			}
			ImGui::TextDisabled("Native v1 runs Project Scene scripts in Play mode unless Run In Editor is enabled.");
		}
		if (ImGui::CollapsingHeader("Resources"))
		{
			const Resources::ResourceManagerStats& resourceStats = context.ResourceStats;
			ImGui::Text("Groups: %zu", resourceStats.GroupCount);
			ImGui::Text("Resources: %zu", resourceStats.ResourceCount);
			ImGui::Text("Declared: %zu", resourceStats.DeclaredCount);
			ImGui::Text("Prepared: %zu", resourceStats.PreparedCount);
			ImGui::Text("Loaded: %zu", resourceStats.LoadedCount);
			ImGui::Text("Failed: %zu", resourceStats.FailedCount);
			ImGui::Text("Indexed Size: %.2f MB", static_cast<double>(resourceStats.DeclaredBytes) / (1024.0 * 1024.0));
			ImGui::Text("Loaded Size: %.2f MB", static_cast<double>(resourceStats.LoadedBytes) / (1024.0 * 1024.0));
		}
		if (ImGui::CollapsingHeader("Materials / Shader Variants"))
		{
			const Materials::MaterialResourceStats& materialStats = context.MaterialStats;
			ImGui::Text("Materials: %zu", materialStats.MaterialCount);
			ImGui::Text(
				"Models: Phong %zu | PBR %zu | Unlit %zu",
				materialStats.PhongCount,
				materialStats.PbrCount,
				materialStats.UnlitCount);
			ImGui::Text(
				"Texture Slots: %zu | Overrides %zu | Embedded %zu",
				materialStats.TextureSlotCount,
				materialStats.OverrideSlotCount,
				materialStats.EmbeddedSlotCount);

			const Materials::ShaderVariantCacheStats& variantStats = context.ShaderVariantStats;
			ImGui::SeparatorText("Shader Variant Cache");
			ImGui::Text("Variants: %zu", variantStats.VariantCount);
			ImGui::Text("Requests: %llu", static_cast<unsigned long long>(variantStats.RequestCount));
			ImGui::Text(
				"Models: Phong %zu | PBR %zu | Unlit %zu",
				variantStats.PhongVariantCount,
				variantStats.PbrVariantCount,
				variantStats.UnlitVariantCount);
			ImGui::Text("Deferred variants: %zu", variantStats.DeferredVariantCount);
			ImGui::Text("Transparent variants: %zu", variantStats.TransparentVariantCount);
		}
		if (ImGui::CollapsingHeader("Lighting"))
		{
			ImGui::Text("Scene enabled lights: %u%s", context.SceneLightCount, context.UsesFallbackLight ? " (fallback directional active)" : "");
			ImGui::Text("Forward realtime lights: %u / %u used", context.ForwardLightUsedCount, context.ForwardLightLimit);
			if (context.ForwardLightTruncatedCount > 0)
			{
				ImGui::TextColored(
					ImVec4(1.0f, 0.72f, 0.22f, 1.0f),
					"Forward light cap: %u light(s) ignored in Forward/Forward+.",
					context.ForwardLightTruncatedCount);
			}
			ImGui::Text("Forward+ realtime lights: max %u (shared Forward path)", context.ForwardLightLimit);
			ImGui::Text("Deferred realtime lights: unlimited (%u active)", context.DeferredLightCount);
			ImGui::Text("Deferred light buffer: %u slots, grows as needed", context.DeferredLightBufferCapacity);
			ImGui::Text(
				"Deferred tile passes: %u viewport(s), %u total tiles",
				context.DeferredTileViewportCount,
				context.DeferredTileCountTotal);
			ImGui::Text("Deferred tile references: %u", context.DeferredTileLightReferenceCount);
			const double averageTileLightCount = context.DeferredTileCountTotal > 0
				? static_cast<double>(context.DeferredTileLightReferenceCount) / static_cast<double>(context.DeferredTileCountTotal)
				: 0.0;
			ImGui::Text("Deferred tile lights: avg %.2f, max %u", averageTileLightCount, context.DeferredMaxTileLightCount);
			ImGui::Text("Deferred full-tile lights: %u", context.DeferredFullTileLightCount);
			ImGui::TextUnformatted("Deferred mode has no fixed scene-side light cap in v1.");
		}
		if (ImGui::CollapsingHeader("Render Graph"))
		{
			const Rendering::RenderGraphStats& graphStats = context.RenderGraphStats;
			ImGui::Text("Frame: %llu", static_cast<unsigned long long>(graphStats.FrameIndex));
			ImGui::Text("Passes: %zu / %zu enabled", graphStats.EnabledPassCount, graphStats.PassCount);
			ImGui::Text("World: %zu", graphStats.WorldPassCount);
			ImGui::Text("Shadow: %zu", graphStats.ShadowPassCount);
			ImGui::Text("Geometry: %zu", graphStats.GeometryPassCount);
			ImGui::Text("Lighting: %zu", graphStats.LightingPassCount);
			ImGui::Text("Post Process: %zu", graphStats.PostProcessPassCount);
			ImGui::Text("Editor: %zu", graphStats.EditorPassCount);
			ImGui::Text("Deferred: %s", graphStats.UsesDeferred ? "Yes" : "No");
			ImGui::Text("HDR path: %s", graphStats.UsesHdr ? "Yes" : "No");
			ImGui::SeparatorText("CPU Timing");
			ImGui::Text("Timed Passes: %zu", graphStats.TimedPassCount);
			ImGui::Text("Exclusive Sum: %.3f ms", graphStats.TotalCpuMs);
			ImGui::Text("Setup: %.3f  Clear: %.3f  Shadow: %.3f", graphStats.SetupCpuMs, graphStats.ClearCpuMs, graphStats.ShadowCpuMs);
			ImGui::Text("Geometry: %.3f  Lighting: %.3f  Post: %.3f", graphStats.GeometryCpuMs, graphStats.LightingCpuMs, graphStats.PostProcessCpuMs);
			ImGui::Text("Editor: %.3f  Present: %.3f  Debug: %.3f", graphStats.EditorCpuMs, graphStats.PresentCpuMs, graphStats.DebugCpuMs);
			if (context.RenderGraphPasses && ImGui::TreeNode("Timed Pass Details"))
			{
				for (const Rendering::RenderGraphPass& pass : *context.RenderGraphPasses)
				{
					if (!pass.Enabled || !pass.HasCpuTiming)
					{
						continue;
					}
					ImGui::Text("%7.3f ms  [%s]%s %s",
						pass.CpuMilliseconds,
						Rendering::ToString(pass.Kind),
						pass.IncludeCpuInStats ? "" : " inclusive",
						pass.Name.c_str());
				}
				ImGui::TreePop();
			}
		}
		if (ImGui::CollapsingHeader("Render Stats"))
		{
			const Rendering::RenderFrameStats& renderStats = context.RenderFrameStats;
			ImGui::Text("Frame: %llu", static_cast<unsigned long long>(renderStats.FrameIndex));
			ImGui::Text("Render Entities: %u", renderStats.RenderEntityCount);
			ImGui::Text("Enabled Mesh Entities: %u", renderStats.EnabledMeshEntityCount);
			ImGui::Text("Transparent Entities: %u", renderStats.TransparentEntityCount);
			ImGui::Text("Frustum Culling: %s", context.ViewFrustumCullingEnabled ? "On" : "Off");
			ImGui::Text("Culling: %u requests, %u tests, %u visible results, %u culled results",
				renderStats.ViewCullingRequestCount,
				renderStats.ViewCullingTestCount,
				renderStats.ViewVisibleEntityCount,
				renderStats.ViewCulledEntityCount);
			ImGui::Text("Culling Cache: %u hits, %u misses",
				renderStats.ViewCullingCacheHitCount,
				renderStats.ViewCullingCacheMissCount);
			ImGui::Text("Visible List Entries: %u", renderStats.ViewVisibleListEntityCount);
			ImGui::Text("Scene View: %u requests, %u visible list, %u culled",
				renderStats.SceneViewCullingRequestCount,
				renderStats.SceneViewVisibleListEntityCount,
				renderStats.SceneViewCulledEntityCount);
			ImGui::Text("Game View: %u requests, %u visible list, %u culled",
				renderStats.GameViewCullingRequestCount,
				renderStats.GameViewVisibleListEntityCount,
				renderStats.GameViewCulledEntityCount);
			ImGui::SeparatorText("Draw Calls");
			ImGui::Text("Total: %u", renderStats.DrawCallCount);
			ImGui::Text("Indexed: %u  Fullscreen: %u  Instanced: %u",
				renderStats.IndexedDrawCallCount,
				renderStats.FullscreenDrawCallCount,
				renderStats.InstancedDrawCallCount);
			ImGui::Text("Opaque: %u  Transparent: %u", renderStats.OpaqueDrawCallCount, renderStats.TransparentDrawCallCount);
			ImGui::Text("Shadow: %u  Deferred Geometry: %u", renderStats.ShadowDrawCallCount, renderStats.DeferredGeometryDrawCallCount);
			ImGui::Text("Benchmark: %u", renderStats.BenchmarkDrawCallCount);
			ImGui::SeparatorText("Submitted Work");
			ImGui::Text("Indices: %llu", static_cast<unsigned long long>(renderStats.SubmittedIndexCount));
			ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(renderStats.SubmittedTriangleCount));
			ImGui::Text("Instances: %llu", static_cast<unsigned long long>(renderStats.SubmittedInstanceCount));
		}
		if (ImGui::CollapsingHeader("Post Process"))
		{
			const Rendering::PostProcessStats& postStats = context.PostProcessStats;
			ImGui::Text("Backend: %s", postStats.Backend.data());
			ImGui::Text("HDR Target: %s", postStats.UsesHdrTarget ? "Yes" : "No");
			ImGui::Text("Tone Mapping: %s", postStats.ToneMappingEnabled ? postStats.ToneMapper.data() : "Off");
			ImGui::Text("Tone Map Pass: %s", postStats.ToneMapPassScheduled ? "Scheduled" : "Inline/Skipped");
			ImGui::Text("Exposure: %.2f", postStats.Exposure);
		}
		if (ImGui::CollapsingHeader("Shadows"))
		{
			const Rendering::ShadowStats& shadowStats = context.ShadowStats;
			ImGui::Text("Enabled: %s", shadowStats.Enabled ? "Yes" : "No");
			ImGui::Text("Directional caster: %s", shadowStats.HasDirectionalCaster ? "Yes" : "No");
			ImGui::Text("Light Entity: %u", shadowStats.LightEntity);
			ImGui::Text("Map Size: %u", shadowStats.MapSize);
			ImGui::Text("Distance: %.1f", shadowStats.Distance);
			ImGui::Text("Ortho Size: %.1f", shadowStats.OrthographicSize);
			ImGui::Text("Bias: %.5f", shadowStats.Bias);
			ImGui::Text("Normal Bias: %.4f", shadowStats.NormalBias);
			ImGui::Text("Strength: %.2f", shadowStats.Strength);
		}
		if (context.AssetLogLines && !context.AssetLogLines->empty())
		{
			ImGui::Separator();
			const size_t logCount = context.AssetLogLines->size();
			const std::string_view logFilter = TextFilter(m_ConsoleLogFilter);
			const size_t firstLogIndex = logFilter.empty() && logCount > 8 ? logCount - 8 : 0;
			size_t shownLogCount = 0;
			for (size_t logIndex = firstLogIndex; logIndex < logCount; ++logIndex)
			{
				const std::string& logLine = (*context.AssetLogLines)[logIndex];
				if (!ContainsCaseInsensitive(logLine, logFilter))
				{
					continue;
				}

				ImGui::TextWrapped("%s", logLine.c_str());
				++shownLogCount;
			}
			if (!logFilter.empty())
			{
				ImGui::TextDisabled("%zu / %zu matching log lines", shownLogCount, logCount);
			}
		}
		ImGui::End();
	}

	void EditorLayer::DrawStatusBar(EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		constexpr float statusBarHeight = 26.0f;
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - statusBarHeight));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, statusBarHeight));
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.085f, 0.10f, 0.96f));
		const bool visible = ImGui::Begin("Status Bar", nullptr, flags);
		if (!visible)
		{
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(2);
			return;
		}

		const bool isPlaying = context.PlayState == EditorPlayState::Play || context.PlayState == EditorPlayState::EnteringPlay;
		const char* playStateText = isPlaying ? "Play" : context.PlayState == EditorPlayState::Paused ? "Paused" : "Edit";
		const std::string sceneName = context.CurrentScenePath.empty()
			? std::string("<untitled>")
			: context.CurrentScenePath.filename().string();
		const std::string dirtyMarker = context.IsSceneDirty ? "*" : "";
		const double triangleK = static_cast<double>(context.RenderFrameStats.SubmittedTriangleCount) / 1000.0;

		ImGui::Text("Project: %s", context.ProjectName.c_str());
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				context.OnRevealProject ? "Click to reveal project.\n%s" : "%s",
				context.ProjectRootPath.empty() ? "<no project root>" : context.ProjectRootPath.string().c_str());
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.OnRevealProject)
		{
			context.OnRevealProject();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Text("Scene: %s%s", sceneName.c_str(), dirtyMarker.c_str());
		if (ImGui::IsItemHovered())
		{
			const char* actionText = context.IsSceneDirty && context.CanEditProjectScene && context.OnSaveScene
				? "Click to save scene.\n"
				: "";
			ImGui::SetTooltip(
				"%s%s",
				actionText,
				context.CurrentScenePath.empty() ? "<no scene path>" : context.CurrentScenePath.string().c_str());
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
			context.IsSceneDirty &&
			context.CanEditProjectScene &&
			context.OnSaveScene)
		{
			context.OnSaveScene();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Text(
			"%s%s | %s | %s",
			playStateText,
			context.ActiveSceneIsRuntimeClone ? " Runtime Clone" : "",
			GraphicsApiName(context.CurrentApi),
			RenderModeToString(context.CurrentRenderMode).data());
		if (ImGui::IsItemHovered() && context.ActiveSceneIsRuntimeClone)
		{
			ImGui::SetTooltip("Play mode is rendering and inspecting a runtime Scene clone. Runtime edits are discarded on Stop.");
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Text(
			"Draw %u | %.1fk tris | Ent %u",
			context.RenderFrameStats.DrawCallCount,
			triangleK,
			context.RenderFrameStats.RenderEntityCount);
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		ImGui::Text("Assets: %s", context.ProjectRefreshInProgress ? "Scanning" : "Ready");
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Click to refresh the Project asset tree.");
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.OnProjectRefresh)
		{
			context.OnProjectRefresh();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		const char* autosaveText = !context.AutosaveEnabled
			? "Autosave Off"
			: context.IsSceneDirty ? "Autosave Armed" : "Autosave Idle";
		ImGui::Text("%s", autosaveText);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip(
				"%s%s",
				context.OnAutosaveEnabledChanged ? "Click to toggle autosave.\n" : "",
				context.AutosaveStatusMessage.empty() ? "No autosave status message." : context.AutosaveStatusMessage.c_str());
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.OnAutosaveEnabledChanged)
		{
			context.OnAutosaveEnabledChanged(!context.AutosaveEnabled);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();
		if (!m_SourceControlSummary.RootPath.empty() &&
			!context.ProjectRootPath.empty() &&
			SamePath(m_SourceControlSummary.RootPath, context.ProjectRootPath) &&
			m_SourceControlSummary.IsGitRepository)
		{
			ImGui::Text(
				"Git: %s%s",
				m_SourceControlSummary.Branch.c_str(),
				m_SourceControlSummary.IsClean ? "" : " dirty");
		}
		else
		{
			ImGui::TextDisabled("Git: not refreshed");
		}
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Click to refresh Git status. Open Console > Source Control for actions.");
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			RefreshSourceControlStatus(context);
		}

		const float rightEdge = ImGui::GetWindowContentRegionMax().x;
		const float cursorX = ImGui::GetCursorPosX();
		const float helpWidth = ImGui::CalcTextSize("F1 Shortcuts").x;
		if (rightEdge - cursorX > helpWidth + 28.0f)
		{
			ImGui::SameLine(rightEdge - helpWidth);
			ImGui::TextDisabled("F1 Shortcuts");
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Click or press F1 to open Keyboard Shortcuts.");
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				OpenShortcutReference();
			}
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
	}

	void EditorLayer::DrawProjectEntryRecursive(const Asset::AssetFileEntry& entry, EditorContext& context)
	{
		const std::string_view projectFilter = TextFilter(m_ProjectFilter);
		if (!ProjectEntryMatchesFilter(entry, projectFilter, m_ProjectQuickFilter, m_ProjectFavoritePaths))
		{
			return;
		}

		const std::filesystem::path entryPath = entry.Path;
		const bool directory = entry.Kind == Asset::AssetFileKind::Directory;
		const bool favorite = IsProjectFavorite(entryPath);
		std::string label = favorite ? "* " : "";
		label.append(AssetKindTag(entry.Kind));
		label.push_back(' ');
		label.append(entry.Name);
		label.append("##");
		label.append(entryPath.string());

		if (directory)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
			if (!projectFilter.empty() || m_ProjectQuickFilter != ProjectQuickFilter::All)
			{
				flags |= ImGuiTreeNodeFlags_DefaultOpen;
			}
			const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				m_SelectedAssetPath = entryPath;
			}
			if (ImGui::BeginPopupContextItem())
			{
				m_SelectedAssetPath = entryPath;
				if (ImGui::MenuItem(favorite ? "Remove From Favorites" : "Add To Favorites"))
				{
					ToggleProjectFavorite(entryPath, context);
				}
				if (ImGui::MenuItem("Open") && context.OnAssetOpen)
				{
					context.OnAssetOpen(entryPath);
				}
				if (ImGui::MenuItem("Reveal") && context.OnAssetReveal)
				{
					context.OnAssetReveal(entryPath);
				}
				if (ImGui::MenuItem("Scope This Folder"))
				{
					m_SelectedAssetPath = entryPath;
					m_ProjectFolderScopeEnabled = true;
					SaveProjectState(context);
				}
				if (m_ProjectFolderScopeEnabled && ImGui::MenuItem("Clear Folder Scope"))
				{
					m_ProjectFolderScopeEnabled = false;
					SaveProjectState(context);
				}
				ImGui::Separator();
				DrawProjectPathCopyMenuItems(entryPath, context.ProjectSnapshot ? context.ProjectSnapshot->RootPath : std::filesystem::path{});
				if (ImGui::BeginMenu("Create"))
				{
					const auto createAsset = [&](ProjectCreateAssetKind kind)
					{
						OpenProjectCreateAssetDialog(kind, entryPath);
					};
					if (ImGui::MenuItem("Folder"))
					{
						createAsset(ProjectCreateAssetKind::Folder);
					}
					if (ImGui::MenuItem("Scene"))
					{
						createAsset(ProjectCreateAssetKind::Scene);
					}
					if (ImGui::MenuItem("Material"))
					{
						createAsset(ProjectCreateAssetKind::Material);
					}
					if (ImGui::MenuItem("Skybox"))
					{
						createAsset(ProjectCreateAssetKind::Skybox);
					}
					if (ImGui::MenuItem("Script"))
					{
						createAsset(ProjectCreateAssetKind::Script);
					}
					if (ImGui::MenuItem("Prefab"))
					{
						createAsset(ProjectCreateAssetKind::Prefab);
					}
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
			{
				context.OnAssetOpen(entryPath);
			}
			if (open)
			{
				for (const auto& child : entry.Children)
				{
					DrawProjectEntryRecursive(child, context);
				}
				ImGui::TreePop();
			}
			return;
		}

		const bool selected = SamePath(m_SelectedAssetPath, entryPath);
		if (ImGui::Selectable(label.c_str(), selected))
		{
			m_SelectedAssetPath = entryPath;
			AddRecentProjectAssetPath(entryPath, context, true);
		}
		if (ImGui::BeginPopupContextItem())
		{
			m_SelectedAssetPath = entryPath;
			AddRecentProjectAssetPath(entryPath, context, true);
			if (ImGui::MenuItem(favorite ? "Remove From Favorites" : "Add To Favorites"))
			{
				ToggleProjectFavorite(entryPath, context);
			}
			if (ImGui::MenuItem("Open") && context.OnAssetOpen)
			{
				AddRecentProjectAssetPath(entryPath, context, true);
				context.OnAssetOpen(entryPath);
			}
			if (ImGui::MenuItem("Reveal") && context.OnAssetReveal)
			{
				AddRecentProjectAssetPath(entryPath, context, true);
				context.OnAssetReveal(entryPath);
			}
			ImGui::Separator();
			DrawProjectPathCopyMenuItems(entryPath, context.ProjectSnapshot ? context.ProjectSnapshot->RootPath : std::filesystem::path{});
			if (entry.Kind == Asset::AssetFileKind::Model)
			{
				if (ImGui::MenuItem("Load Model") && context.OnModelDrop)
				{
					AddRecentProjectAssetPath(entryPath, context, true);
					context.OnModelDrop(entryPath, AssetDropTarget::Game);
				}
				if (ImGui::MenuItem("Reimport") && context.OnAssetReimportRequested)
				{
					AddRecentProjectAssetPath(entryPath, context, true);
					context.OnAssetReimportRequested(entryPath);
				}
			}
			if (Asset::IsSkyboxAssetPath(entryPath) && ImGui::MenuItem("Apply Skybox", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetOpen)))
			{
				AddRecentProjectAssetPath(entryPath, context, true);
				context.OnAssetOpen(entryPath);
			}
			if (context.OnAssetImportSettingsRequested)
			{
				if (ImGui::MenuItem("Import Settings"))
				{
					AddRecentProjectAssetPath(entryPath, context, true);
					context.OnAssetImportSettingsRequested(entryPath);
				}
			}
			ImGui::EndPopup();
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
		{
			AddRecentProjectAssetPath(entryPath, context, true);
			context.OnAssetOpen(entryPath);
		}
		if ((entry.Kind == Asset::AssetFileKind::Model || entry.Kind == Asset::AssetFileKind::Image)
			&& ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			const std::string payloadPath = entryPath.string();
			ImGui::SetDragDropPayload(kAssetPathPayload, payloadPath.c_str(), payloadPath.size() + 1);
			ImGui::Text("%s %s", entry.Kind == Asset::AssetFileKind::Model ? "Load" : "Use", entry.Name.c_str());
			ImGui::EndDragDropSource();
		}
	}

	void EditorLayer::DrawSelectedAssetDetails(const Asset::AssetFileSnapshot& snapshot, EditorContext& context)
	{
		ImGui::Separator();
		if (m_SelectedAssetPath.empty())
		{
			ImGui::TextUnformatted("Selected Asset: <none>");
			return;
		}

		std::error_code errorCode;
		const bool directory = std::filesystem::is_directory(m_SelectedAssetPath, errorCode);
		const bool regularFile = std::filesystem::is_regular_file(m_SelectedAssetPath, errorCode);
		ImGui::Text("Selected Asset: %s", RelativeDisplayPath(m_SelectedAssetPath, snapshot.RootPath).c_str());
		ImGui::Text("Type: %s", directory ? "Directory" : ExtensionTag(m_SelectedAssetPath));
		ImGui::Text("Favorite: %s", IsProjectFavorite(m_SelectedAssetPath) ? "Yes" : "No");
		if (regularFile)
		{
			const uintmax_t fileSize = std::filesystem::file_size(m_SelectedAssetPath, errorCode);
			if (!errorCode)
			{
				ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(fileSize));
			}
		}
		if (directory && ImGui::CollapsingHeader("Directory Summary", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const std::vector<Asset::AssetFileEntry>* selectedChildren = nullptr;
			if (SamePath(m_SelectedAssetPath, snapshot.RootPath))
			{
				selectedChildren = &snapshot.Children;
			}
			else if (const Asset::AssetFileEntry* selectedEntry = FindAssetEntryByPath(snapshot.Children, m_SelectedAssetPath))
			{
				selectedChildren = &selectedEntry->Children;
			}

			if (!selectedChildren)
			{
				ImGui::TextDisabled("Directory is not present in the current project snapshot.");
			}
			else
			{
				const ProjectDirectorySummary summary = BuildProjectDirectorySummary(*selectedChildren);
				ImGui::Text(
					"Direct: %zu item(s), %zu folder(s), %zu file(s)",
					summary.DirectChildren,
					summary.DirectDirectories,
					summary.DirectFiles);
				ImGui::Text(
					"Recursive: %zu folder(s), %zu file(s), %.2f MB",
					summary.RecursiveDirectories,
					summary.RecursiveFiles,
					static_cast<double>(summary.TotalBytes) / (1024.0 * 1024.0));
				if (ImGui::BeginTable("ProjectDirectorySummaryTable", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
				{
					ImGui::TableSetupColumn("Kind");
					ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableHeadersRow();
					const auto addSummaryRow = [this, &context](const char* label, size_t count, ProjectQuickFilter filter)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::TextUnformatted(label);
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%zu", count);
						ImGui::TableSetColumnIndex(2);
						ImGui::PushID(label);
						ImGui::BeginDisabled(count == 0 || filter == ProjectQuickFilter::All);
						if (ImGui::SmallButton("Show"))
						{
							m_ProjectFolderScopeEnabled = true;
							m_ProjectQuickFilter = filter;
							m_ProjectFilter.fill('\0');
							SaveProjectState(context);
						}
						ImGui::EndDisabled();
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("Scope this folder and show only %s.", ProjectQuickFilterName(filter));
						}
						ImGui::PopID();
					};
					addSummaryRow("Folders", summary.RecursiveDirectories, ProjectQuickFilter::Folders);
					addSummaryRow("Models", summary.Models, ProjectQuickFilter::Models);
					addSummaryRow("Images", summary.Images, ProjectQuickFilter::Images);
					addSummaryRow("Scenes", summary.Scenes, ProjectQuickFilter::Scenes);
					addSummaryRow("Materials", summary.Materials, ProjectQuickFilter::Materials);
					addSummaryRow("Prefabs", summary.Prefabs, ProjectQuickFilter::Prefabs);
					addSummaryRow("Source", summary.Source, ProjectQuickFilter::Source);
					addSummaryRow("Text", summary.Text, ProjectQuickFilter::Text);
					addSummaryRow("Other", summary.Other, ProjectQuickFilter::All);
					ImGui::EndTable();
				}
			}
		}

		const std::filesystem::path navigationTargetDirectory = directory
			? m_SelectedAssetPath
			: (regularFile ? m_SelectedAssetPath.parent_path() : snapshot.RootPath);
		const bool canSelectContainingFolder =
			regularFile &&
			!navigationTargetDirectory.empty() &&
			std::filesystem::exists(navigationTargetDirectory, errorCode);
		const bool canScopeDirectory =
			(directory || regularFile) &&
			!navigationTargetDirectory.empty() &&
			std::filesystem::exists(navigationTargetDirectory, errorCode);
		ImGui::BeginDisabled(!canSelectContainingFolder);
		if (ImGui::Button("Select Folder"))
		{
			m_SelectedAssetPath = navigationTargetDirectory;
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Select this asset's containing folder.");
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!canScopeDirectory);
		if (ImGui::Button(directory ? "Scope Here" : "Scope Folder"))
		{
			m_SelectedAssetPath = navigationTargetDirectory;
			m_ProjectFolderScopeEnabled = true;
			SaveProjectState(context);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Use this folder as the Project browser root while Scope Folder is enabled.");
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!m_ProjectFolderScopeEnabled);
		if (ImGui::Button("Clear Scope"))
		{
			m_ProjectFolderScopeEnabled = false;
			SaveProjectState(context);
		}
		ImGui::EndDisabled();

		const Asset::AssetFileKind selectedAssetKind = directory
			? Asset::AssetFileKind::Directory
			: Asset::ClassifyAssetPath(m_SelectedAssetPath);
		const ProjectQuickFilter selectedAssetQuickFilter = ProjectQuickFilterForAsset(m_SelectedAssetPath, selectedAssetKind);
		const std::filesystem::path selectedStem = directory
			? m_SelectedAssetPath.filename()
			: m_SelectedAssetPath.stem();
		const std::string selectedNameSearch = selectedStem.empty()
			? m_SelectedAssetPath.filename().string()
			: selectedStem.string();
		if (!selectedNameSearch.empty())
		{
			if (ImGui::Button("Search Name"))
			{
				SetTextBuffer(m_ProjectFilter, selectedNameSearch);
				m_ProjectQuickFilter = ProjectQuickFilter::All;
				SaveProjectState(context);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Search Project assets by this asset name.");
			}
			ImGui::SameLine();
		}
		const std::string filterTypeLabel = std::format("Filter {}", ProjectQuickFilterName(selectedAssetQuickFilter));
		ImGui::BeginDisabled(selectedAssetQuickFilter == ProjectQuickFilter::All);
		if (ImGui::Button(filterTypeLabel.c_str()))
		{
			m_ProjectFilter.fill('\0');
			m_ProjectQuickFilter = selectedAssetQuickFilter;
			SaveProjectState(context);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show only assets matching this asset type.");
		}
		const std::string selectedExtension = ToLower(m_SelectedAssetPath.extension().string());
		if (!directory && !selectedExtension.empty())
		{
			ImGui::SameLine();
			const std::string filterExtensionLabel = std::format("Filter {}", selectedExtension);
			if (ImGui::Button(filterExtensionLabel.c_str()))
			{
				SetTextBuffer(m_ProjectFilter, selectedExtension);
				m_ProjectQuickFilter = ProjectQuickFilter::All;
				SaveProjectState(context);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Search Project assets by this file extension.");
			}
		}

		if (ImGui::Button("Open") && context.OnAssetOpen)
		{
			AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
			context.OnAssetOpen(m_SelectedAssetPath);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reveal") && context.OnAssetReveal)
		{
			AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
			context.OnAssetReveal(m_SelectedAssetPath);
		}
		ImGui::SameLine();
		if (ImGui::Button(IsProjectFavorite(m_SelectedAssetPath) ? "Unfavorite" : "Favorite"))
		{
			ToggleProjectFavorite(m_SelectedAssetPath, context);
		}
		if (regularFile && Asset::IsModelAssetPath(m_SelectedAssetPath))
		{
			ImGui::SameLine();
			if (ImGui::Button("Load Model") && context.OnModelDrop)
			{
				AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
				context.OnModelDrop(m_SelectedAssetPath, AssetDropTarget::Game);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reimport") && context.OnAssetReimportRequested)
			{
				AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
				context.OnAssetReimportRequested(m_SelectedAssetPath);
			}
		}
		if (regularFile && Asset::IsSkyboxAssetPath(m_SelectedAssetPath))
		{
			ImGui::SameLine();
			if (ImGui::Button("Apply Skybox") && context.OnAssetOpen)
			{
				AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
				context.OnAssetOpen(m_SelectedAssetPath);
			}
		}
		if (regularFile && context.OnAssetImportSettingsRequested)
		{
			ImGui::SameLine();
			if (ImGui::Button("Import Settings"))
			{
				AddRecentProjectAssetPath(m_SelectedAssetPath, context, true);
				context.OnAssetImportSettingsRequested(m_SelectedAssetPath);
			}
		}
		if (ImGui::Button("Copy Path"))
		{
			CopyTextToClipboard(m_SelectedAssetPath.string());
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy Relative"))
		{
			CopyTextToClipboard(RelativeDisplayPath(m_SelectedAssetPath, snapshot.RootPath));
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy Name"))
		{
			CopyTextToClipboard(m_SelectedAssetPath.filename().string());
		}

		if (regularFile)
		{
			DrawImportSettingsEditor(snapshot, context);
		}

		if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (directory)
			{
				ImGui::TextDisabled("Directory contents are shown in the browser.");
			}
			else if (regularFile && Asset::ClassifyAssetPath(m_SelectedAssetPath) == Asset::AssetFileKind::Image)
			{
				DrawImageAssetPreview(m_SelectedAssetPath);
			}
			else if (regularFile && Asset::ClassifyAssetPath(m_SelectedAssetPath) == Asset::AssetFileKind::Model)
			{
				ImGui::Text("Model asset");
				ImGui::TextDisabled("Use Load Model or drag this asset into Scene/Game.");
			}
			else if (regularFile && (Asset::ClassifyAssetPath(m_SelectedAssetPath) == Asset::AssetFileKind::Text
				|| Asset::ClassifyAssetPath(m_SelectedAssetPath) == Asset::AssetFileKind::Source))
			{
				std::ifstream file(m_SelectedAssetPath);
				std::string preview;
				std::string line;
				size_t lineCount = 0;
				while (lineCount < 12 && std::getline(file, line))
				{
					preview.append(line);
					preview.push_back('\n');
					++lineCount;
				}
				if (preview.empty())
				{
					ImGui::TextDisabled("No text preview available.");
				}
				else
				{
					ImGui::BeginChild("AssetTextPreview", ImVec2(-1.0f, 140.0f), true);
					ImGui::TextUnformatted(preview.c_str());
					ImGui::EndChild();
				}
			}
			else
			{
				ImGui::TextDisabled("No preview available for this asset type.");
			}
		}

		DrawSelectedAssetDependencyView(snapshot, context);
	}

	void EditorLayer::EnsureImportSettingsLoadedForAsset(const std::filesystem::path& assetPath)
	{
		if (m_ProjectImportSettingsLoaded && m_ProjectImportSettingsAssetPath == assetPath)
		{
			return;
		}

		m_ProjectImportSettingsLoaded = false;
		m_ProjectImportSettingsDirty = false;
		m_ProjectImportSettingsAssetPath = assetPath;
		m_ProjectImportSettings = {};
		m_ProjectImportSettings.SourcePath = assetPath;
		m_ProjectImportSettingsStatus.clear();
		m_ProjectImportSettingsError.clear();

		Asset::AssetImportSettingsResult result = Asset::AssetImportSettingsService::LoadOrDefault(assetPath);
		if (result.Success)
		{
			m_ProjectImportSettings = result.Settings;
			if (m_ProjectImportSettings.SourcePath.empty())
			{
				m_ProjectImportSettings.SourcePath = assetPath;
			}

			const std::filesystem::path settingsPath = Asset::AssetImportSettingsService::GetSettingsPathForAsset(assetPath);
			std::error_code errorCode;
			m_ProjectImportSettingsStatus = std::filesystem::exists(settingsPath, errorCode)
				? "Loaded import settings."
				: "Using default import settings. Save to create the .import.json file.";
		}
		else
		{
			m_ProjectImportSettings = {};
			m_ProjectImportSettings.SourcePath = assetPath;
			m_ProjectImportSettingsStatus = "Using defaults because the existing import settings could not be read.";
			m_ProjectImportSettingsError = result.ErrorMessage;
			m_ProjectImportSettingsDirty = true;
		}

		m_ProjectImportSettingsLoaded = true;
	}

	bool EditorLayer::SaveSelectedImportSettings(EditorContext& context, bool reimportAfterSave)
	{
		if (!m_ProjectImportSettingsLoaded || m_ProjectImportSettingsAssetPath.empty())
		{
			return false;
		}

		m_ProjectImportSettings.SourcePath = m_ProjectImportSettingsAssetPath;
		std::string errorMessage;
		if (!Asset::AssetImportSettingsService::Save(m_ProjectImportSettings, errorMessage))
		{
			m_ProjectImportSettingsError = errorMessage;
			m_ProjectImportSettingsStatus = "Import settings save failed.";
			return false;
		}

		m_ProjectImportSettingsDirty = false;
		m_ProjectImportSettingsError.clear();
		m_ProjectImportSettingsStatus = "Import settings saved.";
		if (context.OnProjectRefresh)
		{
			context.OnProjectRefresh();
		}

		if (reimportAfterSave)
		{
			if (Asset::IsModelAssetPath(m_ProjectImportSettingsAssetPath) && context.OnAssetReimportRequested)
			{
				context.OnAssetReimportRequested(m_ProjectImportSettingsAssetPath);
				m_ProjectImportSettingsStatus = "Import settings saved. Reimport requested.";
			}
			else
			{
				m_ProjectImportSettingsStatus = "Import settings saved. Reimport is only available for model assets.";
			}
		}

		return true;
	}

	void EditorLayer::DrawImportSettingsEditor(const Asset::AssetFileSnapshot& snapshot, EditorContext& context)
	{
		const Asset::AssetFileKind assetKind = Asset::ClassifyAssetPath(m_SelectedAssetPath);
		const bool isModel = assetKind == Asset::AssetFileKind::Model;
		const bool isImage = assetKind == Asset::AssetFileKind::Image;
		if (!isModel && !isImage)
		{
			return;
		}

		EnsureImportSettingsLoadedForAsset(m_SelectedAssetPath);

		if (!ImGui::CollapsingHeader("Import Settings", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		ImGui::PushID("SelectedAssetImportSettings");
		const std::filesystem::path settingsPath = Asset::AssetImportSettingsService::GetSettingsPathForAsset(m_SelectedAssetPath);
		ImGui::Text("Settings File: %s", RelativeDisplayPath(settingsPath, snapshot.RootPath).c_str());
		if (!m_ProjectImportSettingsStatus.empty())
		{
			ImGui::TextDisabled("%s", m_ProjectImportSettingsStatus.c_str());
		}
		if (!m_ProjectImportSettingsError.empty())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_ProjectImportSettingsError.c_str());
		}
		if (m_ProjectImportSettingsDirty)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.22f, 1.0f), "Unsaved import setting changes.");
		}

		auto markDirty = [this]()
			{
				m_ProjectImportSettingsDirty = true;
				m_ProjectImportSettingsStatus = "Unsaved import setting changes.";
			};

		if (isModel)
		{
			if (ImGui::TreeNodeEx("Model", ImGuiTreeNodeFlags_DefaultOpen))
			{
				float scale = m_ProjectImportSettings.Model.Scale;
				if (ImGui::DragFloat("Scale", &scale, 0.01f, 0.001f, 1000.0f, "%.3f"))
				{
					m_ProjectImportSettings.Model.Scale = (std::max)(0.001f, scale);
					markDirty();
				}

				float rotationOffset[3] =
				{
					m_ProjectImportSettings.Model.RotationOffset.x,
					m_ProjectImportSettings.Model.RotationOffset.y,
					m_ProjectImportSettings.Model.RotationOffset.z
				};
				if (ImGui::DragFloat3("Rotation Offset", rotationOffset, 0.1f, -360.0f, 360.0f, "%.2f"))
				{
					m_ProjectImportSettings.Model.RotationOffset = { rotationOffset[0], rotationOffset[1], rotationOffset[2] };
					markDirty();
				}

				if (ImGui::Checkbox("Import Materials", &m_ProjectImportSettings.Model.ImportMaterials))
				{
					markDirty();
				}
				if (ImGui::Checkbox("Import Animations", &m_ProjectImportSettings.Model.ImportAnimations))
				{
					markDirty();
				}
				if (ImGui::Checkbox("Generate Tangents", &m_ProjectImportSettings.Model.GenerateTangents))
				{
					markDirty();
				}
				if (ImGui::Checkbox("Generate Colliders", &m_ProjectImportSettings.Model.GenerateColliders))
				{
					markDirty();
				}
				if (ImGui::Checkbox("Normal Y Flip", &m_ProjectImportSettings.Model.NormalYFlip))
				{
					markDirty();
				}
				ImGui::TreePop();
			}
		}

		if (ImGui::TreeNodeEx(isModel ? "Texture Defaults" : "Texture", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::Checkbox("sRGB", &m_ProjectImportSettings.Texture.Srgb))
			{
				markDirty();
			}
			if (ImGui::Checkbox("Generate Mips", &m_ProjectImportSettings.Texture.GenerateMips))
			{
				markDirty();
			}
			if (ImGui::Checkbox("Normal Map", &m_ProjectImportSettings.Texture.NormalMap))
			{
				markDirty();
			}
			if (ImGui::Checkbox("Clamp To Edge", &m_ProjectImportSettings.Texture.ClampToEdge))
			{
				markDirty();
			}
			ImGui::TreePop();
		}

		if (ImGui::Button("Save Import Settings"))
		{
			static_cast<void>(SaveSelectedImportSettings(context, false));
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!isModel || !context.OnAssetReimportRequested);
		if (ImGui::Button("Save & Reimport"))
		{
			static_cast<void>(SaveSelectedImportSettings(context, true));
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Reset Defaults"))
		{
			m_ProjectImportSettings = {};
			m_ProjectImportSettings.SourcePath = m_SelectedAssetPath;
			m_ProjectImportSettingsDirty = true;
			m_ProjectImportSettingsError.clear();
			m_ProjectImportSettingsStatus = "Default import settings staged. Save to write them.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Reload"))
		{
			m_ProjectImportSettingsLoaded = false;
			EnsureImportSettingsLoadedForAsset(m_SelectedAssetPath);
		}

		ImGui::PopID();
	}

	void EditorLayer::DrawImageAssetPreview(const std::filesystem::path& imagePath)
	{
		EnsureProjectImagePreview(imagePath);

		if (!m_ProjectPreviewError.empty())
		{
			ImGui::TextWrapped("%s", m_ProjectPreviewError.c_str());
			return;
		}
		if (m_ProjectPreviewPixels.empty() || m_ProjectPreviewWidth <= 0 || m_ProjectPreviewHeight <= 0)
		{
			ImGui::TextDisabled("No image preview available.");
			return;
		}

		ImGui::Text(
			"Image: %dx%d, %d channel(s)",
			m_ProjectPreviewSourceWidth,
			m_ProjectPreviewSourceHeight,
			m_ProjectPreviewSourceChannels);
		ImGui::TextDisabled(
			"Preview: %dx%d CPU thumbnail",
			m_ProjectPreviewWidth,
			m_ProjectPreviewHeight);

		const float maxPreviewWidth = (std::min)(ImGui::GetContentRegionAvail().x, 260.0f);
		const float aspect = static_cast<float>(m_ProjectPreviewHeight) / static_cast<float>(m_ProjectPreviewWidth);
		const ImVec2 previewSize(maxPreviewWidth, (std::max)(48.0f, maxPreviewWidth * aspect));
		ImGui::InvisibleButton("##ProjectImageCpuPreview", previewSize);

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float cellWidth = previewSize.x / static_cast<float>(m_ProjectPreviewWidth);
		const float cellHeight = previewSize.y / static_cast<float>(m_ProjectPreviewHeight);
		for (int y = 0; y < m_ProjectPreviewHeight; ++y)
		{
			for (int x = 0; x < m_ProjectPreviewWidth; ++x)
			{
				const uint32_t color = m_ProjectPreviewPixels[static_cast<size_t>(y) * static_cast<size_t>(m_ProjectPreviewWidth) + static_cast<size_t>(x)];
				const ImVec2 pixelMin(min.x + static_cast<float>(x) * cellWidth, min.y + static_cast<float>(y) * cellHeight);
				const ImVec2 pixelMax(min.x + static_cast<float>(x + 1) * cellWidth + 0.5f, min.y + static_cast<float>(y + 1) * cellHeight + 0.5f);
				drawList->AddRectFilled(pixelMin, pixelMax, color);
			}
		}
		drawList->AddRect(min, max, IM_COL32(120, 120, 120, 255));
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", imagePath.string().c_str());
		}
	}

	void EditorLayer::EnsureProjectImagePreview(const std::filesystem::path& imagePath)
	{
		const std::filesystem::path normalizedPath = imagePath.lexically_normal();
		if (!m_ProjectPreviewImagePath.empty() && SamePath(m_ProjectPreviewImagePath, normalizedPath))
		{
			return;
		}

		m_ProjectPreviewImagePath = normalizedPath;
		m_ProjectPreviewPixels.clear();
		m_ProjectPreviewWidth = 0;
		m_ProjectPreviewHeight = 0;
		m_ProjectPreviewSourceWidth = 0;
		m_ProjectPreviewSourceHeight = 0;
		m_ProjectPreviewSourceChannels = 0;
		m_ProjectPreviewError.clear();

		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load(normalizedPath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels || width <= 0 || height <= 0)
		{
			m_ProjectPreviewError = std::format("Image preview load failed: {}", normalizedPath.string());
			if (pixels)
			{
				stbi_image_free(pixels);
			}
			return;
		}

		m_ProjectPreviewSourceWidth = width;
		m_ProjectPreviewSourceHeight = height;
		m_ProjectPreviewSourceChannels = channels;

		constexpr int maxThumbnailDimension = 64;
		if (width >= height)
		{
			m_ProjectPreviewWidth = (std::min)(maxThumbnailDimension, width);
			m_ProjectPreviewHeight = (std::max)(1, static_cast<int>(std::round(static_cast<float>(m_ProjectPreviewWidth) * static_cast<float>(height) / static_cast<float>(width))));
		}
		else
		{
			m_ProjectPreviewHeight = (std::min)(maxThumbnailDimension, height);
			m_ProjectPreviewWidth = (std::max)(1, static_cast<int>(std::round(static_cast<float>(m_ProjectPreviewHeight) * static_cast<float>(width) / static_cast<float>(height))));
		}

		m_ProjectPreviewPixels.resize(static_cast<size_t>(m_ProjectPreviewWidth) * static_cast<size_t>(m_ProjectPreviewHeight));
		for (int y = 0; y < m_ProjectPreviewHeight; ++y)
		{
			for (int x = 0; x < m_ProjectPreviewWidth; ++x)
			{
				const int sourceX = std::clamp(
					static_cast<int>((static_cast<float>(x) + 0.5f) * static_cast<float>(width) / static_cast<float>(m_ProjectPreviewWidth)),
					0,
					width - 1);
				const int sourceY = std::clamp(
					static_cast<int>((static_cast<float>(y) + 0.5f) * static_cast<float>(height) / static_cast<float>(m_ProjectPreviewHeight)),
					0,
					height - 1);
				const size_t sourceOffset = (static_cast<size_t>(sourceY) * static_cast<size_t>(width) + static_cast<size_t>(sourceX)) * 4;
				const uint8_t alpha = pixels[sourceOffset + 3];
				const uint8_t checker = ((x / 4 + y / 4) % 2) == 0 ? 192 : 224;
				const float alphaFloat = static_cast<float>(alpha) / 255.0f;
				const auto blend = [alphaFloat, checker](uint8_t channel) -> uint8_t
				{
					return static_cast<uint8_t>(static_cast<float>(channel) * alphaFloat + static_cast<float>(checker) * (1.0f - alphaFloat) + 0.5f);
				};
				m_ProjectPreviewPixels[static_cast<size_t>(y) * static_cast<size_t>(m_ProjectPreviewWidth) + static_cast<size_t>(x)] = IM_COL32(
					blend(pixels[sourceOffset + 0]),
					blend(pixels[sourceOffset + 1]),
					blend(pixels[sourceOffset + 2]),
					255);
			}
		}

		stbi_image_free(pixels);
	}

	void EditorLayer::DrawSelectedAssetDependencyView(const Asset::AssetFileSnapshot& snapshot, EditorContext& context)
	{
		if (!ImGui::CollapsingHeader("Dependencies / References", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (ImGui::Button("Refresh Graph"))
		{
			m_ProjectAnalysisSelectionPath.clear();
			m_ProjectReferenceIndex = {};
			m_ForceRebuildProjectReferenceIndex = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Text assets up to 4 MB are indexed per project snapshot.");

		RefreshSelectedAssetAnalysis(snapshot, context);

		ImGui::Text("Indexed: %zu file(s)", m_ProjectAnalysisScannedFiles);
		if (m_ProjectAnalysisSkippedFiles > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Skipped: %zu", m_ProjectAnalysisSkippedFiles);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Cache: %s", m_ProjectReferenceIndex.LoadedFromDisk ? "Library/EditorAssetIndex.json" : "memory");
		if (!m_ProjectModelInspectionLines.empty() && ImGui::TreeNodeEx("Model Import Inspection", ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (const std::string& line : m_ProjectModelInspectionLines)
			{
				ImGui::TextWrapped("%s", line.c_str());
			}
			ImGui::TreePop();
		}

		const auto drawPathList = [this, &snapshot, &context](const char* emptyText, const std::vector<std::filesystem::path>& paths)
		{
			if (paths.empty())
			{
				ImGui::TextDisabled("%s", emptyText);
				return;
			}

			for (size_t pathIndex = 0; pathIndex < paths.size(); ++pathIndex)
			{
				const std::filesystem::path& path = paths[pathIndex];
				const std::string label = std::format("{}##asset_graph_{}", RelativeDisplayPath(path, snapshot.RootPath), pathIndex);
				const bool selected = SamePath(m_SelectedAssetPath, path);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
				{
					m_SelectedAssetPath = path;
					AddRecentProjectAssetPath(path, context, true);
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
					{
						context.OnAssetOpen(path);
					}
				}
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("Click to select, double-click to open.");
				}
				if (ImGui::BeginPopupContextItem())
				{
					m_SelectedAssetPath = path;
					AddRecentProjectAssetPath(path, context, true);
					if (ImGui::MenuItem("Open") && context.OnAssetOpen)
					{
						context.OnAssetOpen(path);
					}
					if (ImGui::MenuItem("Reveal") && context.OnAssetReveal)
					{
						context.OnAssetReveal(path);
					}
					if (ImGui::MenuItem(IsProjectFavorite(path) ? "Remove From Favorites" : "Add To Favorites"))
					{
						ToggleProjectFavorite(path, context);
					}
					if (Asset::IsModelAssetPath(path))
					{
						if (ImGui::MenuItem("Load Model", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnModelDrop)))
						{
							context.OnModelDrop(path, AssetDropTarget::Game);
						}
						if (ImGui::MenuItem("Reimport", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetReimportRequested)))
						{
							context.OnAssetReimportRequested(path);
						}
					}
					if (Asset::IsSkyboxAssetPath(path) && ImGui::MenuItem("Apply Skybox", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetOpen)))
					{
						context.OnAssetOpen(path);
					}
					ImGui::Separator();
					DrawProjectPathCopyMenuItems(path, snapshot.RootPath);
					ImGui::EndPopup();
				}
			}
		};

		if (ImGui::TreeNodeEx("Dependencies", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("Assets referenced by the selected file.");
			drawPathList("No project asset dependencies found.", m_ProjectDependencyPaths);
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("References", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::TextDisabled("Project files referencing the selected asset.");
			drawPathList("No project references found.", m_ProjectReferencePaths);
			ImGui::TreePop();
		}
	}

	void EditorLayer::EnsureProjectReferenceIndex(const Asset::AssetFileSnapshot& snapshot, const EditorContext& context)
	{
		const std::filesystem::path rootPath = snapshot.RootPath.lexically_normal();
		const std::filesystem::path projectRootPath = context.ProjectRootPath.empty()
			? rootPath.parent_path().lexically_normal()
			: context.ProjectRootPath.lexically_normal();
		const uint64_t signature = Asset::AssetDatabase::ComputeReferenceIndexSignature(snapshot);
		if (!m_ProjectReferenceIndex.AssetRootPath.empty()
			&& SamePath(m_ProjectReferenceIndex.AssetRootPath, rootPath)
			&& m_ProjectReferenceIndex.Signature == signature
			&& !m_ForceRebuildProjectReferenceIndex)
		{
			return;
		}

		m_ProjectReferenceIndex = Asset::AssetDatabase::LoadOrBuildReferenceIndex(
			snapshot,
			projectRootPath,
			m_ForceRebuildProjectReferenceIndex);
		m_ForceRebuildProjectReferenceIndex = false;
	}

	void EditorLayer::RefreshSelectedAssetAnalysis(const Asset::AssetFileSnapshot& snapshot, const EditorContext& context)
	{
		const std::filesystem::path selectedPath = m_SelectedAssetPath.lexically_normal();
		const std::filesystem::path rootPath = snapshot.RootPath.lexically_normal();
		const uint64_t currentIndexSignature = Asset::AssetDatabase::ComputeReferenceIndexSignature(snapshot);
		if (!m_ProjectAnalysisSelectionPath.empty()
			&& SamePath(m_ProjectAnalysisSelectionPath, selectedPath)
			&& SamePath(m_ProjectAnalysisRootPath, rootPath)
			&& m_ProjectReferenceIndex.Signature == currentIndexSignature
			&& !m_ForceRebuildProjectReferenceIndex)
		{
			return;
		}

		m_ProjectAnalysisSelectionPath = selectedPath;
		m_ProjectAnalysisRootPath = rootPath;
		m_ProjectDependencyPaths.clear();
		m_ProjectReferencePaths.clear();
		m_ProjectModelInspectionLines.clear();
		m_ProjectAnalysisScannedFiles = 0;
		m_ProjectAnalysisSkippedFiles = 0;

		if (selectedPath.empty())
		{
			return;
		}

		EnsureProjectReferenceIndex(snapshot, context);
		m_ProjectAnalysisScannedFiles = m_ProjectReferenceIndex.ScannedFiles;
		m_ProjectAnalysisSkippedFiles = m_ProjectReferenceIndex.SkippedFiles;

		const auto addDependencyPath = [this](const std::filesystem::path& path)
		{
			if (path.empty())
			{
				return;
			}

			const auto duplicateIt = std::ranges::find_if(m_ProjectDependencyPaths, [&path](const std::filesystem::path& existingPath)
				{
					return SamePath(existingPath, path);
				});
			if (duplicateIt == m_ProjectDependencyPaths.end())
			{
				m_ProjectDependencyPaths.push_back(path.lexically_normal());
			}
		};

		for (const std::filesystem::path& dependencyPath : Asset::AssetDatabase::GetDependencies(m_ProjectReferenceIndex, selectedPath))
		{
			addDependencyPath(dependencyPath);
		}
		m_ProjectReferencePaths = Asset::AssetDatabase::GetReferences(m_ProjectReferenceIndex, selectedPath);

		if (Asset::ClassifyAssetPath(selectedPath) == Asset::AssetFileKind::Model)
		{
			Asset::AssimpModelLoader loader;
			const Asset::ModelInspectionSummary summary = loader.InspectModel(selectedPath.string());
			m_ProjectModelInspectionLines.push_back(std::format(
				"Scene={} Root={} Meshes={} Renderable={} Materials={} Animations={} Vertices={} Faces={} Indices={}",
				summary.HasScene ? "yes" : "no",
				summary.HasRootNode ? "yes" : "no",
				summary.MeshCount,
				summary.RenderableMeshCount,
				summary.MaterialCount,
				summary.AnimationCount,
				summary.VertexCount,
				summary.FaceCount,
				summary.IndexCount));
			if (!summary.AssimpError.empty())
			{
				m_ProjectModelInspectionLines.push_back(std::format("Assimp: {}", summary.AssimpError));
			}
			if (summary.Textures.empty())
			{
				m_ProjectModelInspectionLines.push_back("No material texture references discovered.");
			}
			else
			{
				for (const Asset::ModelTextureInspection& texture : summary.Textures)
				{
					std::string line = std::format(
						"Material {} '{}' {} <- {}",
						texture.MaterialIndex,
						texture.MaterialName.empty() ? "<unnamed>" : texture.MaterialName,
						Asset::MaterialTextureSlotName(texture.Slot),
						texture.SourceType);
					if (!texture.RawPath.empty())
					{
						line.append(std::format(" raw='{}'", texture.RawPath));
					}
					if (texture.Embedded)
					{
						line.append(" resolved=<embedded>");
					}
					else if (!texture.ResolvedPath.empty())
					{
						line.append(std::format(" resolved='{}'", RelativeDisplayPath(texture.ResolvedPath, rootPath)));
						addDependencyPath(texture.ResolvedPath);
					}
					else
					{
						line.append(" resolved=<unresolved>");
					}
					if (texture.AutoMatched)
					{
						line.append(" auto-match");
					}
					m_ProjectModelInspectionLines.push_back(std::move(line));
				}
			}
		}

		std::ranges::sort(m_ProjectDependencyPaths);
		std::ranges::sort(m_ProjectReferencePaths);
	}

	bool EditorLayer::IsProjectFavorite(const std::filesystem::path& path) const
	{
		return std::ranges::any_of(m_ProjectFavoritePaths, [&path](const std::filesystem::path& favoritePath)
			{
				return SamePath(favoritePath, path);
			});
	}

	void EditorLayer::ToggleProjectFavorite(const std::filesystem::path& path, const EditorContext& context)
	{
		const auto it = std::ranges::find_if(m_ProjectFavoritePaths, [&path](const std::filesystem::path& favoritePath)
			{
				return SamePath(favoritePath, path);
			});
		if (it != m_ProjectFavoritePaths.end())
		{
			m_ProjectFavoritePaths.erase(it);
			SaveProjectState(context);
			return;
		}
		if (!path.empty())
		{
			m_ProjectFavoritePaths.push_back(path.lexically_normal());
			SaveProjectState(context);
		}
	}

	void EditorLayer::AddRecentProjectAssetPath(const std::filesystem::path& assetPath, const EditorContext& context, bool saveState)
	{
		if (assetPath.empty())
		{
			return;
		}

		std::error_code errorCode;
		if (!std::filesystem::is_regular_file(assetPath, errorCode))
		{
			return;
		}

		const std::filesystem::path normalizedAssetPath = assetPath.lexically_normal();
		std::erase_if(m_ProjectRecentAssetPaths, [&normalizedAssetPath](const std::filesystem::path& existingPath)
			{
				return SamePath(existingPath, normalizedAssetPath);
			});
		m_ProjectRecentAssetPaths.insert(m_ProjectRecentAssetPaths.begin(), normalizedAssetPath);
		constexpr size_t maxRecentAssets = 16;
		if (m_ProjectRecentAssetPaths.size() > maxRecentAssets)
		{
			m_ProjectRecentAssetPaths.resize(maxRecentAssets);
		}

		if (saveState)
		{
			SaveProjectState(context);
		}
	}

	void EditorLayer::DrawRecentProjectAssets(const Asset::AssetFileSnapshot& snapshot, EditorContext& context, std::string_view projectFilter)
	{
		if (m_ProjectQuickFilter != ProjectQuickFilter::All)
		{
			return;
		}

		if (!ImGui::CollapsingHeader("Recent Assets", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		if (m_ProjectRecentAssetPaths.empty())
		{
			ImGui::TextDisabled("No recent assets yet.");
			return;
		}

		size_t visibleRecentCount = 0;
		bool stopRecentLoop = false;
		for (const std::filesystem::path& recentAssetPath : m_ProjectRecentAssetPaths)
		{
			const std::string relativeAssetPath = RelativeDisplayPath(recentAssetPath, snapshot.RootPath);
			if (!projectFilter.empty() &&
				!ContainsCaseInsensitive(relativeAssetPath + " " + recentAssetPath.filename().string(), projectFilter))
			{
				continue;
			}

			++visibleRecentCount;
			const bool recentSelected = SamePath(m_SelectedAssetPath, recentAssetPath);
			const std::string label = std::format("{} {}##recent_asset_{}", ExtensionTag(recentAssetPath), relativeAssetPath, recentAssetPath.string());
			if (ImGui::Selectable(label.c_str(), recentSelected))
			{
				m_SelectedAssetPath = recentAssetPath;
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
			{
				m_SelectedAssetPath = recentAssetPath;
				context.OnAssetOpen(recentAssetPath);
			}
			if (ImGui::BeginPopupContextItem())
			{
				m_SelectedAssetPath = recentAssetPath;
				if (ImGui::MenuItem("Open") && context.OnAssetOpen)
				{
					context.OnAssetOpen(recentAssetPath);
				}
				if (ImGui::MenuItem("Reveal") && context.OnAssetReveal)
				{
					context.OnAssetReveal(recentAssetPath);
				}
				if (ImGui::MenuItem(IsProjectFavorite(recentAssetPath) ? "Remove From Favorites" : "Add To Favorites"))
				{
					ToggleProjectFavorite(recentAssetPath, context);
				}
				if (Asset::IsModelAssetPath(recentAssetPath) && ImGui::MenuItem("Load Model", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnModelDrop)))
				{
					context.OnModelDrop(recentAssetPath, AssetDropTarget::Game);
				}
				if (Asset::IsSkyboxAssetPath(recentAssetPath) && ImGui::MenuItem("Apply Skybox", nullptr, false, context.CanEditProjectScene && static_cast<bool>(context.OnAssetOpen)))
				{
					context.OnAssetOpen(recentAssetPath);
				}
				ImGui::Separator();
				DrawProjectPathCopyMenuItems(recentAssetPath, snapshot.RootPath);
				if (ImGui::MenuItem("Remove From Recent"))
				{
					std::erase_if(m_ProjectRecentAssetPaths, [&recentAssetPath](const std::filesystem::path& existingPath)
						{
							return SamePath(existingPath, recentAssetPath);
						});
					SaveProjectState(context);
					stopRecentLoop = true;
				}
				ImGui::EndPopup();
			}
			if (stopRecentLoop)
			{
				break;
			}
		}

		if (visibleRecentCount == 0)
		{
			ImGui::TextDisabled("No recent assets match the current search.");
		}
		ImGui::BeginDisabled(m_ProjectRecentAssetPaths.empty());
		if (ImGui::SmallButton("Clear Recent Assets"))
		{
			m_ProjectRecentAssetPaths.clear();
			SaveProjectState(context);
		}
		ImGui::EndDisabled();
	}

	void EditorLayer::TrackCurrentSceneInRecentScenes(const EditorContext& context)
	{
		if (!context.CanEditProjectScene || context.ProjectRootPath.empty() || context.CurrentScenePath.empty())
		{
			return;
		}

		const std::filesystem::path normalizedScenePath = context.CurrentScenePath.lexically_normal();
		if (!m_LastTrackedCurrentScenePath.empty() && SamePath(m_LastTrackedCurrentScenePath, normalizedScenePath))
		{
			return;
		}

		m_LastTrackedCurrentScenePath = normalizedScenePath;
		AddRecentScenePath(normalizedScenePath, context, true);
	}

	void EditorLayer::AddRecentScenePath(const std::filesystem::path& scenePath, const EditorContext& context, bool saveState)
	{
		if (scenePath.empty() || ToLower(scenePath.extension().string()) != ".scene")
		{
			return;
		}

		const std::filesystem::path normalizedScenePath = scenePath.lexically_normal();
		std::erase_if(m_RecentScenePaths, [&normalizedScenePath](const std::filesystem::path& existingPath)
			{
				return SamePath(existingPath, normalizedScenePath);
			});
		m_RecentScenePaths.insert(m_RecentScenePaths.begin(), normalizedScenePath);
		constexpr size_t maxRecentScenes = 10;
		if (m_RecentScenePaths.size() > maxRecentScenes)
		{
			m_RecentScenePaths.resize(maxRecentScenes);
		}

		if (saveState)
		{
			SaveProjectState(context);
		}
	}

	void EditorLayer::DrawRecentScenesMenu(EditorContext& context)
	{
		if (!ImGui::BeginMenu("Open Recent"))
		{
			return;
		}

		if (m_RecentScenePaths.empty())
		{
			ImGui::MenuItem("No recent scenes", nullptr, false, false);
		}
		else
		{
			for (size_t sceneIndex = 0; sceneIndex < m_RecentScenePaths.size(); ++sceneIndex)
			{
				const std::filesystem::path& scenePath = m_RecentScenePaths[sceneIndex];
				std::error_code errorCode;
				const bool exists = std::filesystem::is_regular_file(scenePath, errorCode);
				const std::string storedPath = ToStoredProjectPath(scenePath, context.ProjectRootPath);
				const std::string label = std::format(
					"{}. {}{}",
					sceneIndex + 1,
					scenePath.filename().string(),
					exists ? "" : " (missing)");
				if (ImGui::MenuItem(label.c_str(), storedPath.c_str(), false, exists && static_cast<bool>(context.OnOpenScene)))
				{
					context.OnOpenScene(scenePath);
				}
			}
		}

		ImGui::Separator();
		ImGui::BeginDisabled(m_RecentScenePaths.empty());
		if (ImGui::MenuItem("Clear Recent Scenes"))
		{
			m_RecentScenePaths.clear();
			m_LastTrackedCurrentScenePath.clear();
			SaveProjectState(context);
		}
		ImGui::EndDisabled();
		ImGui::EndMenu();
	}

	void EditorLayer::EnsureProjectStateLoaded(const EditorContext& context)
	{
		if (context.ProjectRootPath.empty())
		{
			return;
		}

		const std::filesystem::path normalizedRoot = context.ProjectRootPath.lexically_normal();
		if (m_ProjectStateRootPath.empty() || !SamePath(m_ProjectStateRootPath, normalizedRoot))
		{
			LoadProjectState(normalizedRoot);
		}
	}

	void EditorLayer::LoadProjectState(const std::filesystem::path& projectRootPath)
	{
		m_ProjectStateRootPath = projectRootPath.lexically_normal();
		m_ProjectFavoritePaths.clear();
		m_ProjectRecentAssetPaths.clear();
		m_RecentScenePaths.clear();
		m_ProjectSavedSearches.clear();
		m_CommandPalettePinnedCommands.clear();
		m_CommandPaletteRecentCommands.clear();
		m_CommandPaletteScope = CommandPaletteScope::All;
		m_ProjectEditorLayoutIni.clear();
		m_InspectorComponentOrders.clear();
		m_InspectorPinnedComponents.clear();
		m_LastTrackedCurrentScenePath.clear();
		m_ProjectEditorLayoutRestored = false;
		m_ProjectTwoColumnLayout = true;
		m_ProjectFolderScopeEnabled = false;
		m_ProjectQuickFilter = ProjectQuickFilter::All;
		m_ContentDrawerSortMode = ContentDrawerSortMode::Path;
		m_ContentDrawerSortDescending = false;
		m_ContentDrawerDetailsVisible = true;

		const std::filesystem::path statePath = EditorProjectStatePath(projectRootPath);
		std::ifstream file(statePath, std::ios::binary);
		if (!file)
		{
			return;
		}

		const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		rapidjson::Document document;
		document.Parse(json.c_str(), json.size());
		if (document.HasParseError() || !document.IsObject())
		{
			return;
		}

		if (const auto editorLayoutIt = document.FindMember("editorLayout");
			editorLayoutIt != document.MemberEnd() && editorLayoutIt->value.IsObject())
		{
			const rapidjson::Value& editorLayout = editorLayoutIt->value;
			if (const auto iniIt = editorLayout.FindMember("ini");
				iniIt != editorLayout.MemberEnd() && iniIt->value.IsString())
			{
				m_ProjectEditorLayoutIni = iniIt->value.GetString();
			}
		}

		if (const auto inspectorIt = document.FindMember("inspector");
			inspectorIt != document.MemberEnd() && inspectorIt->value.IsObject())
		{
			const rapidjson::Value& inspector = inspectorIt->value;
			if (const auto pinnedComponentsIt = inspector.FindMember("pinnedComponents");
				pinnedComponentsIt != inspector.MemberEnd() && pinnedComponentsIt->value.IsArray())
			{
				for (const rapidjson::Value& kindValue : pinnedComponentsIt->value.GetArray())
				{
					if (!kindValue.IsString())
					{
						continue;
					}

					SceneComponentKind kind = SceneComponentKind::Mesh;
					if (TryParseComponentKindToken(kindValue.GetString(), kind) &&
						std::ranges::find(m_InspectorPinnedComponents, kind) == m_InspectorPinnedComponents.end())
					{
						m_InspectorPinnedComponents.push_back(kind);
					}
				}
				NormalizeInspectorPinnedComponents(m_InspectorPinnedComponents);
			}

			if (const auto componentOrdersIt = inspector.FindMember("componentOrders");
				componentOrdersIt != inspector.MemberEnd() && componentOrdersIt->value.IsObject())
			{
				for (auto memberIt = componentOrdersIt->value.MemberBegin(); memberIt != componentOrdersIt->value.MemberEnd(); ++memberIt)
				{
					if (!memberIt->name.IsString() || !memberIt->value.IsArray())
					{
						continue;
					}

					EntityId entityId = InvalidEntityId;
					try
					{
						size_t parsed = 0;
						const std::string entityIdText = memberIt->name.GetString();
						const unsigned long value = std::stoul(entityIdText, &parsed, 10);
						if (parsed != entityIdText.size())
						{
							continue;
						}
						entityId = static_cast<EntityId>(value);
					}
					catch (...)
					{
						continue;
					}

					std::vector<SceneComponentKind> order;
					for (const rapidjson::Value& kindValue : memberIt->value.GetArray())
					{
						if (!kindValue.IsString())
						{
							continue;
						}

						SceneComponentKind kind = SceneComponentKind::Mesh;
						if (TryParseComponentKindToken(kindValue.GetString(), kind) &&
							std::ranges::find(order, kind) == order.end())
						{
							order.push_back(kind);
						}
					}

					if (!order.empty())
					{
						m_InspectorComponentOrders[entityId] = std::move(order);
					}
				}
			}
		}

		if (const auto recentScenesIt = document.FindMember("recentScenes");
			recentScenesIt != document.MemberEnd() && recentScenesIt->value.IsArray())
		{
			for (const rapidjson::Value& recentSceneValue : recentScenesIt->value.GetArray())
			{
				if (!recentSceneValue.IsString())
				{
					continue;
				}

				std::filesystem::path scenePath = ResolveStoredProjectPath(recentSceneValue.GetString(), projectRootPath);
				if (scenePath.empty() || ToLower(scenePath.extension().string()) != ".scene")
				{
					continue;
				}

				const bool duplicate = std::ranges::any_of(m_RecentScenePaths, [&scenePath](const std::filesystem::path& existingPath)
					{
						return SamePath(existingPath, scenePath);
					});
				if (!duplicate)
				{
					m_RecentScenePaths.push_back(scenePath.lexically_normal());
				}
				if (m_RecentScenePaths.size() >= 10)
				{
					break;
				}
			}
		}

		if (const auto commandPaletteIt = document.FindMember("commandPalette");
			commandPaletteIt != document.MemberEnd() && commandPaletteIt->value.IsObject())
		{
			const rapidjson::Value& commandPalette = commandPaletteIt->value;
			if (const auto scopeIt = commandPalette.FindMember("scope");
				scopeIt != commandPalette.MemberEnd() && scopeIt->value.IsString())
			{
				m_CommandPaletteScope = CommandPaletteScopeFromToken(scopeIt->value.GetString());
			}
			if (const auto pinnedCommandsIt = commandPalette.FindMember("pinnedCommands");
				pinnedCommandsIt != commandPalette.MemberEnd() && pinnedCommandsIt->value.IsArray())
			{
				for (const rapidjson::Value& pinnedCommandValue : pinnedCommandsIt->value.GetArray())
				{
					if (!pinnedCommandValue.IsString())
					{
						continue;
					}

					const std::string pinnedCommand = pinnedCommandValue.GetString();
					if (!pinnedCommand.empty() &&
						std::ranges::find(m_CommandPalettePinnedCommands, pinnedCommand) == m_CommandPalettePinnedCommands.end())
					{
						m_CommandPalettePinnedCommands.push_back(pinnedCommand);
					}
					if (m_CommandPalettePinnedCommands.size() >= 16)
					{
						break;
					}
				}
			}
			if (const auto recentCommandsIt = commandPalette.FindMember("recentCommands");
				recentCommandsIt != commandPalette.MemberEnd() && recentCommandsIt->value.IsArray())
			{
				for (const rapidjson::Value& recentCommandValue : recentCommandsIt->value.GetArray())
				{
					if (!recentCommandValue.IsString())
					{
						continue;
					}

					const std::string recentCommand = recentCommandValue.GetString();
					if (!recentCommand.empty() &&
						std::ranges::find(m_CommandPaletteRecentCommands, recentCommand) == m_CommandPaletteRecentCommands.end())
					{
						m_CommandPaletteRecentCommands.push_back(recentCommand);
					}
					if (m_CommandPaletteRecentCommands.size() >= 12)
					{
						break;
					}
				}
			}
		}

		const auto projectBrowserIt = document.FindMember("projectBrowser");
		if (projectBrowserIt == document.MemberEnd() || !projectBrowserIt->value.IsObject())
		{
			if (!m_ProjectEditorLayoutIni.empty())
			{
				RestoreSavedEditorLayout();
			}
			return;
		}

		const rapidjson::Value& projectBrowser = projectBrowserIt->value;
		if (const auto twoColumnIt = projectBrowser.FindMember("twoColumn");
			twoColumnIt != projectBrowser.MemberEnd() && twoColumnIt->value.IsBool())
		{
			m_ProjectTwoColumnLayout = twoColumnIt->value.GetBool();
		}
		if (const auto folderScopeIt = projectBrowser.FindMember("folderScope");
			folderScopeIt != projectBrowser.MemberEnd() && folderScopeIt->value.IsBool())
		{
			m_ProjectFolderScopeEnabled = folderScopeIt->value.GetBool();
		}
		if (const auto quickFilterIt = projectBrowser.FindMember("quickFilter");
			quickFilterIt != projectBrowser.MemberEnd() && quickFilterIt->value.IsString())
		{
			m_ProjectQuickFilter = ProjectQuickFilterFromToken(quickFilterIt->value.GetString());
		}
		if (const auto contentDrawerSortIt = projectBrowser.FindMember("contentDrawerSort");
			contentDrawerSortIt != projectBrowser.MemberEnd() && contentDrawerSortIt->value.IsString())
		{
			const std::string_view sortToken = contentDrawerSortIt->value.GetString();
			m_ContentDrawerSortMode = ContentDrawerSortModeFromToken(sortToken);
			m_ContentDrawerSortDescending = sortToken == "sizeDesc" || sortToken == "modifiedDesc";
		}
		if (const auto contentDrawerSortDescendingIt = projectBrowser.FindMember("contentDrawerSortDescending");
			contentDrawerSortDescendingIt != projectBrowser.MemberEnd() && contentDrawerSortDescendingIt->value.IsBool())
		{
			m_ContentDrawerSortDescending = contentDrawerSortDescendingIt->value.GetBool();
		}
		if (const auto contentDrawerDetailsIt = projectBrowser.FindMember("contentDrawerDetails");
			contentDrawerDetailsIt != projectBrowser.MemberEnd() && contentDrawerDetailsIt->value.IsBool())
		{
			m_ContentDrawerDetailsVisible = contentDrawerDetailsIt->value.GetBool();
		}

		if (const auto favoritesIt = projectBrowser.FindMember("favorites");
			favoritesIt != projectBrowser.MemberEnd() && favoritesIt->value.IsArray())
		{
			for (const rapidjson::Value& favoriteValue : favoritesIt->value.GetArray())
			{
				if (!favoriteValue.IsString())
				{
					continue;
				}

				std::filesystem::path favoritePath = ResolveStoredProjectPath(favoriteValue.GetString(), projectRootPath);
				if (favoritePath.empty())
				{
					continue;
				}

				const bool duplicate = std::ranges::any_of(m_ProjectFavoritePaths, [&favoritePath](const std::filesystem::path& existingPath)
					{
						return SamePath(existingPath, favoritePath);
					});
				if (!duplicate)
				{
					m_ProjectFavoritePaths.push_back(favoritePath.lexically_normal());
				}
			}
		}

		if (const auto recentAssetsIt = projectBrowser.FindMember("recentAssets");
			recentAssetsIt != projectBrowser.MemberEnd() && recentAssetsIt->value.IsArray())
		{
			for (const rapidjson::Value& recentAssetValue : recentAssetsIt->value.GetArray())
			{
				if (!recentAssetValue.IsString())
				{
					continue;
				}

				std::filesystem::path recentAssetPath = ResolveStoredProjectPath(recentAssetValue.GetString(), projectRootPath);
				if (recentAssetPath.empty())
				{
					continue;
				}

				const bool duplicate = std::ranges::any_of(m_ProjectRecentAssetPaths, [&recentAssetPath](const std::filesystem::path& existingPath)
					{
						return SamePath(existingPath, recentAssetPath);
					});
				if (!duplicate)
				{
					m_ProjectRecentAssetPaths.push_back(recentAssetPath.lexically_normal());
				}
				if (m_ProjectRecentAssetPaths.size() >= 16)
				{
					break;
				}
			}
		}

		if (const auto searchesIt = projectBrowser.FindMember("savedSearches");
			searchesIt != projectBrowser.MemberEnd() && searchesIt->value.IsArray())
		{
			for (const rapidjson::Value& searchValue : searchesIt->value.GetArray())
			{
				if (!searchValue.IsString())
				{
					continue;
				}

				const std::string search = searchValue.GetString();
				if (!search.empty() && std::ranges::find(m_ProjectSavedSearches, search) == m_ProjectSavedSearches.end())
				{
					m_ProjectSavedSearches.push_back(search);
				}
			}
		}

		if (!m_ProjectEditorLayoutIni.empty())
		{
			RestoreSavedEditorLayout();
		}
	}

	void EditorLayer::SaveProjectState(const EditorContext& context) const
	{
		if (context.ProjectRootPath.empty())
		{
			return;
		}

		std::error_code errorCode;
		const std::filesystem::path statePath = EditorProjectStatePath(context.ProjectRootPath);
		std::filesystem::create_directories(statePath.parent_path(), errorCode);
		if (errorCode)
		{
			return;
		}

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(1);
		writer.Key("projectBrowser");
		writer.StartObject();
		writer.Key("twoColumn");
		writer.Bool(m_ProjectTwoColumnLayout);
		writer.Key("folderScope");
		writer.Bool(m_ProjectFolderScopeEnabled);
		writer.Key("quickFilter");
		const std::string_view quickFilterToken = ProjectQuickFilterToken(m_ProjectQuickFilter);
		writer.String(quickFilterToken.data(), static_cast<rapidjson::SizeType>(quickFilterToken.size()));
		writer.Key("contentDrawerSort");
		const std::string_view contentDrawerSortToken = ContentDrawerSortModeToken(m_ContentDrawerSortMode);
		writer.String(contentDrawerSortToken.data(), static_cast<rapidjson::SizeType>(contentDrawerSortToken.size()));
		writer.Key("contentDrawerSortDescending");
		writer.Bool(m_ContentDrawerSortDescending);
		writer.Key("contentDrawerDetails");
		writer.Bool(m_ContentDrawerDetailsVisible);
		writer.Key("favorites");
		writer.StartArray();
		for (const std::filesystem::path& favoritePath : m_ProjectFavoritePaths)
		{
			const std::string storedPath = ToStoredProjectPath(favoritePath, context.ProjectRootPath);
			writer.String(storedPath.c_str(), static_cast<rapidjson::SizeType>(storedPath.size()));
		}
		writer.EndArray();
		writer.Key("recentAssets");
		writer.StartArray();
		for (const std::filesystem::path& recentAssetPath : m_ProjectRecentAssetPaths)
		{
			const std::string storedPath = ToStoredProjectPath(recentAssetPath, context.ProjectRootPath);
			writer.String(storedPath.c_str(), static_cast<rapidjson::SizeType>(storedPath.size()));
		}
		writer.EndArray();
		writer.Key("savedSearches");
		writer.StartArray();
		for (const std::string& search : m_ProjectSavedSearches)
		{
			writer.String(search.c_str(), static_cast<rapidjson::SizeType>(search.size()));
		}
		writer.EndArray();
		writer.EndObject();
		writer.Key("recentScenes");
		writer.StartArray();
		for (const std::filesystem::path& scenePath : m_RecentScenePaths)
		{
			const std::string storedPath = ToStoredProjectPath(scenePath, context.ProjectRootPath);
			writer.String(storedPath.c_str(), static_cast<rapidjson::SizeType>(storedPath.size()));
		}
		writer.EndArray();
		writer.Key("commandPalette");
		writer.StartObject();
		writer.Key("scope");
		const std::string_view commandPaletteScopeToken = CommandPaletteScopeToken(m_CommandPaletteScope);
		writer.String(commandPaletteScopeToken.data(), static_cast<rapidjson::SizeType>(commandPaletteScopeToken.size()));
		writer.Key("pinnedCommands");
		writer.StartArray();
		for (const std::string& commandLabel : m_CommandPalettePinnedCommands)
		{
			writer.String(commandLabel.c_str(), static_cast<rapidjson::SizeType>(commandLabel.size()));
		}
		writer.EndArray();
		writer.Key("recentCommands");
		writer.StartArray();
		for (const std::string& commandLabel : m_CommandPaletteRecentCommands)
		{
			writer.String(commandLabel.c_str(), static_cast<rapidjson::SizeType>(commandLabel.size()));
		}
		writer.EndArray();
		writer.EndObject();
		writer.Key("inspector");
		writer.StartObject();
		writer.Key("pinnedComponents");
		writer.StartArray();
		for (const SceneComponentKind kind : m_InspectorPinnedComponents)
		{
			const std::string_view token = ComponentKindToken(kind);
			writer.String(token.data(), static_cast<rapidjson::SizeType>(token.size()));
		}
		writer.EndArray();
		writer.Key("componentOrders");
		writer.StartObject();
		std::vector<EntityId> orderedEntityIds;
		orderedEntityIds.reserve(m_InspectorComponentOrders.size());
		for (const auto& [entityId, order] : m_InspectorComponentOrders)
		{
			if (!order.empty())
			{
				orderedEntityIds.push_back(entityId);
			}
		}
		std::ranges::sort(orderedEntityIds);
		for (const EntityId entityId : orderedEntityIds)
		{
			const auto orderIt = m_InspectorComponentOrders.find(entityId);
			if (orderIt == m_InspectorComponentOrders.end() || orderIt->second.empty())
			{
				continue;
			}

			const std::string entityKey = std::to_string(entityId);
			writer.Key(entityKey.c_str(), static_cast<rapidjson::SizeType>(entityKey.size()));
			writer.StartArray();
			for (const SceneComponentKind kind : orderIt->second)
			{
				const std::string_view token = ComponentKindToken(kind);
				writer.String(token.data(), static_cast<rapidjson::SizeType>(token.size()));
			}
			writer.EndArray();
		}
		writer.EndObject();
		writer.EndObject();
		writer.Key("editorLayout");
		writer.StartObject();
		writer.Key("ini");
		writer.String(m_ProjectEditorLayoutIni.c_str(), static_cast<rapidjson::SizeType>(m_ProjectEditorLayoutIni.size()));
		writer.EndObject();
		writer.EndObject();

		std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return;
		}
		file.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
	}
}
