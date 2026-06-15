#pragma once

#include "App/Win32/GameApp.h"
#include "Editor/EditorLayer.h"
#include "Scene/Scene.h"
#include "Rendering/RHI/IGraphicsDevice.h"
#include "Rendering/RHI/ICommandList.h"
#include "Rendering/RHI/IBuffer.h"
#include "Rendering/RHI/GraphicsCommon.h"
#include "Rendering/GraphicsRuntime.h"
#include "Rendering/RenderMode.h"
#include "Rendering/Systems/StaticMeshRenderer.h"
#include "Samples/Benchmark/BenchmarkRunner.h"
#include "Scene/SceneRenderState.h"
#include "Math/Camera.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <wrl.h>

struct ID3D12DescriptorHeap;
using VkDescriptorPool = struct VkDescriptorPool_T*;
struct CameraConstants;

class Engine : public GameApp
{
public:
	Engine(HINSTANCE hInstance);
	~Engine() override;

	[[nodiscard]] bool Init() override;
	LRESULT MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

protected:
	void Update(float deltaTime) override;
	void Render() override;
	void OnResize() override;

private:
	// Graphics API 전환 및 관리
	[[nodiscard]] bool SwitchGraphicsAPI(GraphicsAPI api);
	void SwitchRenderMode(RenderMode renderMode);
	void ShutdownGraphics();

	// 렌더 윈도우 관리 (DX12 flip-model과 Vulkan surface를 분리하기 위한 child HWND)
	[[nodiscard]] bool CreateRenderWindow();
	void DestroyRenderWindow();
	void ResizeRenderWindow();

	// DirectX12 리소스 관리
	[[nodiscard]] bool CreateDx12TriangleResources();
	void DestroyDx12TriangleResources();
	void DrawDx12Triangle(const Camera& camera);

	// Vulkan 리소스 관리
	[[nodiscard]] bool CreateVulkanTriangleResources();
	void DestroyVulkanTriangleResources();
	void DrawVulkanTriangle(const Camera& camera);

	// 공통 리소스 관리
	[[nodiscard]] bool LoadSpiderStaticMesh();
	[[nodiscard]] bool CreateTriangleVertexBuffer();
	[[nodiscard]] bool CreateIndexBuffer();
	[[nodiscard]] bool CreateCameraBuffer();
	[[nodiscard]] bool LoadMaterialTextures();
	[[nodiscard]] bool CreateTextureResources();
	[[nodiscard]] bool CreateImGuiResources();
	void DestroyTextureResources();
	void DestroyImGuiResources();
	void ResetCameraConstantAllocator() noexcept;
	[[nodiscard]] uint64_t AllocateCameraConstantOffset() noexcept;
	[[nodiscard]] uint64_t WriteCameraConstants(const CameraConstants& cameraConstants);
	[[nodiscard]] uint64_t UpdateCameraBuffer();
	[[nodiscard]] uint64_t UpdateCameraBuffer(EntityId entityId);
	[[nodiscard]] uint64_t UpdateCameraBuffer(const Camera& camera);
	[[nodiscard]] uint64_t UpdateCameraBuffer(EntityId entityId, const Camera& camera);
	void UploadEntityGeometry(EntityId entityId);
	void UpdateAnimatedMesh(float deltaTime);
	void UpdateObjectPicking();
	[[nodiscard]] bool TryPickSpider(float mouseX, float mouseY) const;
	[[nodiscard]] EntityId TryPickEntity(float mouseX, float mouseY) const;
	[[nodiscard]] EntityId TryPickEntity(float mouseX, float mouseY, const Camera& camera, float viewportWidth, float viewportHeight) const;
	void BeginEditorFrame();
	void RenderEditorDrawData();
	void UpdateEditorCameraFromInput(float deltaTime);
	void UpdateViewportCameraLenses();
	void RenderWorldViewport(const Editor::ViewportPanelState& viewport, const Camera& camera);
	void FramePrimaryRenderableCamera();
	void FrameSelectedEntityCamera();
	void FrameEntityCamera(EntityId entityId);
	void FramePrimaryRenderableCamera(Camera& camera);
	void FrameSelectedEntityCamera(Camera& camera);
	void FrameEntityCamera(Camera& camera, EntityId entityId);
	void DrawBenchmarkInstances(const Camera& camera);
	void DrawDx12BenchmarkInstances(const Camera& camera);
	void DrawVulkanBenchmarkInstances(const Camera& camera);
	void UploadBenchmarkGeometry(std::span<const Asset::StaticMeshVertex> vertices, std::span<const uint32_t> indices);
	[[nodiscard]] uint64_t UpdateBenchmarkCameraBuffer(const Camera& camera, uint32_t instanceCount, float localScale);
	[[nodiscard]] EntityId CreateEntity(std::string_view name);
	[[nodiscard]] TransformComponent* GetTransformComponent(EntityId entityId);
	[[nodiscard]] const TransformComponent* GetTransformComponent(EntityId entityId) const;
	[[nodiscard]] Asset::StaticMeshAsset* GetMeshAsset(EntityId entityId);
	[[nodiscard]] const Asset::StaticMeshAsset* GetMeshAsset(EntityId entityId) const;
	[[nodiscard]] std::vector<CpuMaterialTexture>* GetMaterialTextures(EntityId entityId);
	[[nodiscard]] const std::vector<CpuMaterialTexture>* GetMaterialTextures(EntityId entityId) const;
	[[nodiscard]] const std::string* GetEntityName(EntityId entityId) const;
	[[nodiscard]] bool IsMaterialTransparent(EntityId entityId, size_t materialIndex) const;
	void RebuildWindowTitleBase();

	// UI 업데이트
	void UpdateRendererMenuState();
	void ResetFpsCounter();
	void UpdateWindowTitleWithFps();
	void ProcessPendingGraphicsApiSwitch();
	void CreateEditorSceneEntities();
	void SyncRuntimeCameraToGameCameraEntity();
	void SyncGameCameraFromSceneEntity();
	[[nodiscard]] bool IsGameCameraEntity(EntityId entityId) const noexcept;


	// 렌더링 리소스
	Rendering::GraphicsRuntime m_Graphics;
	Rendering::StaticMeshRenderer m_StaticMeshRenderer;
	HWND m_hRenderWnd = nullptr;
	Camera m_Camera;
	Camera m_SceneCamera;
	Scene m_Scene;
	EntityId m_SpiderEntity = InvalidEntityId;
	EntityId m_GameCameraEntity = InvalidEntityId;
	EntityId m_KeyLightEntity = InvalidEntityId;
	SceneRenderState m_RenderState;
	Samples::Benchmark::SampleMode m_SampleMode = Samples::Benchmark::SampleMode::SpiderSample;
	Samples::Benchmark::SampleMode m_LastSampleMode = Samples::Benchmark::SampleMode::SpiderSample;
	Samples::Benchmark::BenchmarkRunner m_BenchmarkRunner;
	Editor::EditorLayer m_EditorLayer;
	float m_AnimationTimeSeconds = 0.0f;
	float m_LastDeltaTime = 1.0f / 60.0f;
	bool m_SceneCameraControlActive = false;

	// 현재 렌더 모드 상태
	RenderMode m_RenderMode = RenderMode::Forward;
	GraphicsAPI m_PendingGraphicsApi = GraphicsAPI::Vulkan;
	bool m_HasPendingGraphicsApiSwitch = false;
	std::wstring m_WindowTitleBase = L"EnginePlatformer - Vulkan - Forward";

	// FPS 카운팅
	uint32_t m_FrameCount = 0;
	std::chrono::steady_clock::time_point m_LastFpsUpdate = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point m_RenderStartTime = std::chrono::steady_clock::now();

	struct Dx12ImGuiResources
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
	} m_Dx12ImGui;

	struct VulkanImGuiResources
	{
		VkDescriptorPool DescriptorPool = nullptr;
	} m_VulkanImGui;

	bool m_ImGuiInitialized = false;
	bool m_ShowImGuiDemoWindow = false;
};
