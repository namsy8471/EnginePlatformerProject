#pragma once

#include "App/Win32/GameApp.h"
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
#include <string>
#include <string_view>
#include <vector>
#include <wrl.h>

struct ID3D12DescriptorHeap;
using VkDescriptorPool = struct VkDescriptorPool_T*;

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
	void DrawDx12Triangle();

	// Vulkan 리소스 관리
	[[nodiscard]] bool CreateVulkanTriangleResources();
	void DestroyVulkanTriangleResources();
	void DrawVulkanTriangle();

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
	void UpdateCameraBuffer();
	void UpdateCameraBuffer(EntityId entityId);
	void UploadEntityGeometry(EntityId entityId);
	void UpdateAnimatedMesh(float deltaTime);
	void UpdateObjectPicking();
	[[nodiscard]] bool TryPickSpider(float mouseX, float mouseY) const;
	[[nodiscard]] EntityId TryPickEntity(float mouseX, float mouseY) const;
	void RenderImGui();
	void FramePrimaryRenderableCamera();
	void DrawBenchmarkInstances();
	void DrawDx12BenchmarkInstances();
	void DrawVulkanBenchmarkInstances();
	void UploadBenchmarkGeometry(const std::vector<Asset::StaticMeshVertex>& vertices, const std::vector<uint32_t>& indices);
	void UpdateBenchmarkCameraBuffer(uint32_t instanceCount, float localScale);
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


	// 렌더링 리소스
	Rendering::GraphicsRuntime m_Graphics;
	Rendering::StaticMeshRenderer m_StaticMeshRenderer;
	HWND m_hRenderWnd = nullptr;
	Camera m_Camera;
	Scene m_Scene;
	EntityId m_SpiderEntity = InvalidEntityId;
	SceneRenderState m_RenderState;
	Samples::Benchmark::SampleMode m_SampleMode = Samples::Benchmark::SampleMode::SpiderSample;
	Samples::Benchmark::SampleMode m_LastSampleMode = Samples::Benchmark::SampleMode::SpiderSample;
	Samples::Benchmark::BenchmarkRunner m_BenchmarkRunner;
	float m_AnimationTimeSeconds = 0.0f;

	// 현재 렌더 모드 상태
	RenderMode m_RenderMode = RenderMode::Forward;
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
