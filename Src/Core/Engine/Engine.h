#pragma once

#include "App/Win32/GameApp.h"
#include "Assets/AssetFileSystem.h"
#include "Assets/AssetHotReloadService.h"
#include "Assets/AssetImportService.h"
#include "Assets/RuntimeAssetRegistry.h"
#include "Editor/EditorLayer.h"
#include "Core/Engine/EngineStartupOptions.h"
#include "Physics/PhysicsWorld.h"
#include "Projects/ProjectService.h"
#include "Scene/Scene.h"
#include "Scene/ScenePersistenceService.h"
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
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wrl.h>

struct ID3D12DescriptorHeap;
using VkDescriptorPool = struct VkDescriptorPool_T*;
struct CameraConstants;

class Engine : public GameApp
{
public:
	explicit Engine(HINSTANCE hInstance, EngineStartupOptions startupOptions = {});
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
	[[nodiscard]] bool EnsureGeometryBufferCapacity(size_t vertexCount, size_t indexCount);
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
	void QueueModelImport(const std::filesystem::path& sourcePath, const Camera& placementCamera, bool isReload);
	void QueueModelImportForSceneEntity(const ScenePersistence::LoadedSceneEntity& loadedEntity, EntityId targetEntity);
	void QueueModelImportFromDrop(const std::filesystem::path& sourcePath, Editor::AssetDropTarget target);
	void QueueModelReload(const std::filesystem::path& sourcePath, const std::filesystem::path& changedPath);
	void DrainCompletedAssetJobs();
	void ApplyImportedModel(Asset::AssetImportResult result);
	void ApplyReloadedAsset(Asset::AssetImportResult result);
	void HandleDroppedFiles(HDROP dropHandle);
	void OpenAssetPath(const std::filesystem::path& path);
	void RevealAssetPath(const std::filesystem::path& path) const;
	void AppendAssetLog(std::string message);
	[[nodiscard]] Math::Transform BuildDroppedModelTransform(const Asset::AssetImportResult& result) const;
	[[nodiscard]] static std::vector<std::filesystem::path> CollectWatchedTexturePaths(const std::vector<CpuMaterialTexture>& materialTextures);
	[[nodiscard]] bool SaveCurrentScene();
	[[nodiscard]] bool SaveCurrentSceneAs();
	[[nodiscard]] bool OpenSceneFromDialog();
	[[nodiscard]] bool OpenSceneFromPath(const std::filesystem::path& scenePath, bool promptForDirtyScene);
	[[nodiscard]] bool ConfirmSaveDirtyScene();
	[[nodiscard]] std::optional<std::filesystem::path> ShowOpenSceneDialog() const;
	[[nodiscard]] std::optional<std::filesystem::path> ShowSaveSceneDialog() const;
	[[nodiscard]] std::filesystem::path GetDefaultScenePath() const;
	void ClearProjectSceneRuntimeState();
	void MarkSceneDirty();
	void SetSceneDirty(bool dirty);
	void MoveEntityInHierarchy(EntityId movedEntity, EntityId targetEntity, Editor::EntityDropPlacement placement);
	void AlignGameCameraToSceneCamera();
	void AlignSceneCameraToGameCamera();
	void SetPhysicsSimulationEnabled(bool enabled);
	void RebuildPhysicsWorldFromScene();
	void MarkPhysicsActorDirty(EntityId entityId);
	void CreateDefaultColliderForPrimitive(EntityId entityId, Asset::PrimitiveMeshKind kind);
	[[nodiscard]] EntityId CreatePrimitiveEntity(Asset::PrimitiveMeshKind kind);
	[[nodiscard]] bool ApplyPrimitiveMeshToEntity(EntityId entityId, Asset::PrimitiveMeshKind kind, const Math::Transform& localTransform, bool createDefaultCollider = true);
	void AddComponentToEntity(EntityId entityId, SceneComponentKind kind);
	void RemoveComponentFromEntity(EntityId entityId, SceneComponentKind kind);
	void SetComponentEnabledForEntity(EntityId entityId, SceneComponentKind kind, bool enabled);
	void RenameEntityFromHierarchy(EntityId entityId, std::string_view name);
	void DuplicateEntityFromHierarchy(EntityId entityId);
	void DeleteEntityFromHierarchy(EntityId entityId);
	[[nodiscard]] std::string MakeDuplicateEntityName(EntityId entityId) const;
	void RemoveEntityFromRenderState(EntityId entityId);
	[[nodiscard]] bool CreateTextureResourcesForEntity(EntityId entityId);
	void DestroyTextureResourcesForEntity(EntityId entityId);
	[[nodiscard]] bool RecreateTextureResourcesForEntity(EntityId entityId);
	[[nodiscard]] bool RecreateDynamicTextureResources();
	[[nodiscard]] bool RecreateVulkanEntityDescriptorSets();
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
	void InitializeProjectScene();
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
	Samples::Benchmark::SampleMode m_SampleMode = Samples::Benchmark::SampleMode::ProjectScene;
	Samples::Benchmark::SampleMode m_LastSampleMode = Samples::Benchmark::SampleMode::ProjectScene;
	Samples::Benchmark::BenchmarkRunner m_BenchmarkRunner;
	EngineStartupOptions m_StartupOptions;
	std::optional<Projects::ProjectDescriptor> m_Project;
	std::filesystem::path m_CurrentScenePath;
	uint64_t m_AssetSceneGeneration = 1;
	Editor::EditorLayer m_EditorLayer;
	Asset::AssetFileSystem m_AssetFileSystem;
	Asset::AssetImportService m_AssetImportService;
	Asset::AssetHotReloadService m_AssetHotReloadService;
	Asset::RuntimeAssetRegistry m_RuntimeAssetRegistry;
	Physics::PhysicsWorld m_PhysicsWorld;
	std::unordered_map<EntityId, Math::Transform> m_PhysicsSimulationSnapshot;
	std::vector<std::string> m_AssetLogLines;
	float m_LastDeltaTime = 1.0f / 60.0f;
	bool m_SceneCameraControlActive = false;
	bool m_SceneDirty = false;
	bool m_PhysicsSimulationEnabled = false;

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
