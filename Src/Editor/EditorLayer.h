#pragma once

#include "Math/Camera.h"
#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/RenderMode.h"
#include "Samples/Benchmark/BenchmarkRunner.h"
#include "Scene/Scene.h"

#include <filesystem>
#include <functional>

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
		std::function<void(GraphicsAPI)> OnGraphicsApiChanged;
		std::function<void(RenderMode)> OnRenderModeChanged;
		std::function<void()> OnFrameSelected;
		std::function<void(float, float, float, float)> OnScenePick;
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
		void DrawProject();
		void DrawBenchmark(EditorContext& context);
		void DrawConsole(const EditorContext& context);
		void BuildDefaultLayout(unsigned int dockspaceId, float viewportWidth, float viewportHeight);
		void StoreViewportState(ViewportPanelState& target, float screenLeft, float screenTop, float width, float height, bool hovered, bool focused) const;
		void DrawDirectoryRecursive(const std::filesystem::path& rootPath, const std::filesystem::path& directoryPath);
		void DrawSelectedAssetDetails(const std::filesystem::path& rootPath) const;

		std::filesystem::path m_SelectedAssetPath;
		ViewportPanelState m_SceneViewport;
		ViewportPanelState m_GameViewport;
		bool m_DefaultLayoutBuilt = false;
	};
}
