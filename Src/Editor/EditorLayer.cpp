#include "EditorLayer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Editor
{
	namespace
	{
		constexpr const char* kAssetPathPayload = "ENGINE_ASSET_PATH";

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
		HandleHierarchyShortcuts(context);
		DrawHierarchy(context);
		DrawSceneView(context);
		DrawGameView(context);
		DrawInspector(context);
		DrawProject(context);
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
		if (ImGui::Combo("##SampleMode", &sampleModeIndex, "Project Scene\0Spider Sample\0ECS Benchmark\0"))
		{
			context.SampleMode = static_cast<Samples::Benchmark::SampleMode>(sampleModeIndex);
		}

		if (ImGui::Button("Frame Selected") && context.OnFrameSelected)
		{
			context.OnFrameSelected();
		}

		ImGui::Separator();
		ImGui::Checkbox("Gizmos", &m_ShowSceneGizmos);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Show Scene View camera gizmos");
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
		EntityId pendingDuplicateEntity = InvalidEntityId;
		EntityId pendingDeleteEntity = InvalidEntityId;
		for (const SceneEntity& entity : context.ActiveScene.GetEntities())
		{
			const bool selected = entity.Id == selectedEntity;
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (selected)
			{
				flags |= ImGuiTreeNodeFlags_Selected;
			}

			const std::string* entityName = context.ActiveScene.GetEntityName(entity.Id);
			const std::string displayName = entityName && !entityName->empty() ? *entityName : "<unnamed>";
			std::string label = displayName;
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
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{
				context.ActiveScene.SetSelectedEntity(entity.Id);
			}

			const std::string popupId = "HierarchyEntityContext##" + std::to_string(entity.Id);
			if (ImGui::BeginPopupContextItem(popupId.c_str()))
			{
				context.ActiveScene.SetSelectedEntity(entity.Id);
				if (ImGui::MenuItem("Rename"))
				{
					OpenRenamePopup(entity.Id, displayName);
				}
				if (ImGui::MenuItem("Duplicate"))
				{
					pendingDuplicateEntity = entity.Id;
				}
				if (ImGui::MenuItem("Delete"))
				{
					pendingDeleteEntity = entity.Id;
				}
				ImGui::EndPopup();
			}
		}

		DrawRenamePopup(context);

		if (pendingDuplicateEntity != InvalidEntityId && context.OnDuplicateEntity)
		{
			context.OnDuplicateEntity(pendingDuplicateEntity);
		}
		if (pendingDeleteEntity != InvalidEntityId && context.OnDeleteEntity)
		{
			context.OnDeleteEntity(pendingDeleteEntity);
		}

		ImGui::End();
	}

	void EditorLayer::HandleHierarchyShortcuts(EditorContext& context)
	{
		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (selectedEntity == InvalidEntityId)
		{
			return;
		}

		if (m_RenamingEntity != InvalidEntityId || m_ShouldOpenRenamePopup)
		{
			return;
		}

		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput || ImGui::IsAnyItemActive())
		{
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
		{
			if (const std::string* entityName = context.ActiveScene.GetEntityName(selectedEntity))
			{
				OpenRenamePopup(selectedEntity, *entityName);
			}
			return;
		}

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		{
			if (context.OnDuplicateEntity)
			{
				context.OnDuplicateEntity(selectedEntity);
			}
			return;
		}

		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			if (context.OnDeleteEntity)
			{
				context.OnDeleteEntity(selectedEntity);
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
		AcceptModelDrop(context, AssetDropTarget::Scene);
		DrawSceneGizmos(context, drawList, canvasPosition, canvasSize);

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

	void EditorLayer::DrawSceneGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const
	{
		if (!m_ShowSceneGizmos || !drawList || canvasSize.x < 1.0f || canvasSize.y < 1.0f)
		{
			return;
		}

		const ImVec2 canvasMax(canvasPosition.x + canvasSize.x, canvasPosition.y + canvasSize.y);
		drawList->PushClipRect(canvasPosition, canvasMax, true);
		DrawGameCameraFrustumGizmo(context, drawList, canvasPosition, canvasSize);
		drawList->PopClipRect();
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
				if (mesh->IsAnimated)
				{
					ImGui::Text("Animation Clips: %d", static_cast<int>(mesh->Animations.size()));
					ImGui::Text("Bones: %d", static_cast<int>(mesh->Bones.size()));
				}
			}

			if (mesh->IsAnimated && !mesh->Animations.empty())
			{
				AnimatorComponent* animator = context.ActiveScene.GetAnimatorComponent(selectedEntity);
				if (!animator)
				{
					if (ImGui::Button("Add Animator"))
					{
						animator = &context.ActiveScene.EnsureAnimatorComponent(selectedEntity);
					}
				}

				if (animator && ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen))
				{
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
							}
							if (selectedClip)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}

					ImGui::Checkbox("Playing", &animator->Playing);
					ImGui::SameLine();
					ImGui::Checkbox("Loop", &animator->Loop);
					ImGui::DragFloat("Speed", &animator->Speed, 0.01f, 0.0f, 10.0f, "%.2f");

					const double ticksPerSecond = currentClip.TicksPerSecond > 0.0 ? currentClip.TicksPerSecond : 25.0;
					const float durationSeconds = currentClip.DurationTicks > 0.0
						? static_cast<float>(currentClip.DurationTicks / ticksPerSecond)
						: 0.0f;
					if (durationSeconds > 0.0f)
					{
						animator->TimeSeconds = (std::clamp)(animator->TimeSeconds, 0.0f, durationSeconds);
						ImGui::SliderFloat("Time", &animator->TimeSeconds, 0.0f, durationSeconds, "%.3f sec");
					}
					else
					{
						ImGui::TextUnformatted("Time: <invalid duration>");
					}

					ImGui::Text("Duration: %.3f sec / %.1f ticks", durationSeconds, currentClip.DurationTicks);
					ImGui::Text("Ticks/sec: %.2f", ticksPerSecond);
					ImGui::Text("Channels: %d", static_cast<int>(currentClip.Channels.size()));
				}
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

	void EditorLayer::DrawProject(EditorContext& context)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		SetInitialWindowRect("Project", 8.0f, viewport->Size.y - 250.0f, viewport->Size.x * 0.42f, 240.0f);
		ImGui::Begin("Project");

		if (ImGui::Button("Refresh") && context.OnProjectRefresh)
		{
			context.OnProjectRefresh();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(context.ProjectRefreshInProgress ? "Scanning..." : "Ready");

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

		ImGui::BeginChild("ProjectTree", ImVec2(0.0f, -86.0f), true);
		const std::string rootLabel = snapshot->RootPath.filename().empty()
			? snapshot->RootPath.string()
			: snapshot->RootPath.filename().string();
		const bool rootOpen = ImGui::TreeNodeEx(rootLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow);
		if (rootOpen)
		{
			for (const auto& entry : snapshot->Children)
			{
				DrawProjectEntryRecursive(entry, context);
			}
			ImGui::TreePop();
		}
		ImGui::EndChild();

		DrawSelectedAssetDetails(*snapshot, context);
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
		if (context.AssetLogLines && !context.AssetLogLines->empty())
		{
			ImGui::Separator();
			const size_t logCount = context.AssetLogLines->size();
			const size_t firstLogIndex = logCount > 8 ? logCount - 8 : 0;
			for (size_t logIndex = firstLogIndex; logIndex < logCount; ++logIndex)
			{
				ImGui::TextWrapped("%s", (*context.AssetLogLines)[logIndex].c_str());
			}
		}
		ImGui::End();
	}

	void EditorLayer::DrawProjectEntryRecursive(const Asset::AssetFileEntry& entry, EditorContext& context)
	{
		const std::filesystem::path entryPath = entry.Path;
		const bool directory = entry.Kind == Asset::AssetFileKind::Directory;
		std::string label = AssetKindTag(entry.Kind);
		label.push_back(' ');
		label.append(entry.Name);
		label.append("##");
		label.append(entryPath.string());

		if (directory)
		{
			const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_OpenOnArrow);
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			{
				m_SelectedAssetPath = entryPath;
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
		}
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && context.OnAssetOpen)
		{
			context.OnAssetOpen(entryPath);
		}
		if (entry.Kind == Asset::AssetFileKind::Model && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			const std::string payloadPath = entryPath.string();
			ImGui::SetDragDropPayload(kAssetPathPayload, payloadPath.c_str(), payloadPath.size() + 1);
			ImGui::Text("Load %s", entry.Name.c_str());
			ImGui::EndDragDropSource();
		}
	}

	void EditorLayer::DrawSelectedAssetDetails(const Asset::AssetFileSnapshot& snapshot, EditorContext& context) const
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
		if (regularFile)
		{
			const uintmax_t fileSize = std::filesystem::file_size(m_SelectedAssetPath, errorCode);
			if (!errorCode)
			{
				ImGui::Text("Size: %llu bytes", static_cast<unsigned long long>(fileSize));
			}
		}

		if (ImGui::Button("Open") && context.OnAssetOpen)
		{
			context.OnAssetOpen(m_SelectedAssetPath);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reveal") && context.OnAssetReveal)
		{
			context.OnAssetReveal(m_SelectedAssetPath);
		}
		if (regularFile && Asset::IsModelAssetPath(m_SelectedAssetPath))
		{
			ImGui::SameLine();
			if (ImGui::Button("Load Model") && context.OnModelDrop)
			{
				context.OnModelDrop(m_SelectedAssetPath, AssetDropTarget::Game);
			}
		}
	}
}
