#pragma once

#include "GameApp.h"
#include "Asset\StaticMesh.h"
#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/GraphicsCommon.h"
#include "Math/Camera.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
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
	[[nodiscard]] bool LoadDiffuseTextureImage();
	[[nodiscard]] bool CreateTextureResources();
	void DestroyTextureResources();
	void UpdateCameraBuffer();
	void UpdateAnimatedMesh(float deltaTime);

	// UI 업데이트
	void UpdateRendererMenuState();
	void ResetFpsCounter();
	void UpdateWindowTitleWithFps();

private:
	// 렌더링 리소스
	IGraphicsDevice* m_Device = nullptr;
	ICommandList* m_CmdList = nullptr;
	IBuffer* m_VertexBuffer = nullptr;
	IBuffer* m_IndexBuffer = nullptr;
	IBuffer* m_CameraBuffer = nullptr;
	HWND m_hRenderWnd = nullptr;
	Camera m_Camera;
	std::unique_ptr<Asset::StaticMeshAsset> m_StaticMeshAsset;
	float m_AnimationTimeSeconds = 0.0f;
	std::filesystem::path m_DiffuseTexturePath;
	std::vector<unsigned char> m_DiffuseTexturePixels;
	int m_DiffuseTextureWidth = 1;
	int m_DiffuseTextureHeight = 1;

	// 현재 API 상태
	GraphicsAPI m_CurrentApi = GraphicsAPI::Vulkan;
	std::wstring m_WindowTitleBase = L"EnginePlatformer - Vulkan";

	// FPS 카운팅
	uint32_t m_FrameCount = 0;
	std::chrono::steady_clock::time_point m_LastFpsUpdate = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point m_RenderStartTime = std::chrono::steady_clock::now();

	// DirectX12 전용 리소스
	struct Dx12TriangleResources
	{
		Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource> DiffuseTexture;
		Microsoft::WRL::ComPtr<ID3D12Resource> DiffuseTextureUpload;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
	} m_Dx12Triangle;

	// Vulkan 전용 리소스
	struct VulkanTriangleResources
	{
		VkShaderModule VertexShader = nullptr;
		VkShaderModule PixelShader = nullptr;
		VkDescriptorSetLayout DescriptorSetLayout = nullptr;
		VkDescriptorPool DescriptorPool = nullptr;
		VkDescriptorSet DescriptorSet = nullptr;
		VkImage DiffuseImage = nullptr;
		VkDeviceMemory DiffuseImageMemory = nullptr;
		VkImageView DiffuseImageView = nullptr;
		VkSampler DiffuseSampler = nullptr;
		VkPipelineLayout PipelineLayout = nullptr;
		VkPipeline Pipeline = nullptr;
		bool IsValid = false;
	} m_VulkanTriangle;
};
