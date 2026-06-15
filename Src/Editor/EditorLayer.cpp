#include "EditorLayer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Editor
{
	namespace
	{
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

		[[nodiscard]] std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
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
			if (extension == ".txt" || extension == ".md")
			{
				return "[TXT]";
			}
			if (extension == ".h" || extension == ".cpp" || extension == ".hlsl" || extension == ".vert" || extension == ".frag")
			{
				return "[SRC]";
			}
			return "[FILE]";
		}

		[[nodiscard]] std::string RelativeDisplayPath(const std::filesystem::path& path, const std::filesystem::path& rootPath)
		{
			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(path, rootPath, errorCode);
			return errorCode ? path.string() : relativePath.string();
		}

		[[nodiscard]] std::vector<std::filesystem::directory_entry> SortedDirectoryEntries(const std::filesystem::path& directoryPath)
		{
			std::vector<std::filesystem::directory_entry> entries;
			std::error_code errorCode;
			for (std::filesystem::directory_iterator it(directoryPath, std::filesystem::directory_options::skip_permission_denied, errorCode), end;
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
			return entries;
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
	}

	void EditorLayer::Draw(EditorContext& context)
	{
		DrawDockSpace();
		DrawToolbar(context);
		DrawHierarchy(context);
		DrawSceneView(context);
		DrawGameView(context);
		DrawInspector(context);
		DrawProject();
		DrawBenchmark(context);
		DrawConsole(context);

		if (context.ShowDemoWindow)
		{
			ImGui::ShowDemoWindow(&context.ShowDemoWindow);
		}
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
		if (ImGui::Combo("##SampleMode", &sampleModeIndex, "Spider Sample\0ECS Benchmark\0"))
		{
			context.SampleMode = static_cast<Samples::Benchmark::SampleMode>(sampleModeIndex);
		}

		if (ImGui::Button("Frame Selected") && context.OnFrameSelected)
		{
			context.OnFrameSelected();
		}

		ImGui::Separator();
		ImGui::Checkbox("Demo", &context.ShowDemoWindow);
		ImGui::EndMainMenuBar();
	}

	void EditorLayer::DrawHierarchy(EditorContext& context)
	{
		SetInitialWindowRect("Hierarchy", 8.0f, 32.0f, 260.0f, 560.0f);
		ImGui::Begin("Hierarchy");

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		for (const SceneEntity& entity : context.ActiveScene.GetEntities())
		{
			const bool selected = entity.Id == selectedEntity;
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (selected)
			{
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			std::string label = entity.Name;
			if (context.ActiveScene.GetCameraComponent(entity.Id))
			{
				label.append(" [Camera]");
			}
			else if (context.ActiveScene.GetLightComponent(entity.Id))
			{
				label.append(" [Light]");
			}
			label.append("##");
			label.append(std::to_string(entity.Id));
			ImGui::TreeNodeEx(label.c_str(), flags);
			if (ImGui::IsItemClicked())
			{
				context.ActiveScene.SetSelectedEntity(entity.Id);
			}
		}

		ImGui::End();
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

		ImGui::InvisibleButton("SceneCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		StoreViewportState(m_SceneViewport, canvasPosition.x, canvasPosition.y, canvasSize.x, canvasSize.y, hovered, focused);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && context.OnScenePick)
		{
			const ImVec2 mousePosition = ImGui::GetMousePos();
			context.OnScenePick(
				mousePosition.x - canvasPosition.x,
				mousePosition.y - canvasPosition.y,
				canvasSize.x,
				canvasSize.y);
		}

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

		ImGui::End();
		ImGui::PopStyleColor();
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

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (selectedEntity == InvalidEntityId)
		{
			ImGui::TextUnformatted("No entity selected");
			ImGui::End();
			return;
		}

		const std::string* entityName = context.ActiveScene.GetEntityName(selectedEntity);
		ImGui::Text("Entity: %s", entityName ? entityName->c_str() : "<unnamed>");
		ImGui::Text("ID: %u", selectedEntity);
		ImGui::Separator();

		if (TransformComponent* transform = context.ActiveScene.GetTransformComponent(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
			{
				bool transformChanged = false;
				transformChanged |= ImGui::DragFloat3("Position", &transform->LocalTransform.Translation.x, 0.05f);
				transformChanged |= ImGui::DragFloat4("Rotation", &transform->LocalTransform.Rotation.x, 0.01f, -1.0f, 1.0f);
				transformChanged |= ImGui::DragFloat3("Scale", &transform->LocalTransform.Scale.x, 0.01f, 0.01f, 100.0f);
				if (transformChanged)
				{
					transform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(transform->LocalTransform.Rotation);
					transform->UpdateWorld();
				}
			}
		}

		if (CameraComponent* camera = context.ActiveScene.GetCameraComponent(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Camera Component", ImGuiTreeNodeFlags_DefaultOpen))
			{
				float fovDegrees = DirectX::XMConvertToDegrees(camera->FovY);
				if (ImGui::DragFloat("FOV", &fovDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
				{
					camera->FovY = DirectX::XMConvertToRadians((std::clamp)(fovDegrees, 1.0f, 179.0f));
				}
				ImGui::DragFloat("Near", &camera->NearZ, 0.01f, 0.001f, 100.0f, "%.3f");
				ImGui::DragFloat("Far", &camera->FarZ, 1.0f, 1.0f, 100000.0f, "%.1f");
				ImGui::Text("Role: %s", camera->IsGameCamera ? "Game Camera" : "Camera");

				const DirectX::XMFLOAT3 position = context.GameCamera.GetPosition();
				ImGui::Text("Runtime Position: %.2f, %.2f, %.2f", position.x, position.y, position.z);
				ImGui::Text("Runtime Aspect: %.3f", context.GameCamera.GetAspect());
			}
		}

		if (LightComponent* light = context.ActiveScene.GetLightComponent(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Light Component", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Enabled", &light->Enabled);
				int typeIndex = std::to_underlying(light->Type);
				if (ImGui::Combo("Type", &typeIndex, "Directional\0Point\0Spot\0"))
				{
					light->Type = static_cast<LightType>((std::clamp)(typeIndex, 0, 2));
				}
				ImGui::ColorEdit3("Color", &light->Color.x);
				ImGui::DragFloat("Intensity", &light->Intensity, 0.05f, 0.0f, 100.0f);
				ImGui::DragFloat("Range", &light->Range, 1.0f, 0.0f, 10000.0f);

				float spotAngleDegrees = DirectX::XMConvertToDegrees(light->SpotAngle);
				if (ImGui::DragFloat("Spot Angle", &spotAngleDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
				{
					light->SpotAngle = DirectX::XMConvertToRadians((std::clamp)(spotAngleDegrees, 1.0f, 179.0f));
				}
				ImGui::Text("Resolved Type: %s", LightTypeName(light->Type));
			}
		}

		if (const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawVector3Text("Local Min", bounds->LocalMin);
				DrawVector3Text("Local Max", bounds->LocalMax);
			}
		}

		if (const Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Text("Vertices: %d", static_cast<int>(mesh->Vertices.size()));
				ImGui::Text("Indices: %d", static_cast<int>(mesh->Indices.size()));
				ImGui::Text("Submeshes: %d", static_cast<int>(mesh->Submeshes.size()));
				ImGui::Text("Materials: %d", static_cast<int>(mesh->Materials.size()));
				ImGui::Text("Animated: %s", mesh->IsAnimated ? "true" : "false");
			}

			if (ImGui::CollapsingHeader("Materials"))
			{
				for (size_t materialIndex = 0; materialIndex < mesh->Materials.size(); ++materialIndex)
				{
					const auto& material = mesh->Materials[materialIndex];
					std::string materialLabel = material.Name.empty() ? "Material" : material.Name;
					materialLabel.append("##");
					materialLabel.append(std::to_string(materialIndex));
					if (ImGui::TreeNode(materialLabel.c_str()))
					{
						ImGui::Text("Diffuse: %s", material.DiffuseTexturePath.empty() ? "<embedded/fallback>" : material.DiffuseTexturePath.string().c_str());
						ImGui::Text("Normal: %s", material.NormalTexturePath.empty() ? "<none>" : material.NormalTexturePath.string().c_str());
						ImGui::Text("Metallic/Roughness: %s", material.MetallicRoughnessTexturePath.empty() ? "<none>" : material.MetallicRoughnessTexturePath.string().c_str());
						ImGui::TreePop();
					}
				}
			}
		}

		ImGui::End();
	}

	void EditorLayer::DrawProject()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Project", 8.0f, viewport->Size.y - 250.0f, viewport->Size.x * 0.42f, 240.0f);
		ImGui::Begin("Project");

		const std::filesystem::path rootPath("Assets");
		std::error_code errorCode;
		if (!std::filesystem::is_directory(rootPath, errorCode))
		{
			ImGui::TextUnformatted("Assets");
			ImGui::End();
			return;
		}

		ImGui::BeginChild("ProjectTree", ImVec2(0.0f, -86.0f), true);
		const bool rootOpen = ImGui::TreeNodeEx("Assets", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow);
		if (rootOpen)
		{
			DrawDirectoryRecursive(rootPath, rootPath);
			ImGui::TreePop();
		}
		ImGui::EndChild();

		DrawSelectedAssetDetails(rootPath);
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

	void EditorLayer::DrawConsole(const EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Console", viewport->Size.x * 0.76f, viewport->Size.y - 250.0f, viewport->Size.x * 0.23f, 240.0f);
		ImGui::Begin("Console");
		ImGui::Text("API: %s", GraphicsApiName(context.CurrentApi));
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
		ImGui::End();
	}

	void EditorLayer::DrawDirectoryRecursive(const std::filesystem::path& rootPath, const std::filesystem::path& directoryPath)
	{
		for (const std::filesystem::directory_entry& entry : SortedDirectoryEntries(directoryPath))
		{
			const std::filesystem::path entryPath = entry.path();
			const std::string name = entryPath.filename().string();
			if (IsDirectory(entry))
			{
				std::string label = "[D] ";
				label.append(name);
				label.append("##");
				label.append(entryPath.string());
				const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_OpenOnArrow);
				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				{
					m_SelectedAssetPath = entryPath;
				}
				if (open)
				{
					DrawDirectoryRecursive(rootPath, entryPath);
					ImGui::TreePop();
				}
				continue;
			}

			if (!IsRegularFile(entry))
			{
				continue;
			}

			std::string label = ExtensionTag(entryPath);
			label.push_back(' ');
			label.append(name);
			label.append("##");
			label.append(entryPath.string());
			const bool selected = SamePath(m_SelectedAssetPath, entryPath);
			if (ImGui::Selectable(label.c_str(), selected))
			{
				m_SelectedAssetPath = entryPath;
			}
		}

		(void)rootPath;
	}

	void EditorLayer::DrawSelectedAssetDetails(const std::filesystem::path& rootPath) const
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
		ImGui::Text("Selected Asset: %s", RelativeDisplayPath(m_SelectedAssetPath, rootPath).c_str());
		ImGui::Text("Type: %s", directory ? "Directory" : ExtensionTag(m_SelectedAssetPath));
		if (regularFile)
		{
			const uintmax_t fileSize = std::filesystem::file_size(m_SelectedAssetPath, errorCode);
			if (!errorCode)
			{
				ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(fileSize));
			}
		}
	}
}
