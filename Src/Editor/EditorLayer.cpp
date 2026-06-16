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
		constexpr const char* kHierarchyEntityPayload = "ENGINE_HIERARCHY_ENTITY";

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
			default:
				return "Component";
			}
		}

		struct ComponentSectionState
		{
			bool Open = false;
			bool Enabled = true;
			bool Removed = false;
		};

		template <typename Component>
		[[nodiscard]] ComponentSectionState BeginComponentSection(
			EditorContext& context,
			EntityId entityId,
			SceneComponentKind kind,
			const char* label,
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

		void DrawMaterialTextureSlotRow(
			EditorContext& context,
			EntityId entityId,
			size_t materialIndex,
			const Asset::StaticMeshMaterial& material,
			Asset::MaterialTextureSlot slot)
		{
			const std::filesystem::path path = Asset::GetMaterialTexturePath(material, slot);
			const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
			const std::string id = std::format("{}##mat{}_slot{}", Asset::MaterialTextureSlotName(slot), materialIndex, Asset::MaterialTextureSlotIndex(slot));

			ImGui::PushID(id.c_str());
			ImGui::Text("%s", std::string(Asset::MaterialTextureSlotName(slot)).c_str());
			ImGui::SameLine(130.0f);
			ImGui::TextWrapped("%s", path.empty() ? (binding.Embedded.IsValid() ? "<embedded>" : "<fallback>") : path.filename().string().c_str());
			if (!path.empty() && ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", path.string().c_str());
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
			ImGui::PopID();
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

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		EntityId pendingDuplicateEntity = InvalidEntityId;
		EntityId pendingDeleteEntity = InvalidEntityId;
		EntityId pendingMoveEntity = InvalidEntityId;
		EntityId pendingMoveTarget = InvalidEntityId;
		EntityDropPlacement pendingMovePlacement = EntityDropPlacement::After;
		Asset::PrimitiveMeshKind pendingPrimitiveKind = Asset::PrimitiveMeshKind::None;
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

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				ImGui::SetDragDropPayload(kHierarchyEntityPayload, &entity.Id, sizeof(EntityId));
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
						if (movedEntity != entity.Id)
						{
							const float itemCenterY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
							pendingMoveEntity = movedEntity;
							pendingMoveTarget = entity.Id;
							pendingMovePlacement = ImGui::GetMousePos().y < itemCenterY
								? EntityDropPlacement::Before
								: EntityDropPlacement::After;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		if (ImGui::BeginPopupContextWindow("HierarchyBlankContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			if (ImGui::BeginMenu("Create"))
			{
				if (ImGui::MenuItem("Cube"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Cube;
				}
				if (ImGui::MenuItem("Sphere"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Sphere;
				}
				if (ImGui::MenuItem("Capsule"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Capsule;
				}
				if (ImGui::MenuItem("Plane"))
				{
					pendingPrimitiveKind = Asset::PrimitiveMeshKind::Plane;
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
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
		if (pendingMoveEntity != InvalidEntityId && pendingMoveTarget != InvalidEntityId && context.OnMoveEntity)
		{
			context.OnMoveEntity(pendingMoveEntity, pendingMoveTarget, pendingMovePlacement);
		}
		if (pendingPrimitiveKind != Asset::PrimitiveMeshKind::None && context.OnCreatePrimitive)
		{
			context.OnCreatePrimitive(pendingPrimitiveKind);
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

		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
		{
			if (context.CanEditProjectScene && context.OnSaveScene)
			{
				context.OnSaveScene();
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

		const EntityId selectedEntity = context.ActiveScene.GetSelectedEntity();
		if (selectedEntity == InvalidEntityId)
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
		DrawColliderGizmos(context, drawList, canvasPosition, canvasSize);
		drawList->PopClipRect();
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
					if (context.OnSceneEdited)
					{
						context.OnSceneEdited();
					}
					if (context.OnPhysicsActorDirty)
					{
						context.OnPhysicsActorDirty(selectedEntity);
					}
				}
			}
		}

		if (CameraComponent* camera = context.ActiveScene.GetCameraComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<CameraComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Camera,
				"Camera");
			if (section.Open && !section.Removed)
			{
				{
					ImGui::BeginDisabled(!section.Enabled);
					bool cameraChanged = false;
					float fovDegrees = DirectX::XMConvertToDegrees(camera->FovY);
					if (ImGui::DragFloat("FOV", &fovDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
					{
						camera->FovY = DirectX::XMConvertToRadians((std::clamp)(fovDegrees, 1.0f, 179.0f));
						cameraChanged = true;
					}
					cameraChanged |= ImGui::DragFloat("Near", &camera->NearZ, 0.01f, 0.001f, 100.0f, "%.3f");
					cameraChanged |= ImGui::DragFloat("Far", &camera->FarZ, 1.0f, 1.0f, 100000.0f, "%.1f");
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

		if (LightComponent* light = context.ActiveScene.GetLightComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<LightComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Light,
				"Light");
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				bool lightChanged = false;
				lightChanged |= ImGui::Checkbox("Emits Light", &light->Enabled);
				int typeIndex = std::to_underlying(light->Type);
				if (ImGui::Combo("Type", &typeIndex, "Directional\0Point\0Spot\0"))
				{
					light->Type = static_cast<LightType>((std::clamp)(typeIndex, 0, 2));
					lightChanged = true;
				}
				lightChanged |= ImGui::ColorEdit3("Color", &light->Color.x);
				lightChanged |= ImGui::DragFloat("Intensity", &light->Intensity, 0.05f, 0.0f, 100.0f);
				lightChanged |= ImGui::DragFloat("Range", &light->Range, 1.0f, 0.0f, 10000.0f);
				lightChanged |= ImGui::Checkbox("Cast Shadows", &light->CastShadows);
				lightChanged |= ImGui::DragFloat("Shadow Bias", &light->ShadowBias, 0.0001f, 0.0f, 0.1f, "%.5f");
				lightChanged |= ImGui::DragFloat("Shadow Normal Bias", &light->ShadowNormalBias, 0.001f, 0.0f, 1.0f, "%.4f");
				lightChanged |= ImGui::DragFloat("Shadow Strength", &light->ShadowStrength, 0.01f, 0.0f, 1.0f, "%.2f");

				float spotAngleDegrees = DirectX::XMConvertToDegrees(light->SpotAngle);
				if (ImGui::DragFloat("Spot Angle", &spotAngleDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg"))
				{
					light->SpotAngle = DirectX::XMConvertToRadians((std::clamp)(spotAngleDegrees, 1.0f, 179.0f));
					lightChanged = true;
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

		if (RigidBodyComponent* rigidBody = context.ActiveScene.GetRigidBodyComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<RigidBodyComponent>(
				context,
				selectedEntity,
				SceneComponentKind::RigidBody,
				"Rigidbody");
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
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
				}
				physicsChanged |= ImGui::DragFloat("Mass", &rigidBody->Mass, 0.05f, 0.001f, 10000.0f);
				physicsChanged |= ImGui::DragFloat("Linear Damping", &rigidBody->LinearDamping, 0.01f, 0.0f, 100.0f);
				physicsChanged |= ImGui::DragFloat("Angular Damping", &rigidBody->AngularDamping, 0.01f, 0.0f, 100.0f);
				physicsChanged |= ImGui::Checkbox("Use Gravity", &rigidBody->UseGravity);
				physicsChanged |= ImGui::DragFloat3("Linear Velocity", &rigidBody->LinearVelocity.x, 0.05f);
				physicsChanged |= ImGui::DragFloat3("Angular Velocity", &rigidBody->AngularVelocity.x, 0.05f);
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

		if (ColliderComponent* collider = context.ActiveScene.GetColliderComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<ColliderComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Collider,
				"Collider");
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
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
				}
				physicsChanged |= ImGui::DragFloat3("Collider Size", &collider->Size.x, 0.05f, 0.001f, 10000.0f);
				physicsChanged |= ImGui::DragFloat("Radius", &collider->Radius, 0.01f, 0.001f, 10000.0f);
				physicsChanged |= ImGui::DragFloat("Height", &collider->Height, 0.01f, 0.001f, 10000.0f);
				physicsChanged |= ImGui::DragFloat3("Offset", &collider->Offset.x, 0.01f);
				physicsChanged |= ImGui::Checkbox("Trigger", &collider->IsTrigger);
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

		if (PhysicsMaterialComponent* physicsMaterial = context.ActiveScene.GetPhysicsMaterialComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<PhysicsMaterialComponent>(
				context,
				selectedEntity,
				SceneComponentKind::PhysicsMaterial,
				"Physics Material");
			if (section.Open && !section.Removed)
			{
				ImGui::BeginDisabled(!section.Enabled);
				bool physicsChanged = false;
				if (!context.ActiveScene.GetColliderComponent(selectedEntity) || !context.ActiveScene.IsColliderEnabled(selectedEntity))
				{
					ImGui::TextUnformatted("No enabled collider: material is ignored.");
				}
				physicsChanged |= ImGui::DragFloat("Static Friction", &physicsMaterial->StaticFriction, 0.01f, 0.0f, 10.0f);
				physicsChanged |= ImGui::DragFloat("Dynamic Friction", &physicsMaterial->DynamicFriction, 0.01f, 0.0f, 10.0f);
				physicsChanged |= ImGui::DragFloat("Restitution", &physicsMaterial->Restitution, 0.01f, 0.0f, 1.0f);
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

		if (const BoundsComponent* bounds = context.ActiveScene.GetBoundsComponent(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Bounds", ImGuiTreeNodeFlags_DefaultOpen))
			{
				DrawVector3Text("Local Min", bounds->LocalMin);
				DrawVector3Text("Local Max", bounds->LocalMax);
			}
		}

		if (MeshComponent* meshComponent = context.ActiveScene.GetMeshComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<MeshComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Mesh,
				"Mesh");
			if (section.Open && !section.Removed)
			{
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

		if (AnimatorComponent* animator = context.ActiveScene.GetAnimatorComponent(selectedEntity))
		{
			const ComponentSectionState section = BeginComponentSection<AnimatorComponent>(
				context,
				selectedEntity,
				SceneComponentKind::Animator,
				"Animator");
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

		if (Asset::StaticMeshAsset* mesh = context.ActiveScene.GetMeshAsset(selectedEntity))
		{
			if (ImGui::CollapsingHeader("Materials"))
			{
				for (size_t materialIndex = 0; materialIndex < mesh->Materials.size(); ++materialIndex)
				{
					auto& material = mesh->Materials[materialIndex];
					std::string materialLabel = material.Name.empty() ? "Material" : material.Name;
					materialLabel.append("##");
					materialLabel.append(std::to_string(materialIndex));
					if (ImGui::TreeNode(materialLabel.c_str()))
					{
						int shadingModelIndex = material.ShadingModel == Asset::MaterialShadingModel::PBR
							? 1
							: material.ShadingModel == Asset::MaterialShadingModel::Unlit ? 2 : 0;
						if (ImGui::Combo("Shading Model", &shadingModelIndex, "Phong\0PBR\0Unlit\0"))
						{
							const auto model = shadingModelIndex == 1
								? Asset::MaterialShadingModel::PBR
								: shadingModelIndex == 2 ? Asset::MaterialShadingModel::Unlit : Asset::MaterialShadingModel::Phong;
							material.ShadingModel = model;
							if (context.OnMaterialShadingModelChanged)
							{
								context.OnMaterialShadingModelChanged(selectedEntity, materialIndex, model);
							}
						}

						bool materialChanged = false;
						materialChanged |= ImGui::ColorEdit4("Base Color", &material.DiffuseColor.x);
						materialChanged |= ImGui::Checkbox("Use Vertex Color", &material.UseVertexColor);
						materialChanged |= ImGui::Checkbox("Normal Y Flip", &material.NormalYFlip);
						materialChanged |= ImGui::ColorEdit3("Emissive", &material.EmissiveColor.x);
						materialChanged |= ImGui::DragFloat("Opacity", &material.Opacity, 0.01f, 0.0f, 1.0f);
						if (material.ShadingModel == Asset::MaterialShadingModel::PBR)
						{
							materialChanged |= ImGui::DragFloat("Metallic", &material.MetallicFactor, 0.01f, 0.0f, 1.0f);
							materialChanged |= ImGui::DragFloat("Roughness", &material.RoughnessFactor, 0.01f, 0.02f, 1.0f);
						}
						else if (material.ShadingModel == Asset::MaterialShadingModel::Phong)
						{
							materialChanged |= ImGui::ColorEdit3("Specular", &material.SpecularColor.x);
							materialChanged |= ImGui::DragFloat("Shininess", &material.Shininess, 1.0f, 1.0f, 1024.0f);
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

						ImGui::Separator();
						ImGui::TextUnformatted("Texture Slots");
						for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
						{
							const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
							if (ShouldShowMaterialSlot(material.ShadingModel, slot))
							{
								DrawMaterialTextureSlotRow(context, selectedEntity, materialIndex, material, slot);
							}
						}
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
		if ((entry.Kind == Asset::AssetFileKind::Model || entry.Kind == Asset::AssetFileKind::Image)
			&& ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
		{
			const std::string payloadPath = entryPath.string();
			ImGui::SetDragDropPayload(kAssetPathPayload, payloadPath.c_str(), payloadPath.size() + 1);
			ImGui::Text("%s %s", entry.Kind == Asset::AssetFileKind::Model ? "Load" : "Use", entry.Name.c_str());
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
