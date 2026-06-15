#pragma once

#include "Assets/AssetFileSystem.h"
#include "Math/Camera.h"
#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"
#include "Samples/Benchmark/BenchmarkRunner.h"
#include "Scene/Scene.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace Editor
{
	struct ViewportPanelState
	{
		float Left = 0.0f;
		float Top = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
		bool IsVisible = false;
		bool IsHovered = false;
		bool IsFocused = false;

		[[nodiscard]] bool CanRender() const noexcept
		{
			return IsVisible && Width >= 1.0f && Height >= 1.0f;
		}

		[[nodiscard]] float AspectRatio() const noexcept
		{
			return Height > 0.0f ? Width / Height : 1.0f;
		}
	};

	enum class AssetDropTarget : uint8_t
	{
		Scene,
		Game,
		External
	};

	enum class EntityDropPlacement : uint8_t
	{
		Before,
		After
	};

	struct EditorContext
	{
		GraphicsAPI CurrentApi;
		RenderMode CurrentRenderMode;
		Camera& SceneCamera;
		Camera& GameCamera;
		Scene& ActiveScene;
		Samples::Benchmark::SampleMode& SampleMode;
		Samples::Benchmark::BenchmarkRunner& BenchmarkRunner;
		bool& ShowDemoWindow;
		int ViewportWidth = 0;
		int ViewportHeight = 0;
		std::string ProjectName = "Development";
		std::filesystem::path ProjectRootPath;
		std::filesystem::path CurrentScenePath;
		std::shared_ptr<const Asset::AssetFileSnapshot> ProjectSnapshot;
		const std::vector<std::string>* AssetLogLines = nullptr;
		bool ProjectRefreshInProgress = false;
		bool IsSceneDirty = false;
		bool CanEditProjectScene = false;
		bool PhysicsSimulationEnabled = false;
		std::function<void(GraphicsAPI)> OnGraphicsApiChanged;
		std::function<void(RenderMode)> OnRenderModeChanged;
		std::function<void()> OnSaveScene;
		std::function<void()> OnSaveSceneAs;
		std::function<void()> OnOpenSceneDialog;
		std::function<void(const std::filesystem::path&)> OnOpenScene;
		std::function<void()> OnRevealProject;
		std::function<void()> OnExit;
		std::function<void()> OnFrameSelected;
		std::function<void()> OnAlignGameCameraToScene;
		std::function<void()> OnAlignSceneCameraToGame;
		std::function<void(float, float, float, float)> OnScenePick;
		std::function<void(const std::filesystem::path&)> OnAssetOpen;
		std::function<void(const std::filesystem::path&)> OnAssetReveal;
		std::function<void(const std::filesystem::path&, AssetDropTarget)> OnModelDrop;
		std::function<void()> OnProjectRefresh;
		std::function<void(EntityId, std::string_view)> OnRenameEntity;
		std::function<void(EntityId)> OnDuplicateEntity;
		std::function<void(EntityId)> OnDeleteEntity;
		std::function<void(Asset::PrimitiveMeshKind)> OnCreatePrimitive;
		std::function<void(EntityId, EntityId, EntityDropPlacement)> OnMoveEntity;
		std::function<void(EntityId, SceneComponentKind)> OnComponentAdded;
		std::function<void(EntityId, SceneComponentKind)> OnComponentRemoved;
		std::function<void(EntityId, SceneComponentKind, bool)> OnComponentEnabledChanged;
		std::function<void()> OnSceneEdited;
		std::function<void(bool)> OnPhysicsSimulationChanged;
		std::function<void(EntityId)> OnPhysicsActorDirty;
	};

	class EditorLayer
	{
	public:
		void Draw(EditorContext& context);
		[[nodiscard]] const ViewportPanelState& GetSceneViewport() const noexcept { return m_SceneViewport; }
		[[nodiscard]] const ViewportPanelState& GetGameViewport() const noexcept { return m_GameViewport; }

	private:
		void DrawDockSpace();
		void DrawToolbar(EditorContext& context);
		void DrawHierarchy(EditorContext& context);
		void DrawSceneView(EditorContext& context);
		void DrawGameView(EditorContext& context);
		void DrawInspector(EditorContext& context);
		void DrawProject(EditorContext& context);
		void DrawBenchmark(EditorContext& context);
		void DrawConsole(const EditorContext& context);
		void DrawSceneGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		void DrawGameCameraFrustumGizmo(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		void DrawColliderGizmos(EditorContext& context, ImDrawList* drawList, const ImVec2& canvasPosition, const ImVec2& canvasSize) const;
		[[nodiscard]] bool ProjectWorldToSceneCanvas(
			const Camera& sceneCamera,
			const DirectX::XMFLOAT3& worldPosition,
			const ImVec2& canvasPosition,
			const ImVec2& canvasSize,
			ImVec2& screenPosition) const;
		void BuildDefaultLayout(unsigned int dockspaceId, float viewportWidth, float viewportHeight);
		void StoreViewportState(ViewportPanelState& target, float screenLeft, float screenTop, float width, float height, bool hovered, bool focused) const;
		void DrawProjectEntryRecursive(const Asset::AssetFileEntry& entry, EditorContext& context);
		void DrawSelectedAssetDetails(const Asset::AssetFileSnapshot& snapshot, EditorContext& context) const;
		void HandleHierarchyShortcuts(EditorContext& context);
		void OpenRenamePopup(EntityId entityId, std::string_view currentName);
		void DrawRenamePopup(EditorContext& context);

		std::filesystem::path m_SelectedAssetPath;
		EntityId m_RenamingEntity = InvalidEntityId;
		std::array<char, 128> m_RenameBuffer = {};
		bool m_ShouldOpenRenamePopup = false;
		bool m_ShouldFocusRenameInput = false;
		ViewportPanelState m_SceneViewport;
		ViewportPanelState m_GameViewport;
		bool m_DefaultLayoutBuilt = false;
		bool m_ShowSceneGizmos = true;
		float m_GameCameraGizmoDepth = 25.0f;
	};
}
