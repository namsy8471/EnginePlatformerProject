#pragma once

#include "GameApp.h"
#include "Scene.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/GraphicsCommon.h"
#include "Math/Camera.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wrl.h>

// DirectX12와 Vulkan 전방 선언
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D12DescriptorHeap;
using VkShaderModule = struct VkShaderModule_T*;
using VkPipelineLayout = struct VkPipelineLayout_T*;
using VkPipeline = struct VkPipeline_T*;
using VkDescriptorSetLayout = struct VkDescriptorSetLayout_T*;
using VkDescriptorPool = struct VkDescriptorPool_T*;
using VkDescriptorSet = struct VkDescriptorSet_T*;
using VkImage = struct VkImage_T*;
using VkImageView = struct VkImageView_T*;
using VkSampler = struct VkSampler_T*;
using VkDeviceMemory = struct VkDeviceMemory_T*;

class Engine : public GameApp
{
public:
	enum class RenderMode
	{
		Forward,
		Deferred,
		ForwardPlus
	};

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
	IGraphicsDevice* m_Device = nullptr;
	ICommandList* m_CmdList = nullptr;
	IBuffer* m_VertexBuffer = nullptr;
	IBuffer* m_IndexBuffer = nullptr;
	IBuffer* m_CameraBuffer = nullptr;
	HWND m_hRenderWnd = nullptr;
	Camera m_Camera;
	Scene m_Scene;
	EntityId m_SpiderEntity = InvalidEntityId;
	std::vector<EntityId> m_RenderEntities;
	std::unordered_set<EntityId> m_TransparentEntities;
	float m_AnimationTimeSeconds = 0.0f;

	// 현재 API 상태
	GraphicsAPI m_CurrentApi = GraphicsAPI::Vulkan;
	RenderMode m_RenderMode = RenderMode::Forward;
	std::wstring m_WindowTitleBase = L"EnginePlatformer - Vulkan - Forward";
	std::vector<bool> m_PrimaryMaterialTransparency;
	std::unordered_map<EntityId, std::vector<bool>> m_EntityMaterialTransparency;

	// FPS 카운팅
	uint32_t m_FrameCount = 0;
	std::chrono::steady_clock::time_point m_LastFpsUpdate = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point m_RenderStartTime = std::chrono::steady_clock::now();

	// DirectX12 전용 리소스
	struct Dx12TriangleResources
	{
		struct MaterialTexture
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
			Microsoft::WRL::ComPtr<ID3D12Resource> TextureUpload;
		};

		Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> TransparentPipelineState;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
		std::vector<MaterialTexture> MaterialTextures;
	} m_Dx12Triangle;

	struct Dx12ImGuiResources
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
	} m_Dx12ImGui;

	// Vulkan 전용 리소스
	struct VulkanTriangleResources
	{
		struct MaterialTexture
		{
			VkImage Image = nullptr;
			VkDeviceMemory ImageMemory = nullptr;
			VkImageView ImageView = nullptr;
			VkSampler Sampler = nullptr;
		};

		VkShaderModule VertexShader = nullptr;
		VkShaderModule PixelShader = nullptr;
		VkDescriptorSetLayout DescriptorSetLayout = nullptr;
		VkDescriptorPool DescriptorPool = nullptr;
		std::vector<VkDescriptorSet> DescriptorSets;
		std::vector<MaterialTexture> MaterialTextures;
		VkPipelineLayout PipelineLayout = nullptr;
		VkPipeline Pipeline = nullptr;
		VkPipeline TransparentPipeline = nullptr;
		bool IsValid = false;
	} m_VulkanTriangle;

	struct VulkanImGuiResources
	{
		VkDescriptorPool DescriptorPool = nullptr;
	} m_VulkanImGui;

	bool m_ImGuiInitialized = false;
	bool m_ShowImGuiDemoWindow = false;
};
