#include "Engine.h"
#include "Animation/AnimationSystem.h"
#include "Input/InputSystem.h"
#include "Scene/PickingSystem.h"
#include "App/Win32/Resource.h"
#include "Rendering/Debug/ImGuiDebugLayer.h"
#include "Rendering/Resources/MaterialTextureSystem.h"
#include "Rendering/Systems/RenderSystem.h"
#include "Samples/Spider/SpiderSampleScene.h"
#include "Utilities/ShaderUtils.h"

#include "Rendering/Backends/DirectX12/DX12Buffer.h"
#include "Rendering/Backends/DirectX12/DX12Device.h"
#include "Rendering/Backends/DirectX12/d3dx12.h"
#include "Rendering/Backends/Vulkan/VulkanBuffer.h"
#include "Rendering/Backends/Vulkan/VulkanDevice.h"

#include <d3dcompiler.h>
#include <glslang/Include/glslang_c_interface.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#if __has_include(<dstorage.h>)
#include <dstorage.h>
#pragma comment(lib, "dstoragecore.lib")
#define ENGINE_HAS_DIRECTSTORAGE_SDK 1
#else
#define ENGINE_HAS_DIRECTSTORAGE_SDK 0
#endif

#include <array>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	// 렌더 윈도우 클래스 등록 및 관리
	LRESULT CALLBACK RenderWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		{
			return 1;
		}

		InputSystem::Get().ProcessMessage(msg, wParam, lParam);

		if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN)
		{
			SetFocus(hWnd);
		}

		UNREFERENCED_PARAMETER(wParam);
		UNREFERENCED_PARAMETER(lParam);

		if (msg == WM_ERASEBKGND)
		{
			return 1;
		}

		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	[[nodiscard]] constexpr std::wstring_view GetRenderWindowClassName() noexcept
	{
		return L"EngineRenderWindowClass";
	}

   [[nodiscard]] bool EnsureRenderWindowClassRegistered(HINSTANCE hInstance)
	{
		static bool isRegistered = false;
		if (isRegistered)
		{
			return true;
		}

		const WNDCLASSEXW windowClass = {
			.cbSize = sizeof(WNDCLASSEXW),
			.style = CS_HREDRAW | CS_VREDRAW,
			.lpfnWndProc = RenderWindowProc,
			.hInstance = hInstance,
			.hCursor = LoadCursor(nullptr, IDC_ARROW),
			.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)),
			.lpszClassName = GetRenderWindowClassName().data()
		};

		if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		{
			return false;
		}

		isRegistered = true;
		return true;
	}

	[[nodiscard]] bool IsDirectStorageTextureCandidate(const CpuMaterialTexture& materialTexture) noexcept
	{
		if (materialTexture.Path.empty())
		{
			return false;
		}

		const std::filesystem::path extension = materialTexture.Path.extension();
		return _wcsicmp(extension.c_str(), L".dds") == 0;
	}

	// 삼각형 정점 데이터
	struct TriangleVertex
	{
		float Position[2];
		float Color[4];
	};

	constexpr std::array<TriangleVertex, 3> kDx12TriangleVertices = {{
		{ { 0.0f, 0.5f },  { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { -0.5f, -0.5f },{ 0.0f, 0.0f, 1.0f, 1.0f } }
	}};

	constexpr std::array<TriangleVertex, 3> kVulkanTriangleVertices = {{
		{ { 0.0f, 0.5f },  { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { -0.5f, -0.5f },{ 0.0f, 0.0f, 1.0f, 1.0f } }
	}};

	struct BenchmarkGeometry
	{
		std::vector<Asset::StaticMeshVertex> Vertices;
		std::vector<uint32_t> Indices;
	};

	[[nodiscard]] Asset::StaticMeshVertex MakeBenchmarkVertex(float x, float y, const DirectX::XMFLOAT4& color)
	{
		Asset::StaticMeshVertex vertex;
		vertex.Position = { x, y, 0.0f };
		vertex.Normal = { 0.0f, 0.0f, -1.0f };
		vertex.TexCoord = { -1.0f, -1.0f };
		vertex.Color = color;
		return vertex;
	}

	void AddBenchmarkTriangle(
		BenchmarkGeometry& geometry,
		float ax,
		float ay,
		float bx,
		float by,
		float cx,
		float cy,
		const DirectX::XMFLOAT4& color)
	{
		const uint32_t baseIndex = static_cast<uint32_t>(geometry.Vertices.size());
		geometry.Vertices.push_back(MakeBenchmarkVertex(ax, ay, color));
		geometry.Vertices.push_back(MakeBenchmarkVertex(bx, by, color));
		geometry.Vertices.push_back(MakeBenchmarkVertex(cx, cy, color));
		geometry.Indices.push_back(baseIndex);
		geometry.Indices.push_back(baseIndex + 1);
		geometry.Indices.push_back(baseIndex + 2);
	}

	void AddBenchmarkQuad(BenchmarkGeometry& geometry, float minX, float minY, float maxX, float maxY, const DirectX::XMFLOAT4& color)
	{
		const uint32_t baseIndex = static_cast<uint32_t>(geometry.Vertices.size());
		geometry.Vertices.push_back(MakeBenchmarkVertex(minX, minY, color));
		geometry.Vertices.push_back(MakeBenchmarkVertex(minX, maxY, color));
		geometry.Vertices.push_back(MakeBenchmarkVertex(maxX, maxY, color));
		geometry.Vertices.push_back(MakeBenchmarkVertex(maxX, minY, color));
		geometry.Indices.insert(geometry.Indices.end(), {
			baseIndex, baseIndex + 1, baseIndex + 2,
			baseIndex, baseIndex + 2, baseIndex + 3
		});
	}

	[[nodiscard]] const BenchmarkGeometry& GetBenchmarkPrimitiveGeometry()
	{
		static const BenchmarkGeometry geometry = []()
		{
			BenchmarkGeometry result;
			AddBenchmarkQuad(result, -0.35f, -0.35f, 0.35f, 0.35f, { 0.9f, 0.95f, 1.0f, 1.0f });
			return result;
		}();
		return geometry;
	}

	[[nodiscard]] const BenchmarkGeometry& GetBenchmarkSpiderGlyphGeometry()
	{
		static const BenchmarkGeometry geometry = []()
		{
			BenchmarkGeometry result;
			const DirectX::XMFLOAT4 bodyColor = { 0.08f, 0.07f, 0.06f, 1.0f };
			const DirectX::XMFLOAT4 legColor = { 0.03f, 0.03f, 0.03f, 1.0f };

			AddBenchmarkQuad(result, -0.22f, -0.16f, 0.22f, 0.16f, bodyColor);
			AddBenchmarkTriangle(result, -0.18f, 0.10f, -0.72f, 0.36f, -0.22f, 0.02f, legColor);
			AddBenchmarkTriangle(result, -0.16f, 0.04f, -0.78f, 0.10f, -0.18f, -0.04f, legColor);
			AddBenchmarkTriangle(result, -0.16f, -0.04f, -0.72f, -0.36f, -0.18f, -0.10f, legColor);
			AddBenchmarkTriangle(result, 0.18f, 0.10f, 0.72f, 0.36f, 0.22f, 0.02f, legColor);
			AddBenchmarkTriangle(result, 0.16f, 0.04f, 0.78f, 0.10f, 0.18f, -0.04f, legColor);
			AddBenchmarkTriangle(result, 0.16f, -0.04f, 0.72f, -0.36f, 0.18f, -0.10f, legColor);
			return result;
		}();
		return geometry;
	}

	[[nodiscard]] bool ShouldUseFullSpiderMeshForBenchmark(uint32_t instanceCount) noexcept
	{
		return instanceCount <= 10000;
	}

	// 셰이더 파일 경로
	[[nodiscard]] constexpr std::string_view GetDx12ShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/Triangle.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Triangle.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Triangle.frag";
	}

	[[nodiscard]] constexpr std::string_view GraphicsApiToString(GraphicsAPI api) noexcept
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

	void LogEngineTrace(std::string_view message)
	{
		constexpr std::string_view prefix = "[Engine][TRACE] ";
		constexpr std::string_view suffix = "\n";

		std::string buffer;
		buffer.reserve(prefix.size() + message.size() + suffix.size());
		buffer.append(prefix);
		buffer.append(message);
		buffer.append(suffix);
		OutputDebugStringA(buffer.c_str());
	}

	void CheckImGuiVulkanResult(VkResult result)
	{
		if (result != VK_SUCCESS)
		{
			throw std::runtime_error("ImGui Vulkan backend call failed.");
		}
	}

	[[nodiscard]] bool IsDirectStorageRuntimeAvailable() noexcept
	{
#if ENGINE_HAS_DIRECTSTORAGE_SDK
		ComPtr<IDStorageFactory> directStorageFactory;
		if (SUCCEEDED(DStorageGetFactory(IID_PPV_ARGS(&directStorageFactory))))
		{
			return true;
		}
#endif

		static int cachedAvailability = -1;
		if (cachedAvailability >= 0)
		{
			return cachedAvailability == 1;
		}

		HMODULE directStorageModule = LoadLibraryW(L"dstoragecore.dll");
		if (directStorageModule != nullptr)
		{
			FreeLibrary(directStorageModule);
			cachedAvailability = 1;
			return true;
		}

		cachedAvailability = 0;
		return false;
	}

#if ENGINE_HAS_DIRECTSTORAGE_SDK
	struct DirectStorageDx12Context
	{
		ComPtr<IDStorageFactory> Factory;
		ComPtr<IDStorageQueue> FileQueue;
		bool IsInitialized = false;
	};

	DirectStorageDx12Context& GetDirectStorageDx12Context() noexcept
	{
		static DirectStorageDx12Context context;
		return context;
	}

	[[nodiscard]] bool EnsureDirectStorageQueueForDx12(ID3D12Device* d3dDevice) noexcept
	{
		if (!d3dDevice)
		{
			return false;
		}

		auto& context = GetDirectStorageDx12Context();
		if (context.IsInitialized)
		{
			return context.FileQueue != nullptr;
		}

		if (FAILED(DStorageGetFactory(IID_PPV_ARGS(&context.Factory))))
		{
			context.IsInitialized = true;
			return false;
		}

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = 256;
		queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
		queueDesc.Name = "EngineDx12TextureQueue";
		queueDesc.Device = d3dDevice;

		if (FAILED(context.Factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&context.FileQueue))))
		{
			context.Factory.Reset();
			context.IsInitialized = true;
			return false;
		}

		context.IsInitialized = true;
		return true;
	}

	[[nodiscard]] bool IsDirectStorageQueueReadyForDx12() noexcept
	{
		const auto& context = GetDirectStorageDx12Context();
		return context.FileQueue != nullptr;
	}

	void ShutdownDirectStorageForDx12() noexcept
	{
		auto& context = GetDirectStorageDx12Context();
		context.FileQueue.Reset();
		context.Factory.Reset();
		context.IsInitialized = false;
	}
#else
	[[nodiscard]] bool EnsureDirectStorageQueueForDx12(ID3D12Device*) noexcept
	{
		return false;
	}

	[[nodiscard]] bool IsDirectStorageQueueReadyForDx12() noexcept
	{
		return false;
	}

	void ShutdownDirectStorageForDx12() noexcept
	{
	}
#endif

	void LogDirectStorageStateForDx12()
	{
       if (!IsDirectStorageRuntimeAvailable())
		{
			LogEngineTrace("DirectStorage runtime not detected. Falling back to UpdateSubresources upload path.");
			return;
		}

#if ENGINE_HAS_DIRECTSTORAGE_SDK
		LogEngineTrace("DirectStorage runtime + SDK detected. DX12 texture upload can be promoted to DirectStorage queue path.");
#else
		LogEngineTrace("DirectStorage runtime detected, but dstorage.h is unavailable in this build environment. Using UpdateSubresources fallback path.");
#endif
	}

	[[nodiscard]] bool IsDirectStorageUploadPathActive() noexcept
	{
#if ENGINE_HAS_DIRECTSTORAGE_SDK
       return IsDirectStorageRuntimeAvailable() && IsDirectStorageQueueReadyForDx12();
#else
		return false;
#endif
	}

	void LogDirectStorageFallbackReasonForTextureUpload(size_t directStorageCandidateCount)
	{
		if (directStorageCandidateCount == 0)
		{
         LogEngineTrace("DirectStorage candidate textures=0 | ActiveUploadPath=UpdateSubresources (dds source not found)");
			return;
		}

		std::string message = "DirectStorage candidate textures=";
		message.append(std::to_string(directStorageCandidateCount));
		message.append(" | ActiveUploadPath=");
		message.append(IsDirectStorageUploadPathActive() ? "DirectStorage" : "UpdateSubresources");

		if (!IsDirectStorageUploadPathActive())
		{
            message.append(" (fallback)");
		}

		LogEngineTrace(message);
	}

	[[nodiscard]] bool IsVulkanDirectStorageEquivalentPathActive() noexcept
	{
		// Vulkan 경로는 DirectStorage API를 직접 사용하지 않으므로 현재는 staging upload가 기본 경로입니다.
		// 추후 전용 비동기 파일 I/O + 전송 큐 스트리밍 파이프라인을 붙이면 여기서 활성 여부를 반환합니다.
		return false;
	}

	void LogVulkanStreamingStateForTextureUpload(size_t directStorageCandidateCount)
	{
		std::string message = "Vulkan texture streaming path=";
		message.append(IsVulkanDirectStorageEquivalentPathActive() ? "AsyncStreaming" : "StagingUpload");
		message.append(" | ddsCandidates=");
		message.append(std::to_string(directStorageCandidateCount));
		LogEngineTrace(message);
	}
}

Engine::Engine(HINSTANCE hInstance) : GameApp(hInstance)
{
}

Engine::~Engine()
{
	ShutdownGraphics();
	DestroyRenderWindow();
	glslang_finalize_process();
}

EntityId Engine::CreateEntity(std::string_view name)
{
	return m_Scene.CreateEntity(name);
}

TransformComponent* Engine::GetTransformComponent(EntityId entityId)
{
	return m_Scene.GetTransformComponent(entityId);
}

const TransformComponent* Engine::GetTransformComponent(EntityId entityId) const
{
	return m_Scene.GetTransformComponent(entityId);
}

Asset::StaticMeshAsset* Engine::GetMeshAsset(EntityId entityId)
{
	return m_Scene.GetMeshAsset(entityId);
}

const Asset::StaticMeshAsset* Engine::GetMeshAsset(EntityId entityId) const
{
	return m_Scene.GetMeshAsset(entityId);
}

std::vector<CpuMaterialTexture>* Engine::GetMaterialTextures(EntityId entityId)
{
	return m_Scene.GetMaterialTextures(entityId);
}

const std::vector<CpuMaterialTexture>* Engine::GetMaterialTextures(EntityId entityId) const
{
	return m_Scene.GetMaterialTextures(entityId);
}

const std::string* Engine::GetEntityName(EntityId entityId) const
{
	return m_Scene.GetEntityName(entityId);
}

bool Engine::IsMaterialTransparent(EntityId entityId, size_t materialIndex) const
{
	return m_RenderState.IsMaterialTransparent(m_Scene, entityId, materialIndex);
}

void Engine::UploadEntityGeometry(EntityId entityId)
{
	const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entityId);
	if (!meshAsset || !m_StaticMeshRenderer.VertexBuffer || !m_StaticMeshRenderer.IndexBuffer)
	{
		return;
	}

	void* mappedVertexData = nullptr;
	m_StaticMeshRenderer.VertexBuffer->Map(&mappedVertexData);
	std::memcpy(mappedVertexData, meshAsset->Vertices.data(), meshAsset->Vertices.size() * sizeof(Asset::StaticMeshVertex));
	m_StaticMeshRenderer.VertexBuffer->Unmap();

	void* mappedIndexData = nullptr;
	m_StaticMeshRenderer.IndexBuffer->Map(&mappedIndexData);
	std::memcpy(mappedIndexData, meshAsset->Indices.data(), meshAsset->Indices.size() * sizeof(uint32_t));
	m_StaticMeshRenderer.IndexBuffer->Unmap();
}

bool Engine::Init()
{
	if (!GameApp::Init())
	{
		return false;
	}

	if (!glslang_initialize_process())
	{
		return false;
	}

	if (!LoadSpiderStaticMesh())
	{
		return false;
	}

	if (!SwitchGraphicsAPI(m_Graphics.CurrentApi))
	{
		return false;
	}

	FramePrimaryRenderableCamera();
	return true;
}

LRESULT Engine::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
	{
		return 1;
	}

	if (msg == WM_COMMAND)
	{
		switch (LOWORD(wParam))
		{
		case IDM_RENDERER_DX12:
			if (m_Graphics.CurrentApi != GraphicsAPI::DirectX12)
			{
				const GraphicsAPI previousApi = m_Graphics.CurrentApi;
				if (!SwitchGraphicsAPI(GraphicsAPI::DirectX12))
				{
					if (!SwitchGraphicsAPI(previousApi))
					{
						LogEngineTrace("SwitchGraphicsAPI failed to restore previous API after DX12 switch failure.");
					}
				}
			}
			return 0;

		case IDM_RENDERER_VULKAN:
			if (m_Graphics.CurrentApi != GraphicsAPI::Vulkan)
			{
				const GraphicsAPI previousApi = m_Graphics.CurrentApi;
				if (!SwitchGraphicsAPI(GraphicsAPI::Vulkan))
				{
					if (!SwitchGraphicsAPI(previousApi))
					{
						LogEngineTrace("SwitchGraphicsAPI failed to restore previous API after Vulkan switch failure.");
					}
				}
			}
			return 0;

		case IDM_RENDERMODE_FORWARD:
			SwitchRenderMode(RenderMode::Forward);
			return 0;

		case IDM_RENDERMODE_DEFERRED:
			SwitchRenderMode(RenderMode::Deferred);
			return 0;

		case IDM_RENDERMODE_FORWARD_PLUS:
			SwitchRenderMode(RenderMode::ForwardPlus);
			return 0;
		}
	}

	return GameApp::MsgProc(hWnd, msg, wParam, lParam);
}

void Engine::Update(float deltaTime)
{
	if (m_LastSampleMode != m_SampleMode)
	{
		FramePrimaryRenderableCamera();
		m_LastSampleMode = m_SampleMode;
	}

	m_Camera.Update(deltaTime, m_hRenderWnd ? m_hRenderWnd : m_hMainWnd);
	if (m_SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		m_BenchmarkRunner.Update(deltaTime);
		return;
	}

	UpdateAnimatedMesh(deltaTime);
	UpdateObjectPicking();
}

void Engine::Render()
{
	if (!m_Graphics.Device || !m_Graphics.CommandList)
	{
		return;
	}

	// 커맨드 리스트 리셋 및 기본 설정
	m_Graphics.CommandList->Reset();
	m_Graphics.CommandList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
	m_Graphics.CommandList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

	// 백버퍼를 렌더타겟 상태로 전환
	IGpuResource* backBuffer = m_Graphics.Device->GetBackBufferResource();
	m_Graphics.CommandList->ResourceBarrier(backBuffer, ResourceState::Present, ResourceState::RenderTarget);

	// 렌더타겟 설정
	void* rtvHandle = m_Graphics.Device->GetCurrentBackBufferRTV();
	void* dsvHandle = m_Graphics.Device->GetDepthStencilView();
	m_Graphics.CommandList->SetRenderTargets(rtvHandle, dsvHandle);

	// 애니메이션 효과를 위한 시간 기반 색상 계산
	const auto now = std::chrono::steady_clock::now();
	const std::chrono::duration<float> elapsed = now - m_RenderStartTime;
	const float color = (std::sinf(elapsed.count() * 2.0f) + 1.0f) * 0.5f;

	// 화면 클리어
	const float clearColor[4] = { color, 0.2f, 0.4f, 1.0f };
	m_Graphics.CommandList->ClearRenderTarget(rtvHandle, clearColor);
	m_Graphics.CommandList->ClearDepthStencil(dsvHandle, 1.0f, 0);

	// Benchmark 모드에서도 기준 씬 오브젝트를 계속 렌더링해 카메라가 볼 대상을 잃지 않게 합니다.
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer);
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer);

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		DrawDx12Triangle();
	}
	else
	{
		DrawVulkanTriangle();
	}

	DrawBenchmarkInstances();
	RenderImGui();

	// 백버퍼를 Present 상태로 전환
	m_Graphics.CommandList->ResourceBarrier(backBuffer, ResourceState::RenderTarget, ResourceState::Present);
	m_Graphics.CommandList->Close();

	// 커맨드 리스트 실행 및 화면 출력
	m_Graphics.Device->ExecuteCommandList(m_Graphics.CommandList);
	m_Graphics.Device->Present();
	m_Graphics.Device->MoveToNextFrame();

	UpdateWindowTitleWithFps();
}

void Engine::OnResize()
{
	ResizeRenderWindow();
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_BenchmarkRunner.SetSpawnView(m_Camera);

	if (m_Graphics.Device)
	{
		DestroyImGuiResources();
		m_Graphics.Device->Resize(m_ClientWidth, m_ClientHeight);

		// Vulkan 파이프라인은 렌더패스 호환성을 유지하더라도 리사이즈 시 재생성해 두는 편이 학습상 명확합니다.
		if (m_Graphics.CurrentApi == GraphicsAPI::Vulkan)
		{
			DestroyVulkanTriangleResources();
			if (!CreateVulkanTriangleResources())
			{
				LogEngineTrace("OnResize failed to recreate Vulkan triangle resources.");
			}
		}

		if (!CreateImGuiResources())
		{
			LogEngineTrace("OnResize failed to recreate ImGui resources.");
		}
	}
}

bool Engine::SwitchGraphicsAPI(GraphicsAPI api)
{
	std::string switchMessage = "SwitchGraphicsAPI: ";
	switchMessage.append(GraphicsApiToString(m_Graphics.CurrentApi));
	switchMessage.append(" -> ");
	switchMessage.append(GraphicsApiToString(api));
	LogEngineTrace(switchMessage);

	ShutdownGraphics();
	DestroyRenderWindow();

	if (!CreateRenderWindow())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during render window creation.");
		return false;
	}

	m_Graphics.CurrentApi = api;
	m_Graphics.Device = IGraphicsDevice::Create(api, m_hRenderWnd, m_ClientWidth, m_ClientHeight);
	if (!m_Graphics.Device || !m_Graphics.Device->Init())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during device initialization.");
		ShutdownGraphics();
		MessageBoxW(m_hMainWnd, L"그래픽 디바이스 초기화에 실패했습니다.", L"Graphics API Error", MB_OK | MB_ICONERROR);
		return false;
	}

	if (api == GraphicsAPI::DirectX12)
	{
		LogDirectStorageStateForDx12();
	}
	else
	{
		LogEngineTrace("Vulkan backend selected. Using Vulkan staging upload path (DirectStorage API is DX12-only).");
	}

	m_Graphics.CommandList = m_Graphics.Device->CreateCommandList();
	if (!CreateTriangleVertexBuffer())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during vertex buffer creation.");
		ShutdownGraphics();
		return false;
	}

	if (!CreateIndexBuffer())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during index buffer creation.");
		ShutdownGraphics();
		return false;
	}

	if (!CreateCameraBuffer())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during camera buffer creation.");
		ShutdownGraphics();
		return false;
	}

	if (!CreateTextureResources())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during texture resource creation.");
		ShutdownGraphics();
		return false;
	}

	bool triangleInitResult = true;
	if (api == GraphicsAPI::DirectX12)
	{
		triangleInitResult = CreateDx12TriangleResources();
	}
	else
	{
		triangleInitResult = CreateVulkanTriangleResources();
	}

	if (!triangleInitResult)
	{
		LogEngineTrace("SwitchGraphicsAPI failed during pipeline resource creation.");
		ShutdownGraphics();
		return false;
	}

	if (!CreateImGuiResources())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during ImGui initialization.");
		ShutdownGraphics();
		return false;
	}

	RebuildWindowTitleBase();
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_RenderStartTime = std::chrono::steady_clock::now();
	ResetFpsCounter();
	UpdateRendererMenuState();
	LogEngineTrace("SwitchGraphicsAPI completed successfully.");

	return true;
}

void Engine::SwitchRenderMode(RenderMode renderMode)
{
	if (m_RenderMode == renderMode)
	{
		return;
	}

	m_RenderMode = renderMode;
	RebuildWindowTitleBase();
	ResetFpsCounter();
	UpdateRendererMenuState();

	std::string modeLogMessage = "Render mode switched to ";
	modeLogMessage.append(RenderModeToString(m_RenderMode));
	LogEngineTrace(modeLogMessage);
}

void Engine::RebuildWindowTitleBase()
{
	const std::wstring_view apiName = m_Graphics.CurrentApi == GraphicsAPI::DirectX12 ? L"DirectX12" : L"Vulkan";
	m_WindowTitleBase.clear();
	m_WindowTitleBase.reserve(64);
	m_WindowTitleBase.append(L"EnginePlatformer - ");
	m_WindowTitleBase.append(apiName);
	m_WindowTitleBase.append(L" - ");
	m_WindowTitleBase.append(RenderModeToWideString(m_RenderMode));
}

void Engine::ShutdownGraphics()
{
	if (m_Graphics.Device)
	{
		LogEngineTrace("ShutdownGraphics waiting for GPU.");
		m_Graphics.Device->WaitForGPU();
	}

	ShutdownDirectStorageForDx12();

	DestroyImGuiResources();

	DestroyTextureResources();
	DestroyDx12TriangleResources();
	DestroyVulkanTriangleResources();
	DestroyTextureResources();

	delete m_StaticMeshRenderer.VertexBuffer;
	m_StaticMeshRenderer.VertexBuffer = nullptr;

	delete m_StaticMeshRenderer.IndexBuffer;
	m_StaticMeshRenderer.IndexBuffer = nullptr;

	delete m_StaticMeshRenderer.CameraBuffer;
	m_StaticMeshRenderer.CameraBuffer = nullptr;

	delete m_Graphics.CommandList;
	m_Graphics.CommandList = nullptr;

	delete m_Graphics.Device;
	m_Graphics.Device = nullptr;

	LogEngineTrace("ShutdownGraphics completed.");
}

bool Engine::CreateRenderWindow()
{
	if (m_hRenderWnd && !IsWindow(m_hRenderWnd))
	{
		m_hRenderWnd = nullptr;
	}

	if (m_hRenderWnd)
	{
		return true;
	}

	if (!EnsureRenderWindowClassRegistered(m_hAppInst))
	{
		MessageBoxW(m_hMainWnd, L"렌더링용 윈도우 클래스를 등록하지 못했습니다.", L"Window Error", MB_OK | MB_ICONERROR);
		return false;
	}

	// API 전환마다 새 child HWND를 만들어 DX12 flip-model swapchain과 Vulkan surface가 같은 HWND를 재사용하지 않도록 합니다.
	m_hRenderWnd = CreateWindowExW(
		0,
		GetRenderWindowClassName().data(),
		L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		0,
		0,
		m_ClientWidth,
		m_ClientHeight,
		m_hMainWnd,
		nullptr,
		m_hAppInst,
		nullptr);

	if (!m_hRenderWnd)
	{
		MessageBoxW(m_hMainWnd, L"렌더링용 윈도우를 생성하지 못했습니다.", L"Window Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ResizeRenderWindow();
	return true;
}

void Engine::DestroyRenderWindow()
{
	if (!m_hRenderWnd)
	{
		return;
	}

	if (!IsWindow(m_hRenderWnd))
	{
		m_hRenderWnd = nullptr;
		return;
	}

	DestroyWindow(m_hRenderWnd);
	m_hRenderWnd = nullptr;
}

void Engine::ResizeRenderWindow()
{
	if (!m_hRenderWnd)
	{
		return;
	}

	if (!IsWindow(m_hRenderWnd))
	{
		m_hRenderWnd = nullptr;
		return;
	}

	// 메인 윈도우는 유지하고 렌더링 백엔드가 붙는 child HWND만 크기를 맞춰 백엔드 출력 대상을 분리합니다.
	MoveWindow(m_hRenderWnd, 0, 0, m_ClientWidth, m_ClientHeight, TRUE);
}

bool Engine::LoadSpiderStaticMesh()
{
	Samples::Spider::LoadResult loadResult = {};
	if (!Samples::Spider::Load(m_Scene, m_RenderState, m_Camera, loadResult))
	{
		MessageBoxW(m_hMainWnd, L"Spider 샘플 씬을 로드하지 못했습니다.", L"Asset Error", MB_OK | MB_ICONERROR);
		return false;
	}

	m_SpiderEntity = loadResult.SpiderEntity;
	return m_SpiderEntity != InvalidEntityId;
}

bool Engine::LoadMaterialTextures()
{
	return Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(
		m_Scene,
		m_RenderState,
		m_Scene.GetPrimaryRenderableEntity(),
		LogEngineTrace);
}

bool Engine::CreateTextureResources()
{
	const auto* materialTextures = GetMaterialTextures(m_Scene.GetPrimaryRenderableEntity());
	if (!materialTextures)
	{
		return false;
	}

	const size_t textureCount = (std::max)(static_cast<size_t>(1), materialTextures->size());

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device);
		if (!dx12Device)
		{
			return false;
		}

		m_StaticMeshRenderer.Dx12.MaterialTextures.clear();
		m_StaticMeshRenderer.Dx12.MaterialTextures.resize(textureCount);
		m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Reset();

        const bool directStorageRuntimeAvailable = IsDirectStorageRuntimeAvailable();
		if (directStorageRuntimeAvailable)
		{
			if (!EnsureDirectStorageQueueForDx12(dx12Device->GetD3DDevice()))
			{
				LogEngineTrace("DirectStorage runtime is available, but DX12 texture queue initialization failed.");
			}
		}

		const bool directStorageUploadPathActive = IsDirectStorageUploadPathActive();
		size_t directStorageCandidateCount = 0;

		ComPtr<ID3D12CommandAllocator> commandAllocator;
		ComPtr<ID3D12GraphicsCommandList> commandList;
		if (FAILED(dx12Device->GetD3DDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator))) ||
			FAILED(dx12Device->GetD3DDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
		{
			return false;
		}

		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			const auto& materialTexture = (*materialTextures)[textureIndex];
			auto& dx12MaterialTexture = m_StaticMeshRenderer.Dx12.MaterialTextures[textureIndex];
			const UINT64 rowPitch = static_cast<UINT64>(materialTexture.Width) * 4;
			const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				DXGI_FORMAT_R8G8B8A8_UNORM,
				static_cast<UINT64>(materialTexture.Width),
				static_cast<UINT>(materialTexture.Height));
			const CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

			if (FAILED(dx12Device->GetD3DDevice()->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&textureDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&dx12MaterialTexture.Texture))))
			{
				return false;
			}

			const UINT64 uploadBufferSize = GetRequiredIntermediateSize(dx12MaterialTexture.Texture.Get(), 0, 1);
			const CD3DX12_HEAP_PROPERTIES uploadHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
			if (FAILED(dx12Device->GetD3DDevice()->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&uploadBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&dx12MaterialTexture.TextureUpload))))
			{
				return false;
			}

			D3D12_SUBRESOURCE_DATA textureData = {};
			textureData.pData = materialTexture.Pixels.data();
			textureData.RowPitch = static_cast<LONG_PTR>(rowPitch);
			textureData.SlicePitch = static_cast<LONG_PTR>(rowPitch * static_cast<UINT64>(materialTexture.Height));

         if (IsDirectStorageTextureCandidate(materialTexture))
			{
                ++directStorageCandidateCount;
			}
			UpdateSubresources(commandList.Get(), dx12MaterialTexture.Texture.Get(), dx12MaterialTexture.TextureUpload.Get(), 0, 0, 1, &textureData);

			auto textureTransitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
				dx12MaterialTexture.Texture.Get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			commandList->ResourceBarrier(1, &textureTransitionBarrier);
		}

		commandList->Close();
		ID3D12CommandList* commandLists[] = { commandList.Get() };
		dx12Device->GetCommandQueue()->ExecuteCommandLists(1, commandLists);
		dx12Device->WaitForGPU();

		const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {
			.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			.NumDescriptors = static_cast<UINT>(textureCount),
			.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		};
		if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.ShaderResourceHeap))))
		{
			return false;
		}

		const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpuHandle = m_StaticMeshRenderer.Dx12.ShaderResourceHeap->GetCPUDescriptorHandleForHeapStart();
		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			dx12Device->GetD3DDevice()->CreateShaderResourceView(
				m_StaticMeshRenderer.Dx12.MaterialTextures[textureIndex].Texture.Get(),
				&srvDesc,
				cpuHandle);
			cpuHandle.ptr += descriptorSize;
		}

     if (directStorageRuntimeAvailable)
		{
			LogDirectStorageFallbackReasonForTextureUpload(directStorageCandidateCount);

			if (!directStorageUploadPathActive && directStorageCandidateCount > 0)
			{
				LogEngineTrace("DirectStorage queue is unavailable. Keeping UpdateSubresources fallback for texture upload.");
			}

			if (directStorageUploadPathActive && directStorageCandidateCount == 0)
			{
				LogEngineTrace("DirectStorage is active but no .dds texture source is available. Keeping UpdateSubresources fallback.");
			}
		}

		return true;
	}

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
	if (!vulkanDevice)
	{
		return false;
	}

	// Vulkan 경로는 material 수만큼 sampled image를 만들고, 각 material texture를 staging buffer를 통해
	// 개별 VkImage로 업로드합니다. 이후 descriptor set은 CreateVulkanTriangleResources()에서 material별로 생성합니다.
	m_StaticMeshRenderer.Vulkan.MaterialTextures.clear();
	m_StaticMeshRenderer.Vulkan.MaterialTextures.resize(textureCount);
	size_t vulkanDdsCandidateCount = 0;

	for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		const auto& materialTexture = (*materialTextures)[textureIndex];
		auto& vulkanMaterialTexture = m_StaticMeshRenderer.Vulkan.MaterialTextures[textureIndex];

		// Vulkan에서도 dds 텍스처 후보를 집계해 두면, 추후 전용 스트리밍 경로 도입 시
		// 어떤 에셋이 GPU 친화 포맷으로 준비되어 있는지 빠르게 파악할 수 있습니다.
		if (IsDirectStorageTextureCandidate(materialTexture))
		{
			++vulkanDdsCandidateCount;
		}

		VkImageCreateInfo imageCreateInfo = {};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.extent.width = static_cast<uint32_t>(materialTexture.Width);
		imageCreateInfo.extent.height = static_cast<uint32_t>(materialTexture.Height);
		imageCreateInfo.extent.depth = 1;
		imageCreateInfo.mipLevels = 1;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;

		if (vkCreateImage(vulkanDevice->GetVkDevice(), &imageCreateInfo, nullptr, &vulkanMaterialTexture.Image) != VK_SUCCESS)
		{
			return false;
		}

		VkMemoryRequirements memoryRequirements = {};
		vkGetImageMemoryRequirements(vulkanDevice->GetVkDevice(), vulkanMaterialTexture.Image, &memoryRequirements);

		VkMemoryAllocateInfo allocateInfo = {};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = vulkanDevice->FindMemoryTypeForTexture(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (vkAllocateMemory(vulkanDevice->GetVkDevice(), &allocateInfo, nullptr, &vulkanMaterialTexture.ImageMemory) != VK_SUCCESS)
		{
			return false;
		}
		vkBindImageMemory(vulkanDevice->GetVkDevice(), vulkanMaterialTexture.Image, vulkanMaterialTexture.ImageMemory, 0);

		BufferDesc stagingDesc = {};
		stagingDesc.Size = static_cast<uint64_t>(materialTexture.Pixels.size());
		stagingDesc.Stride = 4;
		stagingDesc.Heap = HeapType::Upload;
		stagingDesc.InitialState = ResourceState::GenericRead;
		VulkanBuffer stagingBuffer(vulkanDevice, stagingDesc);
		void* mappedData = nullptr;
		stagingBuffer.Map(&mappedData);
		std::memcpy(mappedData, materialTexture.Pixels.data(), materialTexture.Pixels.size());
		stagingBuffer.Unmap();

		VkCommandBufferAllocateInfo commandBufferAllocateInfo = {};
		commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandBufferAllocateInfo.commandPool = vulkanDevice->GetVkCommandPool();
		commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandBufferAllocateInfo.commandBufferCount = 1;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(vulkanDevice->GetVkDevice(), &commandBufferAllocateInfo, &commandBuffer);

		VkCommandBufferBeginInfo commandBufferBeginInfo = {};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo);

		// Vulkan material texture는 copy 전에 TRANSFER_DST_OPTIMAL로, copy 후에는 SHADER_READ_ONLY_OPTIMAL로 전환합니다.
		// 이렇게 해야 fragment shader에서 material별 sampled image를 안전하게 읽을 수 있습니다.
		VkImageMemoryBarrier toTransferBarrier = {};
		toTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.image = vulkanMaterialTexture.Image;
		toTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toTransferBarrier.subresourceRange.baseMipLevel = 0;
		toTransferBarrier.subresourceRange.levelCount = 1;
		toTransferBarrier.subresourceRange.baseArrayLayer = 0;
		toTransferBarrier.subresourceRange.layerCount = 1;
		toTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransferBarrier);

		VkBufferImageCopy copyRegion = {};
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageExtent = { static_cast<uint32_t>(materialTexture.Width), static_cast<uint32_t>(materialTexture.Height), 1 };
		vkCmdCopyBufferToImage(commandBuffer, stagingBuffer.GetVkBuffer(), vulkanMaterialTexture.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

		VkImageMemoryBarrier toShaderReadBarrier = {};
		toShaderReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toShaderReadBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toShaderReadBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		toShaderReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toShaderReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toShaderReadBarrier.image = vulkanMaterialTexture.Image;
		toShaderReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toShaderReadBarrier.subresourceRange.baseMipLevel = 0;
		toShaderReadBarrier.subresourceRange.levelCount = 1;
		toShaderReadBarrier.subresourceRange.baseArrayLayer = 0;
		toShaderReadBarrier.subresourceRange.layerCount = 1;
		toShaderReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toShaderReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderReadBarrier);

		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		vkQueueSubmit(vulkanDevice->GetVkGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(vulkanDevice->GetVkGraphicsQueue());
		vkFreeCommandBuffers(vulkanDevice->GetVkDevice(), vulkanDevice->GetVkCommandPool(), 1, &commandBuffer);

		VkImageViewCreateInfo imageViewCreateInfo = {};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.image = vulkanMaterialTexture.Image;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewCreateInfo.subresourceRange.levelCount = 1;
		imageViewCreateInfo.subresourceRange.layerCount = 1;
		if (vkCreateImageView(vulkanDevice->GetVkDevice(), &imageViewCreateInfo, nullptr, &vulkanMaterialTexture.ImageView) != VK_SUCCESS)
		{
			return false;
		}

		VkSamplerCreateInfo samplerCreateInfo = {};
		samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
		samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
		samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerCreateInfo.maxAnisotropy = 1.0f;
		samplerCreateInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;
		samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		if (vkCreateSampler(vulkanDevice->GetVkDevice(), &samplerCreateInfo, nullptr, &vulkanMaterialTexture.Sampler) != VK_SUCCESS)
		{
			return false;
		}
	}

	// Vulkan은 현재 staging upload를 사용하지만, dds 후보 수와 함께 경로 상태를 로깅해
	// DX12/DirectStorage와 비교 가능한 진단 정보를 남깁니다.
	LogVulkanStreamingStateForTextureUpload(vulkanDdsCandidateCount);

	return true;
}

void Engine::DestroyTextureResources()
{
	m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Reset();
	m_StaticMeshRenderer.Dx12.MaterialTextures.clear();

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
	if (!vulkanDevice)
	{
		m_StaticMeshRenderer.Vulkan.MaterialTextures.clear();
		return;
	}

	// Vulkan material texture는 material 수만큼 생성되므로 sampler/image view/image/memory를 모두 순회하며 해제합니다.
	// 이 정리는 파이프라인 정리와 분리되어 있어, 리사이즈 시 파이프라인만 재생성하고 텍스처는 재사용할 수 있습니다.
	for (auto& materialTexture : m_StaticMeshRenderer.Vulkan.MaterialTextures)
	{
		if (materialTexture.Sampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(vulkanDevice->GetVkDevice(), materialTexture.Sampler, nullptr);
			materialTexture.Sampler = VK_NULL_HANDLE;
		}

		if (materialTexture.ImageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(vulkanDevice->GetVkDevice(), materialTexture.ImageView, nullptr);
			materialTexture.ImageView = VK_NULL_HANDLE;
		}

		if (materialTexture.Image != VK_NULL_HANDLE)
		{
			vkDestroyImage(vulkanDevice->GetVkDevice(), materialTexture.Image, nullptr);
			materialTexture.Image = VK_NULL_HANDLE;
		}

		if (materialTexture.ImageMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(vulkanDevice->GetVkDevice(), materialTexture.ImageMemory, nullptr);
			materialTexture.ImageMemory = VK_NULL_HANDLE;
		}
	}

	m_StaticMeshRenderer.Vulkan.MaterialTextures.clear();
}

bool Engine::CreateImGuiResources()
{
	if (m_ImGuiInitialized)
	{
		return true;
	}

	HWND imguiWindow = m_hRenderWnd ? m_hRenderWnd : m_hMainWnd;
	if (!imguiWindow || !m_Graphics.Device)
	{
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::StyleColorsDark();

	if (!ImGui_ImplWin32_Init(imguiWindow))
	{
		ImGui::DestroyContext();
		return false;
	}

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device);
		if (!dx12Device)
		{
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = 1;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_Dx12ImGui.ShaderResourceHeap))))
		{
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		if (!ImGui_ImplDX12_Init(
			dx12Device->GetD3DDevice(),
			2,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			m_Dx12ImGui.ShaderResourceHeap.Get(),
			m_Dx12ImGui.ShaderResourceHeap->GetCPUDescriptorHandleForHeapStart(),
			m_Dx12ImGui.ShaderResourceHeap->GetGPUDescriptorHandleForHeapStart()))
		{
			m_Dx12ImGui.ShaderResourceHeap.Reset();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}
	}
	else
	{
		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
		if (!vulkanDevice)
		{
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		const VkDescriptorPoolSize poolSizes[] = {
			{ VK_DESCRIPTOR_TYPE_SAMPLER, 100 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
			{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 }
		};

		// Vulkan ImGui backend는 내부 폰트 텍스처와 사용자 이미지용 descriptor set을 자체적으로 할당하므로,
		// 엔진의 material descriptor pool과 분리된 전용 descriptor pool을 준비해 넘겨야 합니다.
		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 100 * static_cast<uint32_t>(std::size(poolSizes));
		poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
		poolInfo.pPoolSizes = poolSizes;
		if (vkCreateDescriptorPool(vulkanDevice->GetVkDevice(), &poolInfo, nullptr, &m_VulkanImGui.DescriptorPool) != VK_SUCCESS)
		{
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.ApiVersion = VK_API_VERSION_1_0;
		initInfo.Instance = vulkanDevice->GetVkInstance();
		initInfo.PhysicalDevice = vulkanDevice->GetVkPhysicalDevice();
		initInfo.Device = vulkanDevice->GetVkDevice();
		initInfo.QueueFamily = vulkanDevice->GetVkGraphicsQueueFamilyIndex();
		initInfo.Queue = vulkanDevice->GetVkGraphicsQueue();
		initInfo.DescriptorPool = m_VulkanImGui.DescriptorPool;
		initInfo.RenderPass = vulkanDevice->GetVkRenderPass();
		initInfo.MinImageCount = (std::max)(2u, vulkanDevice->GetVkSwapchainImageCount());
		initInfo.ImageCount = (std::max)(2u, vulkanDevice->GetVkSwapchainImageCount());
		initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;
		if (!ImGui_ImplVulkan_Init(&initInfo))
		{
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), m_VulkanImGui.DescriptorPool, nullptr);
			m_VulkanImGui.DescriptorPool = VK_NULL_HANDLE;
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}

		// Vulkan backend는 현재 render pass 형식에 맞는 폰트 texture 리소스를 먼저 준비해야 합니다.
		// 이 엔진은 resize 시 render pass를 다시 만들므로 backend도 재초기화하면서 같은 경로를 다시 탑니다.
		if (!ImGui_ImplVulkan_CreateFontsTexture())
		{
			ImGui_ImplVulkan_Shutdown();
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), m_VulkanImGui.DescriptorPool, nullptr);
			m_VulkanImGui.DescriptorPool = VK_NULL_HANDLE;
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
			return false;
		}
		ImGui_ImplVulkan_DestroyFontsTexture();
	}

	m_ImGuiInitialized = true;
	return true;
}

void Engine::DestroyImGuiResources()
{
	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		if (m_ImGuiInitialized)
		{
			ImGui_ImplDX12_Shutdown();
		}
		m_Dx12ImGui.ShaderResourceHeap.Reset();
	}
	else
	{
		if (m_ImGuiInitialized)
		{
			ImGui_ImplVulkan_Shutdown();
		}

		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
		if (vulkanDevice && m_VulkanImGui.DescriptorPool != VK_NULL_HANDLE)
		{
			// Vulkan backend가 descriptor set을 반납한 뒤, 엔진이 소유한 전용 descriptor pool을 파괴합니다.
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), m_VulkanImGui.DescriptorPool, nullptr);
		}
		m_VulkanImGui.DescriptorPool = VK_NULL_HANDLE;
	}

	if (ImGui::GetCurrentContext() != nullptr)
	{
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}

	m_ImGuiInitialized = false;
}

void Engine::RenderImGui()
{
	if (!m_ImGuiInitialized)
	{
		return;
	}

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		ImGui_ImplDX12_NewFrame();
	}
	else
	{
		ImGui_ImplVulkan_NewFrame();
	}
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	Rendering::ImGuiDebugLayer::DrawDebugWindow(
		m_Graphics.CurrentApi,
		m_RenderMode,
		m_Camera,
		m_Scene,
		m_SampleMode,
		m_BenchmarkRunner,
		m_ShowImGuiDemoWindow,
		[this](RenderMode renderMode)
		{
			SwitchRenderMode(renderMode);
		});

	if (m_SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		m_BenchmarkRunner.DrawViewportOverlay(m_Camera, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
	}

	ImGui::Render();

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto commandList = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
		ID3D12DescriptorHeap* descriptorHeaps[] = { m_Dx12ImGui.ShaderResourceHeap.Get() };
		commandList->SetDescriptorHeaps(1, descriptorHeaps);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
	}
	else
	{
		// Vulkan ImGui draw data는 현재 열린 render pass 안에서 같은 command buffer에 기록해야 합니다.
		// 엔진 렌더 루프는 scene draw 뒤, Present 전 barrier 전에 이 함수를 호출하므로 그 조건을 만족합니다.
		auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
	}
}

void Engine::FramePrimaryRenderableCamera()
{
	EntityId focusEntity = m_Scene.GetPrimaryRenderableEntity();
	if (focusEntity == InvalidEntityId && !m_RenderState.RenderEntities.empty())
	{
		focusEntity = m_RenderState.RenderEntities.front();
	}

	const TransformComponent* transform = GetTransformComponent(focusEntity);
	if (!transform)
	{
		return;
	}

	DirectX::XMFLOAT3 localCenter = { 0.0f, 0.0f, 0.0f };
	float maxExtent = 3.0f;
	if (const BoundsComponent* bounds = m_Scene.GetBoundsComponent(focusEntity))
	{
		localCenter = {
			(bounds->LocalMin.x + bounds->LocalMax.x) * 0.5f,
			(bounds->LocalMin.y + bounds->LocalMax.y) * 0.5f,
			(bounds->LocalMin.z + bounds->LocalMax.z) * 0.5f
		};

		const float extentX = (bounds->LocalMax.x - bounds->LocalMin.x) * std::abs(transform->WorldTransform.Scale.x);
		const float extentY = (bounds->LocalMax.y - bounds->LocalMin.y) * std::abs(transform->WorldTransform.Scale.y);
		const float extentZ = (bounds->LocalMax.z - bounds->LocalMin.z) * std::abs(transform->WorldTransform.Scale.z);
		maxExtent = (std::max)(3.0f, (std::max)(extentX, (std::max)(extentY, extentZ)));
	}

	DirectX::XMFLOAT3 worldCenter = {};
	DirectX::XMStoreFloat3(
		&worldCenter,
		DirectX::XMVector3TransformCoord(
			DirectX::XMLoadFloat3(&localCenter),
			transform->GetWorldXmMatrix()));

	const float cameraDistance = (std::max)(maxExtent * 2.5f, 6.0f);
	m_Camera.LookAt(
		{ worldCenter.x, worldCenter.y + maxExtent * 0.35f, worldCenter.z - cameraDistance },
		worldCenter,
		{ 0.0f, 1.0f, 0.0f });
	m_BenchmarkRunner.SetSpawnView(m_Camera);
}

void Engine::DrawBenchmarkInstances()
{
	if (m_SampleMode != Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		return;
	}

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		DrawDx12BenchmarkInstances();
	}
	else
	{
		DrawVulkanBenchmarkInstances();
	}
}

void Engine::DrawDx12BenchmarkInstances()
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device);
	if (!native || !cameraResource || !dx12Device || !m_StaticMeshRenderer.Dx12.PipelineState || !m_StaticMeshRenderer.Dx12.RootSignature)
	{
		return;
	}
	if (!m_StaticMeshRenderer.Dx12.ShaderResourceHeap || m_StaticMeshRenderer.Dx12.MaterialTextures.empty())
	{
		return;
	}

	const auto& config = m_BenchmarkRunner.GetConfig();
	const uint32_t instanceCount = config.ObjectCount;
	if (instanceCount == 0)
	{
		return;
	}

	const bool drawFullSpiderMesh =
		config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		&& ShouldUseFullSpiderMeshForBenchmark(instanceCount);

	const Asset::StaticMeshAsset* meshAsset = nullptr;
	float localScale = 0.45f;
	if (drawFullSpiderMesh)
	{
		meshAsset = GetMeshAsset(m_SpiderEntity);
		if (!meshAsset || meshAsset->Indices.empty())
		{
			return;
		}

		UploadEntityGeometry(m_SpiderEntity);
		localScale = 0.035f;
	}
	else
	{
		const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
			? GetBenchmarkSpiderGlyphGeometry()
			: GetBenchmarkPrimitiveGeometry();
		UploadBenchmarkGeometry(geometry.Vertices, geometry.Indices);
		localScale = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider ? 0.9f : 0.7f;
	}

	UpdateBenchmarkCameraBuffer(instanceCount, localScale);
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer);
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer);

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress());
	ID3D12DescriptorHeap* descriptorHeaps[] = { m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Get() };
	native->SetDescriptorHeaps(1, descriptorHeaps);
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	native->SetPipelineState(m_StaticMeshRenderer.Dx12.PipelineState.Get());

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = m_StaticMeshRenderer.Dx12.ShaderResourceHeap->GetGPUDescriptorHandleForHeapStart();

	if (drawFullSpiderMesh && meshAsset)
	{
		if (meshAsset->Submeshes.empty())
		{
			native->SetGraphicsRootDescriptorTable(1, baseHandle);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), instanceCount, 0, 0, 0);
			return;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			const size_t materialIndex = submesh.MaterialIndex < m_StaticMeshRenderer.Dx12.MaterialTextures.size() ? submesh.MaterialIndex : 0;
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex;
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, instanceCount, submesh.IndexOffset, 0, 0);
		}
		return;
	}

	native->SetGraphicsRootDescriptorTable(1, baseHandle);
	const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		? GetBenchmarkSpiderGlyphGeometry()
		: GetBenchmarkPrimitiveGeometry();
	m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(geometry.Indices.size()), instanceCount, 0, 0, 0);
}

void Engine::DrawVulkanBenchmarkInstances()
{
	if (!m_StaticMeshRenderer.Vulkan.IsValid || m_StaticMeshRenderer.Vulkan.DescriptorSets.empty())
	{
		return;
	}

	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	const auto& config = m_BenchmarkRunner.GetConfig();
	const uint32_t instanceCount = config.ObjectCount;
	if (instanceCount == 0)
	{
		return;
	}

	const bool drawFullSpiderMesh =
		config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		&& ShouldUseFullSpiderMeshForBenchmark(instanceCount);

	const Asset::StaticMeshAsset* meshAsset = nullptr;
	float localScale = 0.45f;
	if (drawFullSpiderMesh)
	{
		meshAsset = GetMeshAsset(m_SpiderEntity);
		if (!meshAsset || meshAsset->Indices.empty())
		{
			return;
		}

		UploadEntityGeometry(m_SpiderEntity);
		localScale = 0.035f;
	}
	else
	{
		const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
			? GetBenchmarkSpiderGlyphGeometry()
			: GetBenchmarkPrimitiveGeometry();
		UploadBenchmarkGeometry(geometry.Vertices, geometry.Indices);
		localScale = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider ? 0.9f : 0.7f;
	}

	UpdateBenchmarkCameraBuffer(instanceCount, localScale);
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer);
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer);

	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.Pipeline);

	if (drawFullSpiderMesh && meshAsset)
	{
		if (meshAsset->Submeshes.empty())
		{
			const VkDescriptorSet descriptorSet = m_StaticMeshRenderer.Vulkan.DescriptorSets.front();
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				0,
				nullptr);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), instanceCount, 0, 0, 0);
			return;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			const size_t materialIndex = submesh.MaterialIndex < m_StaticMeshRenderer.Vulkan.DescriptorSets.size() ? submesh.MaterialIndex : 0;
			const VkDescriptorSet descriptorSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex];
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				0,
				nullptr);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, instanceCount, submesh.IndexOffset, 0, 0);
		}
		return;
	}

	const VkDescriptorSet descriptorSet = m_StaticMeshRenderer.Vulkan.DescriptorSets.front();
	vkCmdBindDescriptorSets(
		commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		m_StaticMeshRenderer.Vulkan.PipelineLayout,
		0,
		1,
		&descriptorSet,
		0,
		nullptr);
	const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		? GetBenchmarkSpiderGlyphGeometry()
		: GetBenchmarkPrimitiveGeometry();
	m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(geometry.Indices.size()), instanceCount, 0, 0, 0);
}

void Engine::UploadBenchmarkGeometry(const std::vector<Asset::StaticMeshVertex>& vertices, const std::vector<uint32_t>& indices)
{
	if (!m_StaticMeshRenderer.VertexBuffer || !m_StaticMeshRenderer.IndexBuffer || vertices.empty() || indices.empty())
	{
		return;
	}

	if (vertices.size() * sizeof(Asset::StaticMeshVertex) > m_StaticMeshRenderer.VertexBuffer->GetSize()
		|| indices.size() * sizeof(uint32_t) > m_StaticMeshRenderer.IndexBuffer->GetSize())
	{
		return;
	}

	void* mappedVertexData = nullptr;
	m_StaticMeshRenderer.VertexBuffer->Map(&mappedVertexData);
	std::memcpy(mappedVertexData, vertices.data(), vertices.size() * sizeof(Asset::StaticMeshVertex));
	m_StaticMeshRenderer.VertexBuffer->Unmap();

	void* mappedIndexData = nullptr;
	m_StaticMeshRenderer.IndexBuffer->Map(&mappedIndexData);
	std::memcpy(mappedIndexData, indices.data(), indices.size() * sizeof(uint32_t));
	m_StaticMeshRenderer.IndexBuffer->Unmap();
}

void Engine::UpdateBenchmarkCameraBuffer(uint32_t instanceCount, float localScale)
{
	if (!m_StaticMeshRenderer.CameraBuffer)
	{
		return;
	}

	CameraConstants cameraConstants = {};
	DirectX::XMStoreFloat4x4(&cameraConstants.WorldViewProjection, m_Camera.GetProjectionMatrix());
	DirectX::XMStoreFloat4x4(&cameraConstants.ViewProjection, m_Camera.GetViewProjectionMatrix());
	const auto position = m_Camera.GetPosition();
	cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
	cameraConstants.BenchmarkParams = {
		static_cast<float>(instanceCount),
		localScale,
		m_Camera.GetFovY(),
		m_Camera.GetAspect()
	};

	void* mappedData = nullptr;
	m_StaticMeshRenderer.CameraBuffer->Map(&mappedData);
	std::memcpy(mappedData, &cameraConstants, sizeof(cameraConstants));
	m_StaticMeshRenderer.CameraBuffer->Unmap();
}

bool Engine::CreateDx12TriangleResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device);
	if (!dx12Device)
	{
		return false;
	}

	const std::string shaderSource = ShaderUtils::LoadShaderSource(GetDx12ShaderPath());
	if (shaderSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"DirectX12 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	ComPtr<ID3DBlob> errors;

	if (FAILED(D3DCompile(shaderSource.c_str(), shaderSource.size(), nullptr, nullptr, nullptr, 
		"VSMain", "vs_5_0", 0, 0, &vertexShader, &errors)))
	{
		return false;
	}

	if (FAILED(D3DCompile(shaderSource.c_str(), shaderSource.size(), nullptr, nullptr, nullptr, 
		"PSMain", "ps_5_0", 0, 0, &pixelShader, &errors)))
	{
		return false;
	}

	CD3DX12_DESCRIPTOR_RANGE descriptorRange = {};
	descriptorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

	CD3DX12_ROOT_PARAMETER rootParameters[2] = {};
	rootParameters[0].InitAsConstantBufferView(0);
	rootParameters[1].InitAsDescriptorTable(1, &descriptorRange, D3D12_SHADER_VISIBILITY_PIXEL);

	CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(2, rootParameters, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature;
	if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)))
	{
		return false;
	}

	if (FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(
		0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.RootSignature))))
	{
		return false;
	}

	static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.RootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.PipelineState))))
	{
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = psoDesc;
	transparentPsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	transparentPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparentPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparentPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	transparentPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	transparentPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	transparentPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparentPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	transparentPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

	return SUCCEEDED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.TransparentPipelineState)));
}

void Engine::DestroyDx12TriangleResources()
{
	m_StaticMeshRenderer.Dx12.TransparentPipelineState.Reset();
	m_StaticMeshRenderer.Dx12.PipelineState.Reset();
	m_StaticMeshRenderer.Dx12.RootSignature.Reset();
}

void Engine::DrawDx12Triangle()
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device);
	if (!native || !cameraResource || !dx12Device || !m_StaticMeshRenderer.Dx12.PipelineState || !m_StaticMeshRenderer.Dx12.RootSignature)
	{
		return;
	}

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress());
	if (!m_StaticMeshRenderer.Dx12.ShaderResourceHeap || m_StaticMeshRenderer.Dx12.MaterialTextures.empty())
	{
		return;
	}

	ID3D12DescriptorHeap* descriptorHeaps[] = { m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Get() };
	native->SetDescriptorHeaps(1, descriptorHeaps);
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = m_StaticMeshRenderer.Dx12.ShaderResourceHeap->GetGPUDescriptorHandleForHeapStart();
	const bool drawTransparentInSecondPass = (m_RenderMode == RenderMode::Forward || m_RenderMode == RenderMode::ForwardPlus) && m_StaticMeshRenderer.Dx12.TransparentPipelineState;
	const bool drawOpaquePass = true;
	if (drawOpaquePass)
	{
		native->SetPipelineState(m_StaticMeshRenderer.Dx12.PipelineState.Get());
	}

	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		// 공용 upload buffer를 재사용하므로,
		// entity마다 geometry와 camera 상수를 갱신한 뒤 draw를 이어서 기록합니다.
		UploadEntityGeometry(entityId);
		UpdateCameraBuffer(entityId);
		native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress());

		if (meshAsset->Submeshes.empty())
		{
			const bool entityIsTransparent = IsMaterialTransparent(entityId, 0);
			if ((entityIsTransparent && !drawTransparentInSecondPass) || (!entityIsTransparent && !drawOpaquePass))
			{
				continue;
			}

			native->SetPipelineState(entityIsTransparent ? m_StaticMeshRenderer.Dx12.TransparentPipelineState.Get() : m_StaticMeshRenderer.Dx12.PipelineState.Get());
			native->SetGraphicsRootDescriptorTable(1, baseHandle);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			continue;
		}

		auto drawSubmesh = [&](const Asset::StaticMeshSubmesh& submesh)
		{
			const size_t materialIndex = submesh.MaterialIndex < m_StaticMeshRenderer.Dx12.MaterialTextures.size() ? submesh.MaterialIndex : 0;
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex;
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
		};

		if (drawOpaquePass)
		{
			native->SetPipelineState(m_StaticMeshRenderer.Dx12.PipelineState.Get());
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh);
				}
			}
		}

		if (drawTransparentInSecondPass)
		{
			native->SetPipelineState(m_StaticMeshRenderer.Dx12.TransparentPipelineState.Get());
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh);
				}
			}
		}
	}
}

bool Engine::CreateCameraBuffer()
{
	constexpr uint64_t dx12AlignedCameraBufferSize = 256;
	const uint64_t cameraBufferSize = (std::max)(dx12AlignedCameraBufferSize, static_cast<uint64_t>(sizeof(CameraConstants)));

	const BufferDesc bufferDesc = {
		.Size = cameraBufferSize,
		.Stride = sizeof(CameraConstants),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_StaticMeshRenderer.CameraBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
	return m_StaticMeshRenderer.CameraBuffer != nullptr;
}

void Engine::UpdateCameraBuffer()
{
	UpdateCameraBuffer(m_Scene.GetPrimaryRenderableEntity());
}

void Engine::UpdateCameraBuffer(EntityId entityId)
{
	if (!m_StaticMeshRenderer.CameraBuffer)
	{
		return;
	}

	CameraConstants cameraConstants = {};
	if (!RenderSystem::BuildCameraConstants(m_Scene, m_Camera, entityId, cameraConstants))
	{
		return;
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.CameraBuffer->Map(&mappedData);
	std::memcpy(mappedData, &cameraConstants, sizeof(cameraConstants));
	m_StaticMeshRenderer.CameraBuffer->Unmap();
}

void Engine::UpdateObjectPicking()
{
	InputSystem& input = InputSystem::Get();
	if (!input.IsMouseButtonPressed(0) || input.IsMouseButtonDown(1))
	{
		return;
	}

	if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
	{
		return;
	}

	const int mouseX = input.GetMouseX();
	const int mouseY = input.GetMouseY();
	if (mouseX < 0 || mouseY < 0 || mouseX >= m_ClientWidth || mouseY >= m_ClientHeight)
	{
		return;
	}

	m_Scene.SetSelectedEntity(TryPickEntity(static_cast<float>(mouseX), static_cast<float>(mouseY)));
}

bool Engine::TryPickSpider(float mouseX, float mouseY) const
{
	return TryPickEntity(mouseX, mouseY) == m_SpiderEntity;
}

EntityId Engine::TryPickEntity(float mouseX, float mouseY) const
{
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (entityId == InvalidEntityId)
		{
			continue;
		}

		if (PickingSystem::TryPickEntityAabb(
			m_Scene,
			entityId,
			m_Camera,
			mouseX,
			mouseY,
			static_cast<float>(m_ClientWidth),
			static_cast<float>(m_ClientHeight)))
		{
			return entityId;
		}
	}

	return InvalidEntityId;
}


























void Engine::UpdateAnimatedMesh(float deltaTime)
{
	AnimationSystem::UpdateAnimatedMesh(
		m_Scene,
		m_SpiderEntity,
		deltaTime,
		m_AnimationTimeSeconds,
		m_StaticMeshRenderer.VertexBuffer);
}

bool Engine::CreateVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
	auto vulkanCameraBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.CameraBuffer);
	if (!vulkanDevice || !vulkanCameraBuffer)
	{
		return false;
	}

	const std::string vertexShaderSource = ShaderUtils::LoadShaderSource(GetVulkanVertexShaderPath());
	const std::string fragmentShaderSource = ShaderUtils::LoadShaderSource(GetVulkanFragmentShaderPath());
	if (vertexShaderSource.empty() || fragmentShaderSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"Vulkan 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	const std::vector<uint32_t> vertexShaderCode = ShaderUtils::CompileGlslToSpirv(
		GLSLANG_STAGE_VERTEX, vertexShaderSource);
	const std::vector<uint32_t> fragmentShaderCode = ShaderUtils::CompileGlslToSpirv(
		GLSLANG_STAGE_FRAGMENT, fragmentShaderSource);

	if (vertexShaderCode.empty() || fragmentShaderCode.empty())
	{
		MessageBoxW(m_hMainWnd, L"Vulkan 셰이더 컴파일에 실패했습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	const VkShaderModuleCreateInfo vertexModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = vertexShaderCode.size() * sizeof(uint32_t),
		.pCode = vertexShaderCode.data()
	};

	if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &vertexModuleCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.VertexShader) != VK_SUCCESS)
	{
		return false;
	}

	const VkShaderModuleCreateInfo fragmentModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fragmentShaderCode.size() * sizeof(uint32_t),
		.pCode = fragmentShaderCode.data()
	};

	if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &fragmentModuleCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.PixelShader) != VK_SUCCESS)
	{
		return false;
	}

	// Vulkan 경로는 카메라 상수 버퍼를 uniform buffer + descriptor set으로 바인딩합니다.
	const VkDescriptorSetLayoutBinding cameraBinding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT
	};
	// Vulkan 경로는 diffuse texture를 combined image sampler로 fragment shader에 바인딩합니다.
	const VkDescriptorSetLayoutBinding textureBinding = {
		.binding = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
	};
	const VkDescriptorSetLayoutBinding bindings[] = { cameraBinding, textureBinding };

	const VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(std::size(bindings)),
		.pBindings = bindings
	};

	if (vkCreateDescriptorSetLayout(vulkanDevice->GetVkDevice(), &descriptorSetLayoutCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.DescriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}

	const uint32_t materialTextureCount = static_cast<uint32_t>((std::max)(static_cast<size_t>(1), m_StaticMeshRenderer.Vulkan.MaterialTextures.size()));

	const VkDescriptorPoolSize descriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = materialTextureCount
	};
	const VkDescriptorPoolSize textureDescriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = materialTextureCount
	};
	const VkDescriptorPoolSize descriptorPoolSizes[] = { descriptorPoolSize, textureDescriptorPoolSize };

	const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = materialTextureCount,
		.poolSizeCount = static_cast<uint32_t>(std::size(descriptorPoolSizes)),
		.pPoolSizes = descriptorPoolSizes
	};

	if (vkCreateDescriptorPool(vulkanDevice->GetVkDevice(), &descriptorPoolCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.DescriptorPool) != VK_SUCCESS)
	{
		return false;
	}

	std::vector<VkDescriptorSetLayout> descriptorSetLayouts(materialTextureCount, m_StaticMeshRenderer.Vulkan.DescriptorSetLayout);
	const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_StaticMeshRenderer.Vulkan.DescriptorPool,
		.descriptorSetCount = materialTextureCount,
		.pSetLayouts = descriptorSetLayouts.data()
	};
	m_StaticMeshRenderer.Vulkan.DescriptorSets.resize(materialTextureCount);

	if (vkAllocateDescriptorSets(vulkanDevice->GetVkDevice(), &descriptorSetAllocateInfo, m_StaticMeshRenderer.Vulkan.DescriptorSets.data()) != VK_SUCCESS)
	{
		return false;
	}

	const VkDescriptorBufferInfo cameraBufferInfo = {
		.buffer = vulkanCameraBuffer->GetVkBuffer(),
		.offset = 0,
		.range = sizeof(CameraConstants)
	};
	// Vulkan 경로는 material 수만큼 descriptor set을 만들고, 각 set에 동일한 camera buffer와 material별 texture를 기록합니다.
	// 이렇게 해 두면 draw 시 submesh.MaterialIndex에 맞는 descriptor set 하나만 다시 바인딩하면 됩니다.
	for (uint32_t materialIndex = 0; materialIndex < materialTextureCount; ++materialIndex)
	{
		const auto& materialTexture = m_StaticMeshRenderer.Vulkan.MaterialTextures[materialIndex];
		const VkDescriptorImageInfo textureImageInfo = {
			.sampler = materialTexture.Sampler,
			.imageView = materialTexture.ImageView,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};

		const VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &cameraBufferInfo
		};
		const VkWriteDescriptorSet textureWriteDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex],
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &textureImageInfo
		};
		const VkWriteDescriptorSet writeDescriptorSets[] = { writeDescriptorSet, textureWriteDescriptorSet };

		vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), static_cast<uint32_t>(std::size(writeDescriptorSets)), writeDescriptorSets, 0, nullptr);
	}

	const VkPipelineShaderStageCreateInfo shaderStages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = m_StaticMeshRenderer.Vulkan.VertexShader,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = m_StaticMeshRenderer.Vulkan.PixelShader,
			.pName = "main"
		}
	};

	// Vulkan 정적 메시 경로는 Assimp로 읽은 interleaved vertex buffer를 그대로 사용하므로
	// binding 0 하나에 Position/Normal/TexCoord attribute를 연결합니다.
	const VkVertexInputBindingDescription vertexBindingDescription = {
		.binding = 0,
		.stride = sizeof(Asset::StaticMeshVertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX
	};

	const VkVertexInputAttributeDescription vertexAttributeDescriptions[] = {
		{
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, Position)
		},
		{
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, Normal)
		},
		{
			.location = 2,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, TexCoord)
		},
		{
			.location = 3,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, Color)
		}
	};

	const VkPipelineVertexInputStateCreateInfo vertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertexBindingDescription,
		.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(vertexAttributeDescriptions)),
		.pVertexAttributeDescriptions = vertexAttributeDescriptions
	};

	const VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	};

	const VkPipelineViewportStateCreateInfo viewportState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1
	};

	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	const VkPipelineDynamicStateCreateInfo dynamicState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamicStates
	};

	const VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f
	};

	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};

	const VkPipelineColorBlendAttachmentState colorBlendAttachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};
	VkPipelineColorBlendAttachmentState transparentColorBlendAttachment = colorBlendAttachment;
	transparentColorBlendAttachment.blendEnable = VK_TRUE;
	transparentColorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	transparentColorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	transparentColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	transparentColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	transparentColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	transparentColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

	const VkPipelineColorBlendStateCreateInfo colorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorBlendAttachment
	};
	const VkPipelineColorBlendStateCreateInfo transparentColorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &transparentColorBlendAttachment
	};

	const VkPipelineDepthStencilStateCreateInfo depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS
	};
	VkPipelineDepthStencilStateCreateInfo transparentDepthStencil = depthStencil;
	transparentDepthStencil.depthWriteEnable = VK_FALSE;

	const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_StaticMeshRenderer.Vulkan.DescriptorSetLayout
	};

	if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &pipelineLayoutCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.PipelineLayout) != VK_SUCCESS)
	{
		return false;
	}

	const VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = shaderStages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depthStencil,
		.pColorBlendState = &colorBlending,
		.pDynamicState = &dynamicState,
		.layout = m_StaticMeshRenderer.Vulkan.PipelineLayout,
		.renderPass = vulkanDevice->GetVkRenderPass(),
		.subpass = 0
	};

	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.Pipeline) != VK_SUCCESS)
	{
		return false;
	}

	// Vulkan 투명 패스는 불투명 파이프라인과 동일한 셰이더/레이아웃을 사용하되,
	// color blend를 켜고 depth write를 꺼서 반투명 유리 오브젝트를 올바르게 합성합니다.
	VkGraphicsPipelineCreateInfo transparentPipelineCreateInfo = pipelineCreateInfo;
	transparentPipelineCreateInfo.pColorBlendState = &transparentColorBlending;
	transparentPipelineCreateInfo.pDepthStencilState = &transparentDepthStencil;
	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &transparentPipelineCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.TransparentPipeline) != VK_SUCCESS)
	{
		return false;
	}

	m_StaticMeshRenderer.Vulkan.IsValid = true;
	return true;
}

void Engine::DestroyVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device);
	if (!vulkanDevice)
	{
		auto savedMaterialTextures = std::move(m_StaticMeshRenderer.Vulkan.MaterialTextures);

		m_StaticMeshRenderer.Vulkan = {};

		m_StaticMeshRenderer.Vulkan.MaterialTextures = std::move(savedMaterialTextures);
		return;
	}

	if (m_StaticMeshRenderer.Vulkan.Pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.Pipeline, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.TransparentPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.TransparentPipeline, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.PipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.PipelineLayout, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.DescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.DescriptorPool, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.DescriptorSetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.DescriptorSetLayout, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.VertexShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.VertexShader, nullptr);
	}

	if (m_StaticMeshRenderer.Vulkan.PixelShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_StaticMeshRenderer.Vulkan.PixelShader, nullptr);
	}

	// 파이프라인 관련 핸들만 초기화합니다.
	// Vulkan material texture는 DestroyTextureResources()가 담당하므로, 리사이즈 중에는 벡터를 보존해야
	// descriptor set과 pipeline만 재생성하면서 기존 텍스처를 다시 사용할 수 있습니다.
	auto savedMaterialTextures = std::move(m_StaticMeshRenderer.Vulkan.MaterialTextures);

	m_StaticMeshRenderer.Vulkan = {};

	m_StaticMeshRenderer.Vulkan.MaterialTextures = std::move(savedMaterialTextures);
}

void Engine::DrawVulkanTriangle()
{
	if (!m_StaticMeshRenderer.Vulkan.IsValid)
	{
		return;
	}

	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	if (m_StaticMeshRenderer.Vulkan.DescriptorSets.empty())
	{
		return;
	}

	const bool drawTransparentInSecondPass = (m_RenderMode == RenderMode::Forward || m_RenderMode == RenderMode::ForwardPlus) && m_StaticMeshRenderer.Vulkan.TransparentPipeline != VK_NULL_HANDLE;
	const bool drawOpaquePass = true;

	// Vulkan은 현재 열린 render pass 안에서 그래픽 파이프라인을 바인딩하고 draw를 기록합니다.
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.Pipeline);

	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		UpdateCameraBuffer(entityId);

		if (meshAsset->Submeshes.empty())
		{
			const bool entityIsTransparent = IsMaterialTransparent(entityId, 0);
			if ((entityIsTransparent && !drawTransparentInSecondPass) || (!entityIsTransparent && !drawOpaquePass))
			{
				continue;
			}

			// Vulkan 단일-메시 fallback에서도 엔티티 투명 여부에 따라 알맞은 파이프라인을 바꿉니다.
			vkCmdBindPipeline(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				entityIsTransparent ? m_StaticMeshRenderer.Vulkan.TransparentPipeline : m_StaticMeshRenderer.Vulkan.Pipeline);

			// Vulkan fallback 경로에서는 첫 번째 material descriptor set을 사용해 전체 메시를 그립니다.
			const VkDescriptorSet descriptorSet = m_StaticMeshRenderer.Vulkan.DescriptorSets.front();
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				0,
				nullptr);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			continue;
		}

		auto drawSubmesh = [&](const Asset::StaticMeshSubmesh& submesh)
		{
			const size_t materialIndex = submesh.MaterialIndex < m_StaticMeshRenderer.Vulkan.DescriptorSets.size() ? submesh.MaterialIndex : 0;
			const VkDescriptorSet descriptorSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex];

			// Vulkan 경로는 submesh.MaterialIndex에 대응하는 descriptor set을 바인딩해 material별 texture를 선택합니다.
			// 그런 다음 해당 submesh의 index 범위만 DrawIndexedInstanced로 기록해 멀티 머티리얼 메시를 올바르게 렌더링합니다.
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				0,
				nullptr);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
		};

		if (drawOpaquePass)
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.Pipeline);
			// Vulkan의 불투명 패스는 forward/deferred/forward+ 공통으로 먼저 실행합니다.
			// deferred와 forward+에서는 이 구간이 G-Buffer geometry pass에 해당하는 역할입니다.
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh);
				}
			}
		}

		if (drawTransparentInSecondPass)
		{
			// Vulkan 투명 패스는 alpha blend가 켜진 전용 파이프라인으로 그려야 유리 오브젝트가 반투명하게 합성됩니다.
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.TransparentPipeline);
			// Vulkan의 투명 패스는 forward와 forward+에서만 실행합니다.
			// deferred 모드에서는 투명 물체를 생략해 전통적인 deferred 제한을 따릅니다.
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh);
				}
			}
		}
	}
}

bool Engine::CreateTriangleVertexBuffer()
{
	const Asset::StaticMeshAsset* spiderMesh = RenderSystem::GetPrimaryRenderableMesh(m_Scene);
	if (!spiderMesh || spiderMesh->Vertices.empty())
	{
		return false;
	}

	const BufferDesc bufferDesc = {
		.Size = static_cast<uint64_t>(spiderMesh->Vertices.size() * sizeof(Asset::StaticMeshVertex)),
		.Stride = sizeof(Asset::StaticMeshVertex),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_StaticMeshRenderer.VertexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
	if (!m_StaticMeshRenderer.VertexBuffer)
	{
		return false;
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.VertexBuffer->Map(&mappedData);
	std::memcpy(mappedData, spiderMesh->Vertices.data(), static_cast<size_t>(bufferDesc.Size));
	m_StaticMeshRenderer.VertexBuffer->Unmap();

	return true;
}

bool Engine::CreateIndexBuffer()
{
	const Asset::StaticMeshAsset* spiderMesh = RenderSystem::GetPrimaryRenderableMesh(m_Scene);
	if (!spiderMesh || spiderMesh->Indices.empty())
	{
		return false;
	}

	const BufferDesc bufferDesc = {
		.Size = static_cast<uint64_t>(spiderMesh->Indices.size() * sizeof(uint32_t)),
		.Stride = sizeof(uint32_t),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_StaticMeshRenderer.IndexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
	if (!m_StaticMeshRenderer.IndexBuffer)
	{
		return false;
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.IndexBuffer->Map(&mappedData);
	std::memcpy(mappedData, spiderMesh->Indices.data(), static_cast<size_t>(bufferDesc.Size));
	m_StaticMeshRenderer.IndexBuffer->Unmap();
	return true;
}

void Engine::UpdateRendererMenuState()
{
	HMENU menu = GetMenu(m_hMainWnd);
	if (!menu)
	{
		return;
	}

	CheckMenuItem(menu, IDM_RENDERER_DX12, 
		MF_BYCOMMAND | (m_Graphics.CurrentApi == GraphicsAPI::DirectX12 ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_RENDERER_VULKAN, 
		MF_BYCOMMAND | (m_Graphics.CurrentApi == GraphicsAPI::Vulkan ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_RENDERMODE_FORWARD,
		MF_BYCOMMAND | (m_RenderMode == RenderMode::Forward ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_RENDERMODE_DEFERRED,
		MF_BYCOMMAND | (m_RenderMode == RenderMode::Deferred ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_RENDERMODE_FORWARD_PLUS,
		MF_BYCOMMAND | (m_RenderMode == RenderMode::ForwardPlus ? MF_CHECKED : MF_UNCHECKED));
	DrawMenuBar(m_hMainWnd);
}

void Engine::ResetFpsCounter()
{
	m_FrameCount = 0;
	m_LastFpsUpdate = std::chrono::steady_clock::now();
	SetWindowTextW(m_hMainWnd, m_WindowTitleBase.c_str());
}

void Engine::UpdateWindowTitleWithFps()
{
	// 제목 표시줄 갱신은 1초에 한 번만 수행해 문자열 포맷 비용과 Win32 호출 빈도를 최소화합니다.
	++m_FrameCount;

	const auto now = std::chrono::steady_clock::now();
	const std::chrono::duration<double> elapsed = now - m_LastFpsUpdate;
	if (elapsed.count() < 1.0)
	{
		return;
	}

	const double fps = static_cast<double>(m_FrameCount) / elapsed.count();
	wchar_t titleBuffer[256] = {};
	swprintf_s(titleBuffer, L"%s | FPS: %.1f", m_WindowTitleBase.c_str(), fps);
	SetWindowTextW(m_hMainWnd, titleBuffer);

	m_FrameCount = 0;
	m_LastFpsUpdate = now;
}
