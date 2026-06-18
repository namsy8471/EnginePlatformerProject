#include "Engine.h"
#include "Animation/AnimationSystem.h"
#include "Assets/PrimitiveMeshFactory.h"
#include "Input/InputSystem.h"
#include "Memory/MemorySystem.h"
#include "Scene/PickingSystem.h"
#include "App/Win32/Resource.h"
#include "Rendering/Resources/MaterialTextureSystem.h"
#include "Rendering/Sky/SkyboxAsset.h"
#include "Rendering/Systems/RenderSystem.h"
#include "Samples/Spider/SpiderSampleScene.h"
#include "Utilities/ShaderUtils.h"

#include "Rendering/Backends/DirectX12/DX12Buffer.h"
#include "Rendering/Backends/DirectX12/DX12Device.h"
#include "Rendering/Backends/DirectX12/d3dx12.h"
#include "Rendering/Backends/Vulkan/VulkanBuffer.h"
#include "Rendering/Backends/Vulkan/VulkanCommandList.h"
#include "Rendering/Backends/Vulkan/VulkanDevice.h"

#include <commdlg.h>
#include <d3dcompiler.h>
#include <glslang/Include/glslang_c_interface.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>

#if __has_include(<dstorage.h>)
#include <dstorage.h>
#pragma comment(lib, "dstoragecore.lib")
#define ENGINE_HAS_DIRECTSTORAGE_SDK 1
#else
#define ENGINE_HAS_DIRECTSTORAGE_SDK 0
#endif

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

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

		if (msg == WM_DROPFILES)
		{
			SendMessageW(GetParent(hWnd), msg, wParam, lParam);
			return 0;
		}

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

	[[nodiscard]] constexpr uint32_t CameraConstantSlotCount() noexcept
	{
		return 8192;
	}

	[[nodiscard]] constexpr uint64_t InvalidCameraConstantOffset() noexcept
	{
		return (std::numeric_limits<uint64_t>::max)();
	}

	[[nodiscard]] constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept
	{
		return alignment > 0 ? ((value + alignment - 1) / alignment) * alignment : value;
	}

	[[nodiscard]] std::array<float, 4> BuildSkyClearColor(const Rendering::SkyboxSettings& skybox) noexcept
	{
		if (!skybox.Enabled)
		{
			return { 0.025f, 0.027f, 0.032f, 1.0f };
		}

		const DirectX::XMFLOAT3 color = Rendering::EstimateSkyboxClearColor(skybox);
		return {
			std::clamp(color.x, 0.0f, 8.0f),
			std::clamp(color.y, 0.0f, 8.0f),
			std::clamp(color.z, 0.0f, 8.0f),
			1.0f
		};
	}

	[[nodiscard]] std::string SanitizeFileStem(std::string_view name)
	{
		std::string sanitized;
		sanitized.reserve(name.size());
		for (const char character : name)
		{
			const bool valid =
				(character >= 'a' && character <= 'z') ||
				(character >= 'A' && character <= 'Z') ||
				(character >= '0' && character <= '9') ||
				character == '_' ||
				character == '-';
			sanitized.push_back(valid ? character : '_');
		}
		return sanitized.empty() ? "Entity" : sanitized;
	}

	template <typename Fn>
	void MeasureRenderGraphPass(Rendering::RenderGraph& graph, size_t passIndex, Fn&& fn)
	{
		const auto begin = std::chrono::steady_clock::now();
		std::forward<Fn>(fn)();
		const auto end = std::chrono::steady_clock::now();
		const std::chrono::duration<double, std::milli> elapsed = end - begin;
		graph.SetPassCpuTime(passIndex, elapsed.count());
	}

	[[nodiscard]] constexpr std::string_view SceneComponentKindName(SceneComponentKind kind) noexcept
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
		case SceneComponentKind::PrefabInstance:
			return "Prefab Instance";
		case SceneComponentKind::SceneReference:
			return "Scene Reference";
		case SceneComponentKind::Script:
			return "Script";
		case SceneComponentKind::Sprite2D:
			return "Sprite 2D";
		case SceneComponentKind::UiElement:
			return "UI Element";
		case SceneComponentKind::AudioSource:
			return "Audio Source";
		case SceneComponentKind::NavigationAgent:
			return "Navigation Agent";
		case SceneComponentKind::NetworkIdentity:
			return "Network Identity";
		default:
			return "Component";
		}
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

	[[nodiscard]] bool IsDirectStorageTextureCandidate(const CpuMaterialTextureSlot& materialTexture) noexcept
	{
		if (materialTexture.Path.empty())
		{
			return false;
		}

		const std::filesystem::path extension = materialTexture.Path.extension();
		return _wcsicmp(extension.c_str(), L".dds") == 0;
	}

	[[nodiscard]] constexpr size_t MaterialSlotCount() noexcept
	{
		return Asset::kMaterialTextureSlotCount;
	}

	[[nodiscard]] size_t FlattenMaterialTextureIndex(size_t materialIndex, Asset::MaterialTextureSlot slot) noexcept
	{
		return materialIndex * MaterialSlotCount() + Asset::MaterialTextureSlotIndex(slot);
	}

	[[nodiscard]] size_t FlattenMaterialTextureIndex(size_t materialIndex, size_t slotIndex) noexcept
	{
		return materialIndex * MaterialSlotCount() + slotIndex;
	}

	[[nodiscard]] size_t MaterialCountFromFlattenedTextureCount(size_t textureCount) noexcept
	{
		return (std::max)(static_cast<size_t>(1), (textureCount + MaterialSlotCount() - 1) / MaterialSlotCount());
	}

	[[nodiscard]] const CpuMaterialTextureSlot& GetCpuMaterialTextureSlot(
		std::span<const CpuMaterialTexture> materialTextures,
		size_t materialIndex,
		size_t slotIndex)
	{
		static const CpuMaterialTexture fallbackMaterialTexture = {};
		if (materialIndex >= materialTextures.size() || slotIndex >= Asset::kMaterialTextureSlotCount)
		{
			return fallbackMaterialTexture.Slots[0];
		}
		return materialTextures[materialIndex].Slots[slotIndex];
	}

	[[nodiscard]] DXGI_FORMAT GetDx12TextureSrvFormat(const CpuMaterialTextureSlot& texture) noexcept
	{
		return texture.Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	[[nodiscard]] VkFormat GetVulkanTextureFormat(const CpuMaterialTextureSlot& texture) noexcept
	{
		return texture.Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	}

	void ApplyMaterialOverrides(Asset::StaticMeshAsset& mesh, const std::vector<Asset::StaticMeshMaterial>& overrides);

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

	[[nodiscard]] constexpr std::string_view GetDx12DeferredGeometryShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/DeferredGeometry.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetDx12DeferredLightingShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/DeferredLighting.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetDx12ShadowDepthShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/ShadowDepth.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetDx12ToneMapShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/ToneMap.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetDx12SkyboxShaderPath() noexcept
	{
		return "Src/Rendering/Backends/DirectX12/Shaders/Skybox.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Triangle.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Triangle.frag";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanDeferredGeometryVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/DeferredGeometry.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanDeferredGeometryFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/DeferredGeometry.frag";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanDeferredLightingVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/DeferredLighting.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanDeferredLightingFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/DeferredLighting.frag";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanToneMapVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/ToneMap.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanToneMapFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/ToneMap.frag";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanSkyboxVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Skybox.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanSkyboxFragmentShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/Skybox.frag";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanShadowDepthVertexShaderPath() noexcept
	{
		return "Src/Rendering/Backends/Vulkan/Shaders/ShadowDepth.vert";
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

Engine::Engine(HINSTANCE hInstance, EngineStartupOptions startupOptions)
	: GameApp(hInstance),
	m_StartupOptions(std::move(startupOptions))
{
	if (m_StartupOptions.SmokeGraphicsApi)
	{
		m_Graphics.CurrentApi = *m_StartupOptions.SmokeGraphicsApi;
		m_PendingGraphicsApi = *m_StartupOptions.SmokeGraphicsApi;
	}
	if (m_StartupOptions.SmokeRenderMode)
	{
		m_RenderMode = *m_StartupOptions.SmokeRenderMode;
	}
}

Engine::~Engine()
{
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: Engine destructor begin.");
	}
	m_AssetImportService.Shutdown();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: asset import service stopped.");
	}
	m_AssetHotReloadService.Shutdown();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: hot reload service stopped.");
	}
	ShutdownJobSystem();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: job system stopped.");
	}
	m_PhysicsWorld.Shutdown();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: physics stopped.");
	}
	ShutdownGraphics();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: graphics stopped.");
	}
	DestroyRenderWindow();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: render window destroyed.");
	}
	glslang_finalize_process();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		AppendAssetLog("Smoke shutdown: Engine destructor end.");
	}
}

void Engine::InitializeJobSystem()
{
	m_JobSystem.Initialize();
	m_PhaseScheduler.SetJobSystem(m_JobSystem);
	m_ScriptRuntime.RegisterDefaultScripts();
	AppendAssetLog(std::format("Job system initialized with {} workers.", m_JobSystem.GetWorkerCount()));
}

void Engine::ShutdownJobSystem()
{
	m_SceneCommandBuffer.Clear();
	m_JobSystem.Shutdown();
}

EntityId Engine::CreateEntity(std::string_view name)
{
	return m_Scene.CreateEntity(name);
}

EntityId Engine::CreateEmptySceneEntity(std::string_view name, EntityId parentEntity)
{
	const std::string entityName = name.empty() ? std::string("Entity") : std::string(name);
	const EntityId entityId = CreateEntity(entityName);
	const DirectX::XMFLOAT3 cameraPosition = m_SceneCamera.GetPosition();
	const DirectX::XMFLOAT3 cameraForward = m_SceneCamera.GetForward();
	const DirectX::XMVECTOR target = DirectX::XMVectorAdd(
		DirectX::XMLoadFloat3(&cameraPosition),
		DirectX::XMVectorScale(DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraForward)), 5.0f));
	DirectX::XMFLOAT3 translation = {};
	DirectX::XMStoreFloat3(&translation, target);

	TransformComponent& transform = m_Scene.EnsureTransformComponent(entityId);
	transform.LocalTransform = Math::Transform(translation, Math::IdentityQuaternion(), Math::OneVector3());
	transform.UpdateWorld();
	if (parentEntity != InvalidEntityId && m_Scene.ContainsEntity(parentEntity))
	{
		static_cast<void>(m_Scene.SetParentEntity(entityId, parentEntity, true));
	}

	m_Scene.SetSelectedEntity(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("Created empty entity {} ({})", entityId, entityName));
	return entityId;
}

EntityId Engine::CreateCameraSceneEntity(EntityId parentEntity)
{
	const EntityId entityId = CreateEmptySceneEntity("Camera", parentEntity);
	CameraComponent& camera = m_Scene.EnsureCameraComponent(entityId);
	camera.FovY = m_SceneCamera.GetFovY();
	camera.NearZ = m_SceneCamera.GetNearZ();
	camera.FarZ = m_SceneCamera.GetFarZ();
	camera.IsGameCamera = false;
	AppendAssetLog(std::format("Added Camera component to entity {}", entityId));
	return entityId;
}

EntityId Engine::CreateLightSceneEntity(EntityId parentEntity)
{
	const EntityId entityId = CreateEmptySceneEntity("Light", parentEntity);
	LightComponent& light = m_Scene.EnsureLightComponent(entityId);
	light.Type = LightType::Directional;
	light.Color = { 1.0f, 0.95f, 0.82f };
	light.Intensity = 3.25f;
	light.Range = 450.0f;
	light.SpotAngle = DirectX::XM_PIDIV4;
	light.Enabled = true;
	AppendAssetLog(std::format("Added Light component to entity {}", entityId));
	return entityId;
}

void Engine::CreateEmptySceneEntityWithUndo(std::string name, EntityId parentEntity)
{
	if (BlockEditSceneMutationDuringPlay("Create Entity"))
	{
		return;
	}

	if (name.empty())
	{
		name = "Entity";
	}

	const auto createdEntity = std::make_shared<EntityId>(InvalidEntityId);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Create {}", name),
		.Execute = [this, name, parentEntity, createdEntity]()
		{
			*createdEntity = CreateEmptySceneEntity(name, parentEntity);
		},
		.Undo = [this, createdEntity]()
		{
			if (*createdEntity != InvalidEntityId && m_Scene.ContainsEntity(*createdEntity))
			{
				DeleteEntityFromHierarchy(*createdEntity);
				*createdEntity = InvalidEntityId;
			}
		}
	});
}

void Engine::CreateCameraSceneEntityWithUndo(EntityId parentEntity)
{
	if (BlockEditSceneMutationDuringPlay("Create Camera"))
	{
		return;
	}

	const auto createdEntity = std::make_shared<EntityId>(InvalidEntityId);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Create Camera",
		.Execute = [this, parentEntity, createdEntity]()
		{
			*createdEntity = CreateCameraSceneEntity(parentEntity);
		},
		.Undo = [this, createdEntity]()
		{
			if (*createdEntity != InvalidEntityId && m_Scene.ContainsEntity(*createdEntity))
			{
				DeleteEntityFromHierarchy(*createdEntity);
				*createdEntity = InvalidEntityId;
			}
		}
	});
}

void Engine::CreateLightSceneEntityWithUndo(EntityId parentEntity)
{
	if (BlockEditSceneMutationDuringPlay("Create Light"))
	{
		return;
	}

	const auto createdEntity = std::make_shared<EntityId>(InvalidEntityId);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Create Light",
		.Execute = [this, parentEntity, createdEntity]()
		{
			*createdEntity = CreateLightSceneEntity(parentEntity);
		},
		.Undo = [this, createdEntity]()
		{
			if (*createdEntity != InvalidEntityId && m_Scene.ContainsEntity(*createdEntity))
			{
				DeleteEntityFromHierarchy(*createdEntity);
				*createdEntity = InvalidEntityId;
			}
		}
	});
}

void Engine::CreateEmptyParentForEntityWithUndo(EntityId childEntity)
{
	if (BlockEditSceneMutationDuringPlay("Create Empty Parent"))
	{
		return;
	}

	if (childEntity == InvalidEntityId || !m_Scene.ContainsEntity(childEntity))
	{
		return;
	}

	const std::string* childName = m_Scene.GetEntityName(childEntity);
	const std::string parentName = std::format("{} Parent", childName && !childName->empty() ? *childName : std::string("Entity"));
	const EntityId previousParent = m_Scene.GetParentEntity(childEntity);
	Math::Transform parentWorldTransform = Math::Transform::Identity();
	if (const TransformComponent* childTransform = m_Scene.GetTransformComponent(childEntity))
	{
		parentWorldTransform = childTransform->WorldTransform;
	}

	const auto createdParent = std::make_shared<EntityId>(InvalidEntityId);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Create Empty Parent",
		.Execute = [this, childEntity, previousParent, parentName, parentWorldTransform, createdParent]()
		{
			if (!m_Scene.ContainsEntity(childEntity))
			{
				return;
			}

			*createdParent = CreateEmptySceneEntity(parentName);
			if (*createdParent == InvalidEntityId)
			{
				return;
			}

			if (TransformComponent* parentTransform = m_Scene.GetTransformComponent(*createdParent))
			{
				parentTransform->LocalTransform = parentWorldTransform;
				parentTransform->WorldTransform = parentWorldTransform;
			}
			if (previousParent != InvalidEntityId && m_Scene.ContainsEntity(previousParent))
			{
				static_cast<void>(m_Scene.SetParentEntity(*createdParent, previousParent, true));
			}
			static_cast<void>(m_Scene.SetParentEntity(childEntity, *createdParent, true));
			static_cast<void>(m_Scene.MoveEntityBefore(*createdParent, childEntity));
			m_Scene.SetSelectedEntity(*createdParent);
			MarkSceneDirty();
			AppendAssetLog(std::format("Created empty parent entity {} for child {}", *createdParent, childEntity));
		},
		.Undo = [this, createdParent]()
		{
			if (*createdParent != InvalidEntityId && m_Scene.ContainsEntity(*createdParent))
			{
				DeleteEntityFromHierarchy(*createdParent);
				*createdParent = InvalidEntityId;
			}
		}
	});
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
	return m_RenderState.IsMaterialTransparent(GetRuntimeScene(), entityId, materialIndex);
}

void Engine::QueueModelImport(const std::filesystem::path& sourcePath, const Camera& placementCamera, bool isReload)
{
	MarkResourcePreparing(sourcePath, Resources::ResourceKind::Mesh);

	Asset::AssetImportPlacement placement;
	placement.CameraPosition = placementCamera.GetPosition();
	placement.CameraForward = placementCamera.GetForward();
	placement.HasPlacement = !isReload;

	const uint64_t generation = m_RuntimeAssetRegistry.NextGeneration(sourcePath);
	Asset::AssetImportRequest request;
	request.SourcePath = sourcePath;
	request.Generation = generation;
	request.SceneGeneration = m_AssetSceneGeneration;
	request.IsReload = isReload;
	request.Placement = placement;
	m_AssetImportService.Queue(std::move(request));

	AppendAssetLog(std::format("{} queued: {}", isReload ? "Reload" : "Import", sourcePath.string()));
}

void Engine::QueueModelImportForSceneEntity(
	const ScenePersistence::LoadedSceneEntity& loadedEntity,
	EntityId targetEntity,
	const std::filesystem::path& restoreSourcePrefabPath)
{
	if (!loadedEntity.HasMesh || loadedEntity.MeshAssetPath.empty())
	{
		return;
	}
	MarkResourcePreparing(loadedEntity.MeshAssetPath, Resources::ResourceKind::Mesh);

	const uint64_t generation = m_RuntimeAssetRegistry.NextGeneration(loadedEntity.MeshAssetPath);
	Asset::AssetImportRequest request;
	request.SourcePath = loadedEntity.MeshAssetPath;
	request.Generation = generation;
	request.SceneGeneration = m_AssetSceneGeneration;
	request.IsReload = false;
	request.Placement.HasPlacement = false;
	request.Restore.HasTargetEntity = true;
	request.Restore.TargetEntity = targetEntity;
	request.Restore.RestoreGeneration = ++m_MeshRestoreGenerations[targetEntity];
	request.Restore.EntityName = loadedEntity.Name;
	request.Restore.LocalTransform = loadedEntity.Transform;
	request.Restore.MeshEnabled = loadedEntity.MeshEnabled;
	request.Restore.MaterialOverrides = loadedEntity.MaterialOverrides;
	request.Restore.HasAnimator = loadedEntity.HasAnimator;
	request.Restore.AnimatorEnabled = loadedEntity.AnimatorEnabled;
	request.Restore.Animator = loadedEntity.Animator;

	std::optional<std::filesystem::file_time_type> sourcePrefabWriteTime;
	if (!restoreSourcePrefabPath.empty())
	{
		std::error_code writeTimeError;
		sourcePrefabWriteTime = std::filesystem::last_write_time(restoreSourcePrefabPath, writeTimeError);
		if (writeTimeError)
		{
			sourcePrefabWriteTime.reset();
		}
	}

	m_MeshRestoreRequests[targetEntity] = MeshRestoreRequestState{
		.Generation = request.Restore.RestoreGeneration,
		.SourcePath = loadedEntity.MeshAssetPath,
		.SourcePrefabPath = restoreSourcePrefabPath,
		.SourcePrefabWriteTime = sourcePrefabWriteTime,
		.ExpectedCurrentMeshSignature = BuildMeshRestoreSignature(targetEntity),
		.Pending = true,
		.Failed = false,
		.Cancelled = false,
		.Conflicted = false,
		.Message = std::format("Queued async Mesh restore: {}", loadedEntity.MeshAssetPath.filename().string())
	};
	m_AssetImportService.Queue(std::move(request));

	AppendAssetLog(std::format("Scene model restore queued: {}", loadedEntity.MeshAssetPath.string()));
}

void Engine::QueueModelImportFromDrop(const std::filesystem::path& sourcePath, Editor::AssetDropTarget target)
{
	if (!Asset::IsModelAssetPath(sourcePath))
	{
		AppendAssetLog(std::format("Unsupported drop ignored: {}", sourcePath.string()));
		return;
	}

	const Camera& placementCamera = target == Editor::AssetDropTarget::Scene ? m_SceneCamera : m_Camera;
	QueueModelImport(sourcePath, placementCamera, false);
}

void Engine::QueueModelReload(const std::filesystem::path& sourcePath, const std::filesystem::path& changedPath)
{
	if (m_RuntimeAssetRegistry.GetEntities(sourcePath).empty())
	{
		return;
	}
	MarkResourcePreparing(sourcePath, Resources::ResourceKind::Mesh);

	Asset::AssetImportPlacement placement;
	placement.HasPlacement = false;

	const uint64_t generation = m_RuntimeAssetRegistry.NextGeneration(sourcePath);
	Asset::AssetImportRequest request;
	request.SourcePath = sourcePath;
	request.Generation = generation;
	request.SceneGeneration = m_AssetSceneGeneration;
	request.IsReload = true;
	request.Placement = placement;
	m_AssetImportService.Queue(std::move(request));
	AppendAssetLog(std::format("Hot reload queued: {} changed {}", sourcePath.string(), changedPath.filename().string()));
}

void Engine::DrainCompletedAssetJobs()
{
	Asset::AssetImportResult result;
	while (m_AssetImportService.TryPopCompleted(result))
	{
		if (result.SceneGeneration != m_AssetSceneGeneration)
		{
			AppendAssetLog(std::format("Stale import discarded: {}", result.SourcePath.string()));
			continue;
		}

		if (result.IsReload && result.Generation != m_RuntimeAssetRegistry.GetGeneration(result.SourcePath))
		{
			AppendAssetLog(std::format("Stale reload discarded: {}", result.SourcePath.string()));
			continue;
		}

		if (!result.IsReload && result.Restore.HasTargetEntity && result.Restore.RestoreGeneration != 0 && IsStaleMeshRestoreResult(result))
		{
			AppendAssetLog(std::format(
				"Stale scene model restore discarded for entity {}: {}",
				result.Restore.TargetEntity,
				result.SourcePath.string()));
			continue;
		}

		if (!result.Success)
		{
			MarkMeshRestoreFailed(result);
			m_RuntimeAssetRegistry.UpdateStatus(result.SourcePath, result.ErrorMessage);
			MarkResourceFailed(result.SourcePath, Resources::ResourceKind::Mesh, result.ErrorMessage);
			AppendAssetLog(std::format("Asset job failed: {}", result.ErrorMessage));
			for (const std::string& diagnostic : result.Diagnostics)
			{
				AppendAssetLog(std::format("  {}", diagnostic));
			}
			continue;
		}

		if (result.IsReload)
		{
			const uintmax_t loadedBytes = result.Mesh
				? (result.Mesh->Vertices.size() * sizeof(Asset::StaticMeshVertex)) + (result.Mesh->Indices.size() * sizeof(uint32_t))
				: 0;
			MarkResourceLoaded(result.SourcePath, Resources::ResourceKind::Mesh, loadedBytes);
			ApplyReloadedAsset(std::move(result));
		}
		else
		{
			const uintmax_t loadedBytes = result.Mesh
				? (result.Mesh->Vertices.size() * sizeof(Asset::StaticMeshVertex)) + (result.Mesh->Indices.size() * sizeof(uint32_t))
				: 0;
			MarkResourceLoaded(result.SourcePath, Resources::ResourceKind::Mesh, loadedBytes);
			ApplyImportedModel(std::move(result));
		}
	}
}

void Engine::ApplyImportedModel(Asset::AssetImportResult result, bool allowMeshRestoreConflict)
{
	if (!result.Mesh)
	{
		return;
	}

	BoundsComponent bounds;
	bounds.LocalMin = result.LocalMin;
	bounds.LocalMax = result.LocalMax;

	if (result.Restore.HasTargetEntity)
	{
		const EntityId entityId = result.Restore.TargetEntity;
		if (!m_Scene.ContainsEntity(entityId))
		{
			AppendAssetLog(std::format("Scene restore skipped for missing entity {}: {}", entityId, result.SourcePath.string()));
			return;
		}

		if (result.Restore.RestoreGeneration != 0)
		{
			if (IsStaleMeshRestoreResult(result))
			{
				AppendAssetLog(std::format(
					"Stale scene model restore discarded for entity {}: {}",
					entityId,
					result.SourcePath.string()));
				return;
			}
		}

		std::string conflictReason;
		if (!allowMeshRestoreConflict && HasMeshRestoreConflict(result, conflictReason))
		{
			if (auto restoreIt = m_MeshRestoreRequests.find(entityId); restoreIt != m_MeshRestoreRequests.end())
			{
				restoreIt->second.Pending = false;
				restoreIt->second.Failed = false;
				restoreIt->second.Cancelled = false;
				restoreIt->second.Conflicted = true;
				restoreIt->second.ImportedVertexCount = result.Mesh ? result.Mesh->Vertices.size() : 0;
				restoreIt->second.ImportedIndexCount = result.Mesh ? result.Mesh->Indices.size() : 0;
				restoreIt->second.ImportedMaterialCount = result.Mesh ? result.Mesh->Materials.size() : 0;
				restoreIt->second.MaterialDiffLines = result.Mesh
					? BuildMeshRestoreMaterialDiff(entityId, *result.Mesh, 32)
					: std::vector<std::string>{ "Stored restore has no imported Mesh material data." };
				restoreIt->second.MaterialDiffRows = result.Mesh
					? BuildMeshRestoreMaterialDiffRows(entityId, *result.Mesh, 64)
					: std::vector<Editor::MeshRestoreMaterialDiffRow>{ Editor::MeshRestoreMaterialDiffRow{
						.Field = "Mesh",
						.CurrentValue = "Current Entity Mesh",
						.RestoreValue = "<no imported Mesh material data>"
					} };
				restoreIt->second.Message = conflictReason;
			}
			AppendAssetLog(std::format("Mesh restore conflict for entity {}: {}", entityId, conflictReason));
			m_MeshRestoreConflictResults[entityId] = std::move(result);
			return;
		}

		if (!result.Restore.EntityName.empty())
		{
			static_cast<void>(m_Scene.RenameEntity(entityId, result.Restore.EntityName));
		}

		TransformComponent& transform = m_Scene.EnsureTransformComponent(entityId);
		transform.LocalTransform = result.Restore.LocalTransform;
		transform.UpdateWorld();

		if (!result.Restore.MaterialOverrides.empty())
		{
			ApplyMaterialOverrides(*result.Mesh, result.Restore.MaterialOverrides);
			static_cast<void>(Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(
				*result.Mesh,
				result.MaterialTextures,
				&result.MaterialTransparency,
				[this](std::string_view message)
				{
					AppendAssetLog(std::string(message));
				}));
		}

		DestroyTextureResourcesForEntity(entityId);
		for (const auto& removedSourcePath : m_RuntimeAssetRegistry.UnregisterEntity(entityId))
		{
			m_AssetHotReloadService.UnwatchLoadedAsset(removedSourcePath);
		}

		m_Scene.ReplaceEntityModel(entityId, std::move(result.Mesh), std::move(result.MaterialTextures), bounds);
		static_cast<void>(m_Scene.SetMeshEnabled(entityId, result.Restore.MeshEnabled));
		if (result.GenerateColliders)
		{
			CreateGeneratedColliderForImportedModel(entityId, bounds);
		}
		if (result.Restore.HasAnimator)
		{
			AnimatorComponent& animator = m_Scene.EnsureAnimatorComponent(entityId);
			animator = result.Restore.Animator;
			static_cast<void>(m_Scene.SetAnimatorEnabled(entityId, result.Restore.AnimatorEnabled));
			if (const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId); meshAsset && !meshAsset->Animations.empty())
			{
				animator.CurrentClipIndex = (std::min)(animator.CurrentClipIndex, static_cast<uint32_t>(meshAsset->Animations.size() - 1));
			}
			else
			{
				animator.CurrentClipIndex = 0;
			}
		}
		else
		{
			static_cast<void>(m_Scene.RemoveAnimatorComponent(entityId));
		}

		if (std::ranges::find(m_RenderState.RenderEntities, entityId) == m_RenderState.RenderEntities.end())
		{
			m_RenderState.RenderEntities.push_back(entityId);
		}
		if (m_Scene.GetPrimaryRenderableEntity() == InvalidEntityId)
		{
			m_Scene.SetPrimaryRenderableEntity(entityId);
		}
		m_RenderState.EntityMaterialTransparency[entityId] = result.MaterialTransparency;

		const Asset::StaticMeshAsset* importedMesh = m_Scene.GetMeshAsset(entityId);
		if (importedMesh && !EnsureGeometryBufferCapacity(importedMesh->Vertices.size(), importedMesh->Indices.size()))
		{
			AppendAssetLog(std::format("Geometry buffer resize failed for restored entity {}", entityId));
		}

		if (!CreateTextureResourcesForEntity(entityId))
		{
			AppendAssetLog(std::format("GPU texture upload failed for restored entity {}", entityId));
		}

		std::vector<std::filesystem::path> watchedTexturePaths;
		if (const auto* materialTextures = m_Scene.GetMaterialTextures(entityId))
		{
			watchedTexturePaths = CollectWatchedAssetPaths(result.SourcePath, *materialTextures);
		}
		m_RuntimeAssetRegistry.RegisterEntity(result.SourcePath, entityId, watchedTexturePaths, "Scene Loaded");
		m_AssetHotReloadService.WatchLoadedAsset(result.SourcePath, watchedTexturePaths);
		m_MeshRestoreRequests.erase(entityId);
		m_MeshRestoreConflictResults.erase(entityId);
		MarkPhysicsActorDirty(entityId);
		AppendAssetLog(std::format("Restored scene model entity {}: {}", entityId, result.SourcePath.string()));
		return;
	}

	const std::string entityName = result.SourcePath.stem().string().empty()
		? "Imported Model"
		: result.SourcePath.stem().string();
	const EntityId entityId = m_Scene.CreateModelEntity(
		entityName,
		BuildDroppedModelTransform(result),
		std::move(result.Mesh),
		std::move(result.MaterialTextures),
		bounds);
	if (result.GenerateColliders)
	{
		CreateGeneratedColliderForImportedModel(entityId, bounds);
	}

	m_RenderState.RenderEntities.push_back(entityId);
	m_RenderState.EntityMaterialTransparency[entityId] = result.MaterialTransparency;
	m_Scene.SetSelectedEntity(entityId);

	const Asset::StaticMeshAsset* importedMesh = m_Scene.GetMeshAsset(entityId);
	if (importedMesh && !EnsureGeometryBufferCapacity(importedMesh->Vertices.size(), importedMesh->Indices.size()))
	{
		AppendAssetLog(std::format("Geometry buffer resize failed for entity {}", entityId));
	}

	if (!CreateTextureResourcesForEntity(entityId))
	{
		AppendAssetLog(std::format("GPU texture upload failed for entity {}", entityId));
	}

	std::vector<std::filesystem::path> watchedTexturePaths = CollectWatchedAssetPaths(result.SourcePath, *m_Scene.GetMaterialTextures(entityId));
	m_RuntimeAssetRegistry.RegisterEntity(result.SourcePath, entityId, watchedTexturePaths, "Loaded");
	m_AssetHotReloadService.WatchLoadedAsset(result.SourcePath, watchedTexturePaths);
	MarkPhysicsActorDirty(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("Imported model entity {}: {}", entityId, result.SourcePath.string()));
}

void Engine::ApplyReloadedAsset(Asset::AssetImportResult result)
{
	if (!result.Mesh)
	{
		return;
	}

	const std::vector<EntityId> entities = m_RuntimeAssetRegistry.GetEntities(result.SourcePath);
	if (entities.empty())
	{
		return;
	}

	BoundsComponent bounds;
	bounds.LocalMin = result.LocalMin;
	bounds.LocalMax = result.LocalMax;

	for (EntityId entityId : entities)
	{
		const bool hadAnimator = m_Scene.GetAnimatorComponent(entityId) != nullptr;
		const bool animatorWasEnabled = hadAnimator && m_Scene.IsAnimatorEnabled(entityId);
		std::vector<Asset::StaticMeshMaterial> existingMaterialOverrides;
		if (const Asset::StaticMeshAsset* existingMesh = m_Scene.GetMeshAsset(entityId))
		{
			existingMaterialOverrides.assign(existingMesh->Materials.begin(), existingMesh->Materials.end());
		}
		auto meshCopy = std::make_unique<Asset::StaticMeshAsset>(*result.Mesh);
		std::vector<CpuMaterialTexture> materialTextures = result.MaterialTextures;
		std::vector<bool> materialTransparency = result.MaterialTransparency;
		if (!existingMaterialOverrides.empty())
		{
			ApplyMaterialOverrides(*meshCopy, existingMaterialOverrides);
			static_cast<void>(Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(
				*meshCopy,
				materialTextures,
				&materialTransparency,
				[this](std::string_view message)
				{
					AppendAssetLog(std::string(message));
				}));
		}
		m_Scene.ReplaceEntityModel(entityId, std::move(meshCopy), std::move(materialTextures), bounds);
		if (result.GenerateColliders)
		{
			CreateGeneratedColliderForImportedModel(entityId, bounds);
		}
		m_RenderState.EntityMaterialTransparency[entityId] = materialTransparency;
		const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
		if (!hadAnimator)
		{
			static_cast<void>(m_Scene.RemoveAnimatorComponent(entityId));
		}
		else if (AnimatorComponent* animator = m_Scene.GetAnimatorComponent(entityId);
			animator && meshAsset && !meshAsset->Animations.empty())
		{
			static_cast<void>(m_Scene.SetAnimatorEnabled(entityId, animatorWasEnabled));
			animator->CurrentClipIndex = (std::min)(animator->CurrentClipIndex, static_cast<uint32_t>(meshAsset->Animations.size() - 1));
		}
		if (meshAsset && !EnsureGeometryBufferCapacity(meshAsset->Vertices.size(), meshAsset->Indices.size()))
		{
			AppendAssetLog(std::format("Geometry buffer resize failed for entity {}", entityId));
		}
		if (!RecreateTextureResourcesForEntity(entityId))
		{
			AppendAssetLog(std::format("GPU texture reload failed for entity {}", entityId));
		}
		MarkPhysicsActorDirty(entityId);
	}

	std::vector<std::filesystem::path> watchedTexturePaths;
	for (EntityId entityId : entities)
	{
		const auto* materialTextures = m_Scene.GetMaterialTextures(entityId);
		const std::vector<std::filesystem::path> entityWatchedTexturePaths = materialTextures
			? CollectWatchedAssetPaths(result.SourcePath, *materialTextures)
			: std::vector<std::filesystem::path>{};
		for (const std::filesystem::path& path : entityWatchedTexturePaths)
		{
			if (std::ranges::find(watchedTexturePaths, path) == watchedTexturePaths.end())
			{
				watchedTexturePaths.push_back(path);
			}
		}
		m_RuntimeAssetRegistry.RegisterEntity(result.SourcePath, entityId, entityWatchedTexturePaths, "Reloaded");
	}
	m_AssetHotReloadService.WatchLoadedAsset(result.SourcePath, watchedTexturePaths);
	AppendAssetLog(std::format("Hot reloaded {} entity instance(s): {}", entities.size(), result.SourcePath.string()));
}

void Engine::HandleDroppedFiles(HDROP dropHandle)
{
	const UINT fileCount = DragQueryFileW(dropHandle, 0xFFFFFFFF, nullptr, 0);
	for (UINT fileIndex = 0; fileIndex < fileCount; ++fileIndex)
	{
		const UINT characterCount = DragQueryFileW(dropHandle, fileIndex, nullptr, 0);
		std::wstring pathText(characterCount + 1, L'\0');
		DragQueryFileW(dropHandle, fileIndex, pathText.data(), characterCount + 1);
		pathText.resize(characterCount);
		QueueModelImportFromDrop(std::filesystem::path(pathText), Editor::AssetDropTarget::External);
	}
	DragFinish(dropHandle);
}

void Engine::OpenAssetPath(const std::filesystem::path& path)
{
	if (path.extension() == ".scene")
	{
		static_cast<void>(OpenSceneFromPath(path, true));
		return;
	}
	if (path.extension() == ".prefab")
	{
		static_cast<void>(InstantiatePrefabAsset(path));
		return;
	}
	if (Asset::IsSkyboxAssetPath(path) || Rendering::IsSkyboxAssetPath(path))
	{
		static_cast<void>(ApplySkyboxAsset(path));
		return;
	}

	ShellExecuteW(m_hMainWnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Engine::RevealAssetPath(const std::filesystem::path& path) const
{
	std::error_code errorCode;
	if (std::filesystem::is_directory(path, errorCode))
	{
		ShellExecuteW(m_hMainWnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}

	std::wstring parameters = L"/select,\"";
	parameters.append(path.wstring());
	parameters.push_back(L'"');
	ShellExecuteW(m_hMainWnd, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
}

void Engine::AppendAssetLog(std::string message)
{
	LogEngineTrace(message);
	if (m_StartupOptions.SmokeLogPath)
	{
		std::error_code errorCode;
		const std::filesystem::path parentPath = m_StartupOptions.SmokeLogPath->parent_path();
		if (!parentPath.empty())
		{
			std::filesystem::create_directories(parentPath, errorCode);
		}

		std::ofstream smokeLog(*m_StartupOptions.SmokeLogPath, std::ios::app);
		if (smokeLog)
		{
			smokeLog << message << '\n';
		}
	}
	m_AssetLogLines.push_back(std::move(message));
	if (m_AssetLogLines.size() > 200)
	{
		m_AssetLogLines.erase(m_AssetLogLines.begin(), m_AssetLogLines.begin() + static_cast<std::ptrdiff_t>(m_AssetLogLines.size() - 200));
	}
}

void Engine::RequestRendererRoadmapHealthLog() noexcept
{
	m_RoadmapHealthLogPending = true;
}

void Engine::LogRendererRoadmapHealthSnapshot()
{
	if (!m_RoadmapHealthLogPending || m_LastCompletedRenderFrameStats.FrameIndex == 0)
	{
		return;
	}

	m_RoadmapHealthLogPending = false;

	const Resources::ResourceManagerStats resourceStats = m_ResourceManager.GetStats();
	const Materials::MaterialResourceStats materialStats = BuildSceneMaterialResourceStats();
	const Materials::ShaderVariantCacheStats variantStats = m_ShaderVariantCache.GetStats();
	const Rendering::RenderGraphStats graphStats = m_RenderGraph.GetStats();
	const Rendering::RenderFrameStats& renderStats = m_LastCompletedRenderFrameStats;
	const Rendering::ShadowStats shadowStats = Rendering::ShadowSystem::BuildStats(m_ShadowFrameData, m_ShadowSettings);
	const bool hdrTargetAvailable =
		(m_Graphics.CurrentApi == GraphicsAPI::DirectX12 && m_StaticMeshRenderer.Dx12.Deferred.HdrColorTexture != nullptr) ||
		(m_Graphics.CurrentApi == GraphicsAPI::Vulkan && m_StaticMeshRenderer.Vulkan.Deferred.HdrColorImage != VK_NULL_HANDLE);
	const Rendering::PostProcessStats postStats = Rendering::PostProcessSystem::BuildStats(m_PostProcessSettings, m_Graphics.CurrentApi, m_RenderMode, hdrTargetAvailable);
	const uint32_t sceneLightCount = RenderSystem::CountSceneRenderableLights(GetRuntimeScene());
	const uint32_t forwardLightUsedCount = sceneLightCount == 0
		? 1u
		: (std::min)(sceneLightCount, kMaxForwardGpuLights);
	const uint32_t forwardLightTruncatedCount = sceneLightCount > kMaxForwardGpuLights
		? sceneLightCount - kMaxForwardGpuLights
		: 0u;

	AppendAssetLog(std::format(
		"Renderer roadmap health [{} | {} | frame {}]",
		GraphicsApiToString(m_Graphics.CurrentApi),
		RenderModeToString(m_RenderMode),
		renderStats.FrameIndex));
	AppendAssetLog(std::format(
		"  1 Resource system: {} group(s), {} declared, {} loaded, {} failed",
		resourceStats.GroupCount,
		resourceStats.ResourceCount,
		resourceStats.LoadedCount,
		resourceStats.FailedCount));
	AppendAssetLog(std::format(
		"  2 Materials/variants: {} material(s), {} variant(s), {} request(s)",
		materialStats.MaterialCount,
		variantStats.VariantCount,
		variantStats.RequestCount));
	AppendAssetLog(std::format(
		"  3 RenderGraph: {} / {} pass(es) enabled, deferred {}, HDR {}",
		graphStats.EnabledPassCount,
		graphStats.PassCount,
		graphStats.UsesDeferred ? "yes" : "no",
		graphStats.UsesHdr ? "yes" : "no"));
	AppendAssetLog(std::format(
		"  4 Shadows: enabled {}, caster {}, {} shadow draw call(s)",
		shadowStats.Enabled ? "yes" : "no",
		shadowStats.HasDirectionalCaster ? "yes" : "no",
		renderStats.ShadowDrawCallCount));
	AppendAssetLog(std::format(
		"  5 HDR/post: HDR target {}, tone map {}, exposure {:.2f}",
		postStats.UsesHdrTarget ? "yes" : "no",
		postStats.ToneMappingEnabled ? postStats.ToneMapper : std::string_view("off"),
		postStats.Exposure));
	AppendAssetLog(std::format(
		"  6 Lights: Forward {} / {} used, {} truncated, Deferred {} active / {} capacity",
		forwardLightUsedCount,
		kMaxForwardGpuLights,
		forwardLightTruncatedCount,
		m_StaticMeshRenderer.DeferredLightCount,
		m_StaticMeshRenderer.DeferredLightBufferCapacity));
	AppendAssetLog(std::format(
		"  7 Deferred tiles: {} viewport pass(es), {} tile(s), {} light reference(s), max {}",
		renderStats.DeferredTileViewportCount,
		renderStats.DeferredTileCountTotal,
		renderStats.DeferredTileLightReferenceCount,
		renderStats.DeferredMaxTileLightCount));
	AppendAssetLog(std::format(
		"  8 Pass timings: {} timed pass(es), {:.3f} ms exclusive sum",
		graphStats.TimedPassCount,
		graphStats.TotalCpuMs));
	AppendAssetLog(std::format(
		"  9 Render stats: {} draw call(s), {} triangle(s), {} instance(s)",
		renderStats.DrawCallCount,
		renderStats.SubmittedTriangleCount,
		renderStats.SubmittedInstanceCount));
	AppendAssetLog(std::format(
		" 10 Frustum culling: {}, {} request(s), {} test(s), {} culled result(s)",
		m_ViewFrustumCullingEnabled ? "on" : "off",
		renderStats.ViewCullingRequestCount,
		renderStats.ViewCullingTestCount,
		renderStats.ViewCulledEntityCount));
}

void Engine::ConfigureResourceSystem()
{
	m_ResourceManager.Reset();
	const std::filesystem::path assetRoot = m_Project
		? (m_Project->RootPath / m_Project->AssetRoot).lexically_normal()
		: std::filesystem::path("Assets").lexically_normal();
	const std::string groupName = m_Project ? "Project" : "Development";

	m_ResourceManager.SetRootPath(assetRoot);
	m_ResourceManager.EnsureGroup(groupName);
	m_ResourceManager.AddResourceLocation(groupName, assetRoot, "FileSystem", true);
	const size_t declaredCount = m_ResourceManager.DeclareResourcesFromDirectory(assetRoot, groupName);
	const Resources::ResourceManagerStats stats = m_ResourceManager.GetStats();
	AppendAssetLog(std::format(
		"Resource system mounted {} [{} group(s), {} declared resource(s), {:.2f} MB indexed].",
		assetRoot.string(),
		stats.GroupCount,
		declaredCount,
		static_cast<double>(stats.DeclaredBytes) / (1024.0 * 1024.0)));
}

Resources::ResourceHandle Engine::DeclareResourceForPath(const std::filesystem::path& sourcePath, Resources::ResourceKind kind)
{
	if (auto existing = m_ResourceManager.FindByPath(sourcePath))
	{
		return *existing;
	}

	std::error_code errorCode;
	const bool regularFile = std::filesystem::is_regular_file(sourcePath, errorCode);
	const uintmax_t sizeBytes = regularFile ? std::filesystem::file_size(sourcePath, errorCode) : 0;
	const std::string groupName = m_Project ? "Project" : "Development";
	return m_ResourceManager.DeclareResource(Resources::ResourceDeclaration{
		.Name = sourcePath.filename().string(),
		.SourcePath = sourcePath,
		.Kind = kind,
		.GroupName = groupName,
		.SizeBytes = errorCode ? 0 : sizeBytes
		});
}

void Engine::MarkResourcePreparing(const std::filesystem::path& sourcePath, Resources::ResourceKind kind)
{
	const Resources::ResourceHandle handle = DeclareResourceForPath(sourcePath, kind);
	if (handle)
	{
		m_ResourceManager.MarkPreparing(handle);
	}
}

void Engine::MarkResourceLoaded(const std::filesystem::path& sourcePath, Resources::ResourceKind kind, uintmax_t loadedBytes)
{
	const Resources::ResourceHandle handle = DeclareResourceForPath(sourcePath, kind);
	if (handle)
	{
		m_ResourceManager.MarkLoaded(handle, loadedBytes);
	}
}

void Engine::MarkResourceFailed(const std::filesystem::path& sourcePath, Resources::ResourceKind kind, std::string_view errorMessage)
{
	const Resources::ResourceHandle handle = DeclareResourceForPath(sourcePath, kind);
	if (handle)
	{
		m_ResourceManager.MarkFailed(handle, std::string(errorMessage));
	}
}

void Engine::TouchShaderVariant(EntityId entityId, size_t materialIndex, bool useDeferredLighting)
{
	const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const bool transparent = IsMaterialTransparent(entityId, materialIndex);
	const bool deferredOpaque = m_RenderMode == RenderMode::Deferred && !transparent;
	const bool deferredGeometry = deferredOpaque && !useDeferredLighting;
	const bool deferredLighting = deferredOpaque && useDeferredLighting;
	const bool skinned = meshAsset->IsAnimated && !meshAsset->Bones.empty();
	static_cast<void>(m_ShaderVariantCache.GetOrCreate(Materials::BuildShaderVariantKey(
		m_Graphics.CurrentApi,
		m_RenderMode,
		meshAsset->Materials[materialIndex],
		m_MaterialDebugView,
		transparent,
		skinned,
		false,
		deferredGeometry,
		deferredLighting)));
}

Materials::MaterialResourceStats Engine::BuildSceneMaterialResourceStats() const
{
	Materials::MaterialResourceStats totalStats;
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(entity.Id);
		if (!meshAsset)
		{
			continue;
		}

		const std::vector<Materials::MaterialResource> materialResources = Materials::BuildMaterialResources(*meshAsset);
		const Materials::MaterialResourceStats stats = Materials::BuildMaterialResourceStats(materialResources);
		totalStats.MaterialCount += stats.MaterialCount;
		totalStats.TextureSlotCount += stats.TextureSlotCount;
		totalStats.OverrideSlotCount += stats.OverrideSlotCount;
		totalStats.EmbeddedSlotCount += stats.EmbeddedSlotCount;
		totalStats.PbrCount += stats.PbrCount;
		totalStats.PhongCount += stats.PhongCount;
		totalStats.UnlitCount += stats.UnlitCount;
	}
	return totalStats;
}

Math::Transform Engine::BuildDroppedModelTransform(const Asset::AssetImportResult& result) const
{
	const DirectX::XMFLOAT3 center = {
		(result.LocalMin.x + result.LocalMax.x) * 0.5f,
		(result.LocalMin.y + result.LocalMax.y) * 0.5f,
		(result.LocalMin.z + result.LocalMax.z) * 0.5f
	};
	const float extentX = result.LocalMax.x - result.LocalMin.x;
	const float extentY = result.LocalMax.y - result.LocalMin.y;
	const float extentZ = result.LocalMax.z - result.LocalMin.z;
	const float maxExtent = (std::max)(1.0f, (std::max)(extentX, (std::max)(extentY, extentZ)));
	const float distance = (std::max)(maxExtent * 2.25f, 6.0f);

	DirectX::XMVECTOR cameraPosition = DirectX::XMLoadFloat3(&result.Placement.CameraPosition);
	DirectX::XMVECTOR cameraForward = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&result.Placement.CameraForward));
	const DirectX::XMVECTOR target = DirectX::XMVectorAdd(cameraPosition, DirectX::XMVectorScale(cameraForward, distance));
	const DirectX::XMVECTOR localCenter = DirectX::XMLoadFloat3(&center);
	DirectX::XMFLOAT3 translation = {};
	DirectX::XMStoreFloat3(&translation, DirectX::XMVectorSubtract(target, localCenter));

	return Math::Transform(translation, Math::IdentityQuaternion(), Math::OneVector3());
}

namespace
{
	[[nodiscard]] BoundsComponent ComputeMeshBounds(const Asset::StaticMeshAsset& mesh)
	{
		BoundsComponent bounds;
		if (mesh.Vertices.empty())
		{
			bounds.LocalMin = { -0.5f, -0.5f, -0.5f };
			bounds.LocalMax = { 0.5f, 0.5f, 0.5f };
			return bounds;
		}

		bounds.LocalMin = mesh.Vertices.front().Position;
		bounds.LocalMax = mesh.Vertices.front().Position;
		for (const Asset::StaticMeshVertex& vertex : mesh.Vertices)
		{
			bounds.LocalMin.x = (std::min)(bounds.LocalMin.x, vertex.Position.x);
			bounds.LocalMin.y = (std::min)(bounds.LocalMin.y, vertex.Position.y);
			bounds.LocalMin.z = (std::min)(bounds.LocalMin.z, vertex.Position.z);
			bounds.LocalMax.x = (std::max)(bounds.LocalMax.x, vertex.Position.x);
			bounds.LocalMax.y = (std::max)(bounds.LocalMax.y, vertex.Position.y);
			bounds.LocalMax.z = (std::max)(bounds.LocalMax.z, vertex.Position.z);
		}
		return bounds;
	}

	void ApplyMaterialOverrides(Asset::StaticMeshAsset& mesh, const std::vector<Asset::StaticMeshMaterial>& overrides)
	{
		const size_t overrideCount = (std::min)(mesh.Materials.size(), overrides.size());
		for (size_t materialIndex = 0; materialIndex < overrideCount; ++materialIndex)
		{
			const std::string importedName = mesh.Materials[materialIndex].Name;
			mesh.Materials[materialIndex] = overrides[materialIndex];
			if (mesh.Materials[materialIndex].Name.empty())
			{
				mesh.Materials[materialIndex].Name = importedName;
			}
		}
	}
}

std::vector<std::filesystem::path> Engine::CollectWatchedTexturePaths(const std::vector<CpuMaterialTexture>& materialTextures)
{
	std::vector<std::filesystem::path> paths;
	for (const auto& materialTexture : materialTextures)
	{
		for (const CpuMaterialTextureSlot& slotTexture : materialTexture.Slots)
		{
			if (!slotTexture.Path.empty() && std::ranges::find(paths, slotTexture.Path) == paths.end())
			{
				paths.push_back(slotTexture.Path);
			}
		}
	}
	return paths;
}

std::vector<std::filesystem::path> Engine::CollectWatchedAssetPaths(const std::filesystem::path& sourcePath, const std::vector<CpuMaterialTexture>& materialTextures)
{
	std::vector<std::filesystem::path> paths = CollectWatchedTexturePaths(materialTextures);
	const std::filesystem::path settingsPath = Asset::AssetImportSettingsService::GetSettingsPathForAsset(sourcePath);
	if (!settingsPath.empty() && std::ranges::find(paths, settingsPath) == paths.end())
	{
		paths.push_back(settingsPath);
	}
	return paths;
}

bool Engine::SaveCurrentScene()
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Save skipped: only Project Scene can be saved.");
		return false;
	}

	if (m_CurrentScenePath.empty())
	{
		return SaveCurrentSceneAs();
	}

	const std::unordered_set<EntityId> excludedEntities = BuildRuntimeExpandedSceneReferenceSet();
	std::string errorMessage;
	if (!ScenePersistence::ScenePersistenceService::SaveScene(
		m_Scene,
		m_RenderState,
		*m_Project,
		m_CurrentScenePath,
		m_AmbientColor,
		m_AmbientIntensity,
		m_Exposure,
		m_SkyboxSettings,
		errorMessage,
		&excludedEntities))
	{
		AppendAssetLog(std::format("Scene save failed: {}", errorMessage));
		const std::wstring message(errorMessage.begin(), errorMessage.end());
		MessageBoxW(m_hMainWnd, message.c_str(), L"Save Scene Error", MB_OK | MB_ICONERROR);
		return false;
	}

	SetSceneDirty(false);
	RebuildWindowTitleBase();
	ResetFpsCounter();
	m_AssetFileSystem.RequestRefresh();
	AppendAssetLog(std::format("Scene saved: {}", m_CurrentScenePath.string()));
	return true;
}

bool Engine::SaveCurrentSceneAs()
{
	const std::optional<std::filesystem::path> selectedPath = ShowSaveSceneDialog();
	if (!selectedPath)
	{
		return false;
	}

	m_CurrentScenePath = *selectedPath;
	return SaveCurrentScene();
}

void Engine::UpdateAutosave(float deltaTime)
{
	if (!m_AutosaveEnabled || IsRuntimeMode() || IsRuntimePlaying())
	{
		return;
	}

	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene || !m_SceneDirty)
	{
		if (!m_SceneDirty)
		{
			m_AutosaveElapsedSeconds = 0.0f;
		}
		return;
	}

	m_AutosaveElapsedSeconds += (std::max)(0.0f, deltaTime);
	if (m_AutosaveElapsedSeconds < m_AutosaveIntervalSeconds)
	{
		return;
	}

	m_AutosaveElapsedSeconds = 0.0f;
	static_cast<void>(SaveAutosaveSnapshot());
}

bool Engine::SaveAutosaveSnapshot()
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		m_LastAutosaveSucceeded = false;
		m_AutosaveStatusMessage = "Autosave skipped: no project scene is active.";
		return false;
	}

	const std::filesystem::path sceneSource = m_CurrentScenePath.empty() ? GetDefaultScenePath() : m_CurrentScenePath;
	std::string sceneStem = sceneSource.stem().string();
	if (sceneStem.empty())
	{
		sceneStem = "Untitled";
	}

	const std::filesystem::path autosaveDirectory = m_Project->RootPath / "Temp" / "Autosaves";
	std::error_code directoryError;
	std::filesystem::create_directories(autosaveDirectory, directoryError);
	if (directoryError)
	{
		m_LastAutosaveSucceeded = false;
		m_AutosaveStatusMessage = std::format("Autosave failed: {}", directoryError.message());
		AppendAssetLog(m_AutosaveStatusMessage);
		return false;
	}

	const std::filesystem::path autosavePath = autosaveDirectory / std::format("{}.autosave.scene", sceneStem);
	const std::unordered_set<EntityId> excludedEntities = BuildRuntimeExpandedSceneReferenceSet();
	std::string errorMessage;
	if (!ScenePersistence::ScenePersistenceService::SaveScene(
		m_Scene,
		m_RenderState,
		*m_Project,
		autosavePath,
		m_AmbientColor,
		m_AmbientIntensity,
		m_Exposure,
		m_SkyboxSettings,
		errorMessage,
		&excludedEntities))
	{
		m_LastAutosaveSucceeded = false;
		m_LastAutosavePath = autosavePath;
		m_AutosaveStatusMessage = std::format("Autosave failed: {}", errorMessage);
		AppendAssetLog(m_AutosaveStatusMessage);
		return false;
	}

	m_LastAutosaveSucceeded = true;
	m_LastAutosavePath = autosavePath;
	m_AutosaveStatusMessage = std::format("Autosaved dirty scene: {}", autosavePath.filename().string());
	AppendAssetLog(std::format("Autosaved scene snapshot: {}", autosavePath.string()));
	return true;
}

void Engine::SetAutosaveEnabled(bool enabled)
{
	if (m_AutosaveEnabled == enabled)
	{
		return;
	}

	m_AutosaveEnabled = enabled;
	m_AutosaveElapsedSeconds = 0.0f;
	m_AutosaveStatusMessage = enabled ? "Autosave enabled." : "Autosave disabled.";
	AppendAssetLog(m_AutosaveStatusMessage);
}

void Engine::SetAutosaveInterval(float intervalSeconds)
{
	const float clampedInterval = std::clamp(intervalSeconds, 15.0f, 900.0f);
	if (std::abs(m_AutosaveIntervalSeconds - clampedInterval) < 0.001f)
	{
		return;
	}

	m_AutosaveIntervalSeconds = clampedInterval;
	m_AutosaveElapsedSeconds = (std::min)(m_AutosaveElapsedSeconds, m_AutosaveIntervalSeconds);
	m_AutosaveStatusMessage = std::format("Autosave interval set to {:.0f} seconds.", m_AutosaveIntervalSeconds);
	AppendAssetLog(m_AutosaveStatusMessage);
}

bool Engine::OpenSceneFromDialog()
{
	const std::optional<std::filesystem::path> selectedPath = ShowOpenSceneDialog();
	return selectedPath ? OpenSceneFromPath(*selectedPath, true) : false;
}

bool Engine::OpenSceneFromPath(const std::filesystem::path& scenePath, bool promptForDirtyScene)
{
	if (!m_Project)
	{
		AppendAssetLog("Open scene skipped: no project is loaded.");
		return false;
	}

	if (promptForDirtyScene && !ConfirmSaveDirtyScene())
	{
		return false;
	}

	const std::filesystem::path resolvedScenePath = ResolveProjectScenePath(scenePath);
	ScenePersistence::LoadSceneResult loadResult = ScenePersistence::ScenePersistenceService::LoadScene(resolvedScenePath, *m_Project);
	if (!loadResult.Success)
	{
		AppendAssetLog(std::format("Scene load failed: {}", loadResult.ErrorMessage));
		const std::wstring message(loadResult.ErrorMessage.begin(), loadResult.ErrorMessage.end());
		MessageBoxW(m_hMainWnd, message.c_str(), L"Open Scene Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ClearProjectSceneRuntimeState();
	m_CurrentScenePath = resolvedScenePath;
	m_SampleMode = Samples::Benchmark::SampleMode::ProjectScene;
	m_LastSampleMode = m_SampleMode;
	m_AmbientColor = loadResult.AmbientColor;
	m_AmbientIntensity = loadResult.AmbientIntensity;
	m_Exposure = loadResult.Exposure;
	m_SkyboxSettings = loadResult.Skybox;

	std::vector<EntityId> createdEntityIds;
	createdEntityIds.reserve(loadResult.Entities.size());
	for (const ScenePersistence::LoadedSceneEntity& loadedEntity : loadResult.Entities)
	{
		createdEntityIds.push_back(CreateEntityFromLoadedSceneEntity(loadedEntity, {}));
	}
	for (size_t entityIndex = 0; entityIndex < loadResult.Entities.size(); ++entityIndex)
	{
		const ScenePersistence::LoadedSceneEntity& loadedEntity = loadResult.Entities[entityIndex];
		if (!loadedEntity.HasHierarchy || entityIndex >= createdEntityIds.size() || createdEntityIds[entityIndex] == InvalidEntityId)
		{
			continue;
		}
		EntityId parentEntityId = loadedEntity.ParentEntityId;
		if (parentEntityId == InvalidEntityId && loadedEntity.ParentIndex < createdEntityIds.size())
		{
			parentEntityId = createdEntityIds[loadedEntity.ParentIndex];
		}
		if (parentEntityId != InvalidEntityId)
		{
			static_cast<void>(m_Scene.SetParentEntity(createdEntityIds[entityIndex], parentEntityId, false));
		}
		m_Scene.EnsureHierarchyComponent(createdEntityIds[entityIndex]).Expanded = loadedEntity.HierarchyExpanded;
	}
	m_Scene.UpdateWorldTransforms();

	CreateEditorSceneEntities();
	ProcessSceneReferenceAutoLoads(false);
	SyncGameCameraFromSceneEntity();
	RebuildPhysicsWorldFromScene();
	SetSceneDirty(false);
	RebuildWindowTitleBase();
	ResetFpsCounter();
	AppendAssetLog(std::format("Scene loaded: {}", m_CurrentScenePath.string()));
	return true;
}

EntityId Engine::CreateEntityFromLoadedSceneEntity(const ScenePersistence::LoadedSceneEntity& loadedEntity, const std::filesystem::path& prefabSourcePath)
{
	const EntityId entityId = m_Scene.CreateEntity(loadedEntity.Name);
	if (loadedEntity.HasTransform)
	{
		TransformComponent& transform = m_Scene.EnsureTransformComponent(entityId);
		transform.LocalTransform = loadedEntity.Transform;
		transform.UpdateWorld();
	}
	if (loadedEntity.HasEditorState)
	{
		m_Scene.EnsureEditorStateComponent(entityId) = loadedEntity.EditorState;
	}
	if (loadedEntity.HasHierarchy)
	{
		SceneHierarchyComponent& hierarchy = m_Scene.EnsureHierarchyComponent(entityId);
		hierarchy.Expanded = loadedEntity.HierarchyExpanded;
		if (loadedEntity.ParentEntityId != InvalidEntityId && m_Scene.ContainsEntity(loadedEntity.ParentEntityId))
		{
			static_cast<void>(m_Scene.SetParentEntity(entityId, loadedEntity.ParentEntityId, false));
		}
	}

	if (loadedEntity.HasCamera)
	{
		CameraComponent& camera = m_Scene.EnsureCameraComponent(entityId);
		camera = loadedEntity.Camera;
		static_cast<void>(m_Scene.SetCameraEnabled(entityId, loadedEntity.CameraEnabled));
		if (camera.IsGameCamera && m_GameCameraEntity == InvalidEntityId)
		{
			m_GameCameraEntity = entityId;
		}
		else if (camera.IsGameCamera)
		{
			camera.IsGameCamera = false;
		}
	}

	if (loadedEntity.HasLight)
	{
		m_Scene.EnsureLightComponent(entityId) = loadedEntity.Light;
		static_cast<void>(m_Scene.SetLightEnabled(entityId, loadedEntity.LightEnabled));
		if (m_KeyLightEntity == InvalidEntityId)
		{
			m_KeyLightEntity = entityId;
		}
	}

	if (loadedEntity.HasMesh)
	{
		if (loadedEntity.PrimitiveKind != Asset::PrimitiveMeshKind::None)
		{
			static_cast<void>(ApplyPrimitiveMeshToEntity(entityId, loadedEntity.PrimitiveKind, loadedEntity.Transform, false));
			if (!loadedEntity.MaterialOverrides.empty())
			{
				if (Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId))
				{
					ApplyMaterialOverrides(*meshAsset, loadedEntity.MaterialOverrides);
					static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
				}
			}
			static_cast<void>(m_Scene.SetMeshEnabled(entityId, loadedEntity.MeshEnabled));
		}
		else if (!loadedEntity.MeshAssetPath.empty())
		{
			QueueModelImportForSceneEntity(loadedEntity, entityId);
		}
		else
		{
			static_cast<void>(m_Scene.EnsureMeshComponent(entityId));
			static_cast<void>(m_Scene.SetMeshEnabled(entityId, loadedEntity.MeshEnabled));
		}
	}
	else if (loadedEntity.HasAnimator)
	{
		m_Scene.EnsureAnimatorComponent(entityId) = loadedEntity.Animator;
		static_cast<void>(m_Scene.SetAnimatorEnabled(entityId, loadedEntity.AnimatorEnabled));
	}

	if (loadedEntity.HasRigidBody)
	{
		m_Scene.EnsureRigidBodyComponent(entityId) = loadedEntity.RigidBody;
		static_cast<void>(m_Scene.SetRigidBodyEnabled(entityId, loadedEntity.RigidBodyEnabled));
	}
	if (loadedEntity.HasCollider)
	{
		m_Scene.EnsureColliderComponent(entityId) = loadedEntity.Collider;
		static_cast<void>(m_Scene.SetColliderEnabled(entityId, loadedEntity.ColliderEnabled));
	}
	if (loadedEntity.HasPhysicsMaterial)
	{
		m_Scene.EnsurePhysicsMaterialComponent(entityId) = loadedEntity.PhysicsMaterial;
		static_cast<void>(m_Scene.SetPhysicsMaterialEnabled(entityId, loadedEntity.PhysicsMaterialEnabled));
	}
	if (loadedEntity.HasPrefabInstance)
	{
		m_Scene.EnsurePrefabInstanceComponent(entityId) = loadedEntity.PrefabInstance;
		static_cast<void>(m_Scene.SetComponentEnabled<PrefabInstanceComponent>(entityId, loadedEntity.PrefabInstanceEnabled));
	}
	if (!prefabSourcePath.empty())
	{
		PrefabInstanceComponent& prefab = m_Scene.EnsurePrefabInstanceComponent(entityId);
		prefab.PrefabPath = std::filesystem::absolute(prefabSourcePath).lexically_normal();
		prefab.SourceName = loadedEntity.Name;
		prefab.TrackPrefabOverrides = true;
		static_cast<void>(m_Scene.SetComponentEnabled<PrefabInstanceComponent>(entityId, true));
	}
	if (loadedEntity.HasSceneReference)
	{
		m_Scene.EnsureSceneReferenceComponent(entityId) = loadedEntity.SceneReference;
		static_cast<void>(m_Scene.SetComponentEnabled<SceneReferenceComponent>(entityId, loadedEntity.SceneReferenceEnabled));
	}
	if (loadedEntity.HasScript)
	{
		m_Scene.EnsureScriptComponent(entityId) = loadedEntity.Script;
		static_cast<void>(m_Scene.SetComponentEnabled<ScriptComponent>(entityId, loadedEntity.ScriptEnabled));
	}
	if (loadedEntity.HasSprite2D)
	{
		m_Scene.EnsureSprite2DComponent(entityId) = loadedEntity.Sprite2D;
		static_cast<void>(m_Scene.SetComponentEnabled<Sprite2DComponent>(entityId, loadedEntity.Sprite2DEnabled));
	}
	if (loadedEntity.HasUiElement)
	{
		m_Scene.EnsureUiElementComponent(entityId) = loadedEntity.UiElement;
		static_cast<void>(m_Scene.SetComponentEnabled<UiElementComponent>(entityId, loadedEntity.UiElementEnabled));
	}
	if (loadedEntity.HasAudioSource)
	{
		m_Scene.EnsureAudioSourceComponent(entityId) = loadedEntity.AudioSource;
		static_cast<void>(m_Scene.SetComponentEnabled<AudioSourceComponent>(entityId, loadedEntity.AudioSourceEnabled));
	}
	if (loadedEntity.HasNavigationAgent)
	{
		m_Scene.EnsureNavigationAgentComponent(entityId) = loadedEntity.NavigationAgent;
		static_cast<void>(m_Scene.SetComponentEnabled<NavigationAgentComponent>(entityId, loadedEntity.NavigationAgentEnabled));
	}
	if (loadedEntity.HasNetworkIdentity)
	{
		m_Scene.EnsureNetworkIdentityComponent(entityId) = loadedEntity.NetworkIdentity;
		static_cast<void>(m_Scene.SetComponentEnabled<NetworkIdentityComponent>(entityId, loadedEntity.NetworkIdentityEnabled));
	}

	return entityId;
}

std::optional<ScenePersistence::LoadedSceneEntity> Engine::BuildLoadedSceneEntityFromEntity(EntityId entityId) const
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return std::nullopt;
	}

	ScenePersistence::LoadedSceneEntity entity;
	if (const std::string* name = m_Scene.GetEntityName(entityId))
	{
		entity.Name = *name;
	}
	if (const TransformComponent* transform = m_Scene.GetTransformComponent(entityId))
	{
		entity.HasTransform = true;
		entity.Transform = transform->LocalTransform;
	}
	if (const EditorStateComponent* editorState = m_Scene.GetEditorStateComponent(entityId))
	{
		entity.HasEditorState = true;
		entity.EditorState = *editorState;
	}
	if (const SceneHierarchyComponent* hierarchy = m_Scene.GetHierarchyComponent(entityId))
	{
		entity.HasHierarchy = true;
		entity.ParentEntityId = hierarchy->Parent;
		entity.HierarchyExpanded = hierarchy->Expanded;
	}
	if (const MeshComponent* mesh = m_Scene.GetMeshComponent(entityId))
	{
		entity.HasMesh = true;
		entity.MeshEnabled = m_Scene.IsMeshEnabled(entityId);
		if (mesh->Asset)
		{
			entity.PrimitiveKind = mesh->Asset->PrimitiveKind;
			entity.MeshAssetPath = mesh->Asset->SourcePath;
			entity.MaterialOverrides.assign(mesh->Asset->Materials.begin(), mesh->Asset->Materials.end());
		}
	}
	if (const CameraComponent* camera = m_Scene.GetCameraComponent(entityId))
	{
		entity.HasCamera = true;
		entity.CameraEnabled = m_Scene.IsCameraEnabled(entityId);
		entity.Camera = *camera;
	}
	if (const LightComponent* light = m_Scene.GetLightComponent(entityId))
	{
		entity.HasLight = true;
		entity.LightEnabled = m_Scene.IsLightEnabled(entityId);
		entity.Light = *light;
	}
	if (const AnimatorComponent* animator = m_Scene.GetAnimatorComponent(entityId))
	{
		entity.HasAnimator = true;
		entity.AnimatorEnabled = m_Scene.IsAnimatorEnabled(entityId);
		entity.Animator = *animator;
	}
	if (const RigidBodyComponent* rigidBody = m_Scene.GetRigidBodyComponent(entityId))
	{
		entity.HasRigidBody = true;
		entity.RigidBodyEnabled = m_Scene.IsRigidBodyEnabled(entityId);
		entity.RigidBody = *rigidBody;
	}
	if (const ColliderComponent* collider = m_Scene.GetColliderComponent(entityId))
	{
		entity.HasCollider = true;
		entity.ColliderEnabled = m_Scene.IsColliderEnabled(entityId);
		entity.Collider = *collider;
	}
	if (const PhysicsMaterialComponent* physicsMaterial = m_Scene.GetPhysicsMaterialComponent(entityId))
	{
		entity.HasPhysicsMaterial = true;
		entity.PhysicsMaterialEnabled = m_Scene.IsPhysicsMaterialEnabled(entityId);
		entity.PhysicsMaterial = *physicsMaterial;
	}
	if (const PrefabInstanceComponent* prefab = m_Scene.GetPrefabInstanceComponent(entityId))
	{
		entity.HasPrefabInstance = true;
		entity.PrefabInstanceEnabled = m_Scene.IsComponentEnabled<PrefabInstanceComponent>(entityId);
		entity.PrefabInstance = *prefab;
	}
	if (const SceneReferenceComponent* sceneReference = m_Scene.GetSceneReferenceComponent(entityId))
	{
		entity.HasSceneReference = true;
		entity.SceneReferenceEnabled = m_Scene.IsComponentEnabled<SceneReferenceComponent>(entityId);
		entity.SceneReference = *sceneReference;
	}
	if (const ScriptComponent* script = m_Scene.GetScriptComponent(entityId))
	{
		entity.HasScript = true;
		entity.ScriptEnabled = m_Scene.IsComponentEnabled<ScriptComponent>(entityId);
		entity.Script = *script;
	}
	if (const Sprite2DComponent* sprite = m_Scene.GetSprite2DComponent(entityId))
	{
		entity.HasSprite2D = true;
		entity.Sprite2DEnabled = m_Scene.IsComponentEnabled<Sprite2DComponent>(entityId);
		entity.Sprite2D = *sprite;
	}
	if (const UiElementComponent* ui = m_Scene.GetUiElementComponent(entityId))
	{
		entity.HasUiElement = true;
		entity.UiElementEnabled = m_Scene.IsComponentEnabled<UiElementComponent>(entityId);
		entity.UiElement = *ui;
	}
	if (const AudioSourceComponent* audio = m_Scene.GetAudioSourceComponent(entityId))
	{
		entity.HasAudioSource = true;
		entity.AudioSourceEnabled = m_Scene.IsComponentEnabled<AudioSourceComponent>(entityId);
		entity.AudioSource = *audio;
	}
	if (const NavigationAgentComponent* navigation = m_Scene.GetNavigationAgentComponent(entityId))
	{
		entity.HasNavigationAgent = true;
		entity.NavigationAgentEnabled = m_Scene.IsComponentEnabled<NavigationAgentComponent>(entityId);
		entity.NavigationAgent = *navigation;
	}
	if (const NetworkIdentityComponent* network = m_Scene.GetNetworkIdentityComponent(entityId))
	{
		entity.HasNetworkIdentity = true;
		entity.NetworkIdentityEnabled = m_Scene.IsComponentEnabled<NetworkIdentityComponent>(entityId);
		entity.NetworkIdentity = *network;
	}

	return entity;
}

std::string Engine::BuildMeshRestoreSignature(EntityId entityId) const
{
	const std::optional<ScenePersistence::LoadedSceneEntity> entity = BuildLoadedSceneEntityFromEntity(entityId);
	return entity ? BuildMeshRestoreSignature(*entity) : "entity:missing";
}

std::string Engine::BuildMeshRestoreSignature(const ScenePersistence::LoadedSceneEntity& entity) const
{
	if (!entity.HasMesh)
	{
		return "mesh:none";
	}

	std::string signature = std::format(
		"mesh:enabled={};primitive={};path={};materials={}",
		entity.MeshEnabled,
		static_cast<uint32_t>(entity.PrimitiveKind),
		entity.MeshAssetPath.lexically_normal().string(),
		entity.MaterialOverrides.size());

	for (size_t materialIndex = 0; materialIndex < entity.MaterialOverrides.size(); ++materialIndex)
	{
		const Asset::StaticMeshMaterial& material = entity.MaterialOverrides[materialIndex];
		signature.append(std::format(
			"|m{}:{}:{}:{:.4f},{:.4f},{:.4f},{:.4f}:vc={}:ny={}",
			materialIndex,
			material.Name,
			static_cast<uint32_t>(material.ShadingModel),
			material.DiffuseColor.x,
			material.DiffuseColor.y,
			material.DiffuseColor.z,
			material.DiffuseColor.w,
			material.UseVertexColor,
			material.NormalYFlip));
		for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
		{
			const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
			const std::filesystem::path texturePath = Asset::GetMaterialTexturePath(material, slot).lexically_normal();
			const Asset::MaterialTextureBinding& binding = material.TextureBindings[slotIndex];
			signature.append(std::format(
				";{}={}:override={}:embedded={}",
				Asset::MaterialTextureSlotKey(slot),
				texturePath.string(),
				binding.IsOverride,
				binding.Embedded.IsValid()));
		}
	}

	return signature;
}

std::vector<std::string> Engine::BuildMeshRestoreMaterialDiff(
	EntityId entityId,
	const Asset::StaticMeshAsset& importedMesh,
	size_t maxLines) const
{
	std::vector<std::string> lines;
	const Asset::StaticMeshAsset* currentMesh = m_Scene.GetMeshAsset(entityId);
	if (!currentMesh)
	{
		lines.push_back("Current Entity has no Mesh asset; stored restore would assign all imported materials.");
		return lines;
	}

	const auto pathLabel = [](const Asset::StaticMeshMaterial& material, Asset::MaterialTextureSlot slot)
	{
		const std::filesystem::path path = Asset::GetMaterialTexturePath(material, slot).lexically_normal();
		if (!path.empty())
		{
			return path.string();
		}

		const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
		return binding.Embedded.IsValid() ? std::string("<embedded>") : std::string("<none>");
	};

	const auto pushLine = [&lines, maxLines](std::string line)
	{
		if (lines.size() < maxLines)
		{
			lines.push_back(std::move(line));
		}
	};

	const size_t currentMaterialCount = currentMesh->Materials.size();
	const size_t importedMaterialCount = importedMesh.Materials.size();
	if (currentMaterialCount != importedMaterialCount)
	{
		pushLine(std::format("Material count: current {} -> restore {}", currentMaterialCount, importedMaterialCount));
	}

	size_t skippedChangeCount = 0;
	const size_t compareCount = (std::max)(currentMaterialCount, importedMaterialCount);
	for (size_t materialIndex = 0; materialIndex < compareCount; ++materialIndex)
	{
		if (lines.size() >= maxLines)
		{
			++skippedChangeCount;
			continue;
		}

		if (materialIndex >= currentMaterialCount)
		{
			const Asset::StaticMeshMaterial& importedMaterial = importedMesh.Materials[materialIndex];
			pushLine(std::format(
				"Material[{}] added by restore: '{}'",
				materialIndex,
				importedMaterial.Name.empty() ? "<unnamed>" : importedMaterial.Name));
			continue;
		}

		if (materialIndex >= importedMaterialCount)
		{
			const Asset::StaticMeshMaterial& currentMaterial = currentMesh->Materials[materialIndex];
			pushLine(std::format(
				"Material[{}] removed by restore: '{}'",
				materialIndex,
				currentMaterial.Name.empty() ? "<unnamed>" : currentMaterial.Name));
			continue;
		}

		const Asset::StaticMeshMaterial& currentMaterial = currentMesh->Materials[materialIndex];
		const Asset::StaticMeshMaterial& importedMaterial = importedMesh.Materials[materialIndex];
		if (currentMaterial.Name != importedMaterial.Name)
		{
			pushLine(std::format(
				"Material[{}] name: '{}' -> '{}'",
				materialIndex,
				currentMaterial.Name.empty() ? "<unnamed>" : currentMaterial.Name,
				importedMaterial.Name.empty() ? "<unnamed>" : importedMaterial.Name));
		}
		if (currentMaterial.ShadingModel != importedMaterial.ShadingModel)
		{
			pushLine(std::format(
				"Material[{}] shading: {} -> {}",
				materialIndex,
				Asset::MaterialShadingModelName(currentMaterial.ShadingModel),
				Asset::MaterialShadingModelName(importedMaterial.ShadingModel)));
		}

		for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
		{
			const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
			const std::string currentPath = pathLabel(currentMaterial, slot);
			const std::string importedPath = pathLabel(importedMaterial, slot);
			const Asset::MaterialTextureBinding& currentBinding = Asset::GetMaterialTextureBinding(currentMaterial, slot);
			const Asset::MaterialTextureBinding& importedBinding = Asset::GetMaterialTextureBinding(importedMaterial, slot);
			if (currentPath == importedPath
				&& currentBinding.IsOverride == importedBinding.IsOverride
				&& currentBinding.Embedded.IsValid() == importedBinding.Embedded.IsValid())
			{
				continue;
			}

			if (lines.size() >= maxLines)
			{
				++skippedChangeCount;
				continue;
			}

			pushLine(std::format(
				"Material[{}] {}: {}{} -> {}{}",
				materialIndex,
				Asset::MaterialTextureSlotName(slot),
				currentPath,
				currentBinding.IsOverride ? " (override)" : "",
				importedPath,
				importedBinding.IsOverride ? " (override)" : ""));
		}
	}

	if (skippedChangeCount > 0)
	{
		lines.push_back(std::format("... {} more material differences not shown.", skippedChangeCount));
	}
	if (lines.empty())
	{
		lines.push_back("No material slot differences detected; conflict came from Mesh identity, counts, or source prefab timestamp.");
	}
	return lines;
}

std::vector<Editor::MeshRestoreMaterialDiffRow> Engine::BuildMeshRestoreMaterialDiffRows(
	EntityId entityId,
	const Asset::StaticMeshAsset& importedMesh,
	size_t maxRows) const
{
	std::vector<Editor::MeshRestoreMaterialDiffRow> rows;
	const Asset::StaticMeshAsset* currentMesh = m_Scene.GetMeshAsset(entityId);
	if (!currentMesh)
	{
		rows.push_back({
			.MaterialIndex = static_cast<size_t>(-1),
			.TextureSlot = Asset::MaterialTextureSlot::Count,
			.Field = "Mesh",
			.CurrentValue = "<none>",
			.RestoreValue = "Imported Mesh materials"
		});
		return rows;
	}

	const auto pathLabel = [](const Asset::StaticMeshMaterial& material, Asset::MaterialTextureSlot slot)
	{
		const std::filesystem::path path = Asset::GetMaterialTexturePath(material, slot).lexically_normal();
		if (!path.empty())
		{
			return path.string();
		}

		const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
		return binding.Embedded.IsValid() ? std::string("<embedded>") : std::string("<none>");
	};
	const auto textureLabel = [&pathLabel](const Asset::StaticMeshMaterial& material, Asset::MaterialTextureSlot slot)
	{
		const Asset::MaterialTextureBinding& binding = Asset::GetMaterialTextureBinding(material, slot);
		std::string label = pathLabel(material, slot);
		if (binding.IsOverride)
		{
			label.append(" (override)");
		}
		if (binding.Embedded.IsValid())
		{
			label.append(std::format(" {}x{}", binding.Embedded.Width, binding.Embedded.Height));
		}
		return label;
	};
	const auto color3Label = [](const DirectX::XMFLOAT3& value)
	{
		return std::format("{:.3f}, {:.3f}, {:.3f}", value.x, value.y, value.z);
	};
	const auto color4Label = [](const DirectX::XMFLOAT4& value)
	{
		return std::format("{:.3f}, {:.3f}, {:.3f}, {:.3f}", value.x, value.y, value.z, value.w);
	};
	const auto floatLabel = [](float value)
	{
		return std::format("{:.4f}", value);
	};
	const auto boolLabel = [](bool value)
	{
		return value ? std::string("true") : std::string("false");
	};
	const auto sameFloat = [](float lhs, float rhs)
	{
		return std::abs(lhs - rhs) <= 0.0001f;
	};
	const auto sameFloat3 = [&sameFloat](const DirectX::XMFLOAT3& lhs, const DirectX::XMFLOAT3& rhs)
	{
		return sameFloat(lhs.x, rhs.x) && sameFloat(lhs.y, rhs.y) && sameFloat(lhs.z, rhs.z);
	};
	const auto sameFloat4 = [&sameFloat](const DirectX::XMFLOAT4& lhs, const DirectX::XMFLOAT4& rhs)
	{
		return sameFloat(lhs.x, rhs.x) && sameFloat(lhs.y, rhs.y) && sameFloat(lhs.z, rhs.z) && sameFloat(lhs.w, rhs.w);
	};

	size_t skippedChangeCount = 0;
	const auto pushRow = [&rows, maxRows, &skippedChangeCount](
		std::string field,
		std::string currentValue,
		std::string restoreValue,
		size_t materialIndex = static_cast<size_t>(-1),
		Asset::MaterialTextureSlot textureSlot = Asset::MaterialTextureSlot::Count,
		Editor::MeshRestoreMaterialFocusKind focusKind = Editor::MeshRestoreMaterialFocusKind::None)
	{
		if (rows.size() < maxRows)
		{
			rows.push_back({
				.MaterialIndex = materialIndex,
				.TextureSlot = textureSlot,
				.FocusKind = focusKind,
				.Field = std::move(field),
				.CurrentValue = std::move(currentValue),
				.RestoreValue = std::move(restoreValue)
			});
		}
		else
		{
			++skippedChangeCount;
		}
	};

	const size_t currentMaterialCount = currentMesh->Materials.size();
	const size_t importedMaterialCount = importedMesh.Materials.size();
	if (currentMaterialCount != importedMaterialCount)
	{
		pushRow("Material Count", std::to_string(currentMaterialCount), std::to_string(importedMaterialCount));
	}

	const size_t compareCount = (std::max)(currentMaterialCount, importedMaterialCount);
	for (size_t materialIndex = 0; materialIndex < compareCount; ++materialIndex)
	{
		if (materialIndex >= currentMaterialCount)
		{
			const Asset::StaticMeshMaterial& importedMaterial = importedMesh.Materials[materialIndex];
			pushRow(
				std::format("Material[{}]", materialIndex),
				"<missing>",
				importedMaterial.Name.empty() ? "<unnamed>" : importedMaterial.Name,
				materialIndex);
			continue;
		}
		if (materialIndex >= importedMaterialCount)
		{
			const Asset::StaticMeshMaterial& currentMaterial = currentMesh->Materials[materialIndex];
			pushRow(
				std::format("Material[{}]", materialIndex),
				currentMaterial.Name.empty() ? "<unnamed>" : currentMaterial.Name,
				"<removed>",
				materialIndex);
			continue;
		}

		const Asset::StaticMeshMaterial& currentMaterial = currentMesh->Materials[materialIndex];
		const Asset::StaticMeshMaterial& importedMaterial = importedMesh.Materials[materialIndex];
		const std::string materialPrefix = std::format("Material[{}]", materialIndex);
		if (currentMaterial.Name != importedMaterial.Name)
		{
			pushRow(
				materialPrefix + " Name",
				currentMaterial.Name.empty() ? "<unnamed>" : currentMaterial.Name,
				importedMaterial.Name.empty() ? "<unnamed>" : importedMaterial.Name,
				materialIndex);
		}
		if (currentMaterial.ShadingModel != importedMaterial.ShadingModel)
		{
			pushRow(
				materialPrefix + " Shading",
				std::string(Asset::MaterialShadingModelName(currentMaterial.ShadingModel)),
				std::string(Asset::MaterialShadingModelName(importedMaterial.ShadingModel)),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::ShadingModel);
		}
		if (!sameFloat4(currentMaterial.DiffuseColor, importedMaterial.DiffuseColor))
		{
			pushRow(
				materialPrefix + " Base Color Tint",
				color4Label(currentMaterial.DiffuseColor),
				color4Label(importedMaterial.DiffuseColor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::BaseColor);
		}
		if (!sameFloat4(currentMaterial.ImportedDiffuseTint, importedMaterial.ImportedDiffuseTint))
		{
			pushRow(materialPrefix + " Imported Diffuse Tint", color4Label(currentMaterial.ImportedDiffuseTint), color4Label(importedMaterial.ImportedDiffuseTint), materialIndex);
		}
		if (!sameFloat3(currentMaterial.SpecularColor, importedMaterial.SpecularColor))
		{
			pushRow(
				materialPrefix + " Specular Color",
				color3Label(currentMaterial.SpecularColor),
				color3Label(importedMaterial.SpecularColor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::SpecularColor);
		}
		if (!sameFloat3(currentMaterial.EmissiveColor, importedMaterial.EmissiveColor))
		{
			pushRow(
				materialPrefix + " Emissive Color",
				color3Label(currentMaterial.EmissiveColor),
				color3Label(importedMaterial.EmissiveColor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::EmissiveColor);
		}
		if (!sameFloat(currentMaterial.MetallicFactor, importedMaterial.MetallicFactor))
		{
			pushRow(
				materialPrefix + " Metallic",
				floatLabel(currentMaterial.MetallicFactor),
				floatLabel(importedMaterial.MetallicFactor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::Metallic);
		}
		if (!sameFloat(currentMaterial.RoughnessFactor, importedMaterial.RoughnessFactor))
		{
			pushRow(
				materialPrefix + " Roughness",
				floatLabel(currentMaterial.RoughnessFactor),
				floatLabel(importedMaterial.RoughnessFactor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::Roughness);
		}
		if (!sameFloat(currentMaterial.Shininess, importedMaterial.Shininess))
		{
			pushRow(
				materialPrefix + " Shininess",
				floatLabel(currentMaterial.Shininess),
				floatLabel(importedMaterial.Shininess),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::Shininess);
		}
		if (!sameFloat(currentMaterial.Opacity, importedMaterial.Opacity))
		{
			pushRow(
				materialPrefix + " Opacity",
				floatLabel(currentMaterial.Opacity),
				floatLabel(importedMaterial.Opacity),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::Opacity);
		}
		if (currentMaterial.UseVertexColor != importedMaterial.UseVertexColor)
		{
			pushRow(
				materialPrefix + " Use Vertex Color",
				boolLabel(currentMaterial.UseVertexColor),
				boolLabel(importedMaterial.UseVertexColor),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::UseVertexColor);
		}
		if (currentMaterial.NormalYFlip != importedMaterial.NormalYFlip)
		{
			pushRow(
				materialPrefix + " Normal Y Flip",
				boolLabel(currentMaterial.NormalYFlip),
				boolLabel(importedMaterial.NormalYFlip),
				materialIndex,
				Asset::MaterialTextureSlot::Count,
				Editor::MeshRestoreMaterialFocusKind::NormalYFlip);
		}

		for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
		{
			const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
			const std::string currentTexture = textureLabel(currentMaterial, slot);
			const std::string importedTexture = textureLabel(importedMaterial, slot);
			const Asset::MaterialTextureBinding& currentBinding = Asset::GetMaterialTextureBinding(currentMaterial, slot);
			const Asset::MaterialTextureBinding& importedBinding = Asset::GetMaterialTextureBinding(importedMaterial, slot);
			if (currentTexture == importedTexture
				&& currentBinding.IsOverride == importedBinding.IsOverride
				&& currentBinding.Embedded.IsValid() == importedBinding.Embedded.IsValid())
			{
				continue;
			}
			pushRow(
				materialPrefix + " " + std::string(Asset::MaterialTextureSlotName(slot)),
				currentTexture,
				importedTexture,
				materialIndex,
				slot,
				Editor::MeshRestoreMaterialFocusKind::TextureSlot);
		}
	}

	if (skippedChangeCount > 0)
	{
		rows.push_back({
			.MaterialIndex = static_cast<size_t>(-1),
			.TextureSlot = Asset::MaterialTextureSlot::Count,
			.Field = "More Differences",
			.CurrentValue = std::format("{} hidden", skippedChangeCount),
			.RestoreValue = std::format("{} hidden", skippedChangeCount)
		});
	}
	if (rows.empty())
	{
		rows.push_back({
			.MaterialIndex = static_cast<size_t>(-1),
			.TextureSlot = Asset::MaterialTextureSlot::Count,
			.Field = "Material Diff",
			.CurrentValue = "No material factor or slot differences detected",
			.RestoreValue = "Conflict came from Mesh identity, counts, or source prefab timestamp"
		});
	}
	return rows;
}

bool Engine::SaveSelectedEntityAsPrefab()
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Prefab save is only available in Project Scene.");
		return false;
	}

	const EntityId selectedEntity = m_Scene.GetSelectedEntity();
	if (selectedEntity == InvalidEntityId || !m_Scene.ContainsEntity(selectedEntity))
	{
		AppendAssetLog("Prefab save failed: no selected entity.");
		return false;
	}

	const std::string* entityName = m_Scene.GetEntityName(selectedEntity);
	const std::string prefabName = SanitizeFileStem(entityName && !entityName->empty() ? *entityName : "Entity");
	const std::filesystem::path prefabPath = (m_Project->RootPath / m_Project->AssetRoot / "Prefabs" / (prefabName + ".prefab")).lexically_normal();

	ScenePersistence::PrefabSaveOptions options;
	options.AmbientColor = m_AmbientColor;
	options.AmbientIntensity = m_AmbientIntensity;
	options.Exposure = m_Exposure;
	options.Skybox = m_SkyboxSettings;

	std::string errorMessage;
	if (!ScenePersistence::PrefabService::SaveEntityAsPrefab(m_Scene, selectedEntity, *m_Project, prefabPath, options, errorMessage))
	{
		AppendAssetLog(std::format("Prefab save failed: {}", errorMessage));
		return false;
	}

	PrefabInstanceComponent& prefab = m_Scene.EnsurePrefabInstanceComponent(selectedEntity);
	prefab.PrefabPath = prefabPath;
	prefab.SourceName = entityName && !entityName->empty() ? *entityName : "Entity";
	prefab.TrackPrefabOverrides = true;
	static_cast<void>(m_Scene.SetComponentEnabled<PrefabInstanceComponent>(selectedEntity, true));
	static_cast<void>(DeclareResourceForPath(prefabPath, Resources::ResourceKind::Scene));
	m_AssetFileSystem.RequestRefresh();
	MarkSceneDirty();
	AppendAssetLog(std::format("Saved prefab: {}", prefabPath.string()));
	return true;
}

bool Engine::ApplyEntityToPrefabSource(EntityId entityId)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Apply to Prefab is only available in Project Scene.");
		return false;
	}
	if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Apply to Prefab failed: invalid entity.");
		return false;
	}

	const PrefabInstanceComponent* prefab = m_Scene.GetPrefabInstanceComponent(entityId);
	if (!prefab || prefab->PrefabPath.empty())
	{
		AppendAssetLog("Apply to Prefab failed: entity has no prefab source path.");
		return false;
	}

	ScenePersistence::PrefabSaveOptions options;
	options.AmbientColor = m_AmbientColor;
	options.AmbientIntensity = m_AmbientIntensity;
	options.Exposure = m_Exposure;
	options.Skybox = m_SkyboxSettings;
	options.IncludePrefabInstanceComponent = false;

	std::string errorMessage;
	const std::filesystem::path prefabPath = prefab->PrefabPath.lexically_normal();
	if (!ScenePersistence::PrefabService::SaveEntityAsPrefab(m_Scene, entityId, *m_Project, prefabPath, options, errorMessage))
	{
		AppendAssetLog(std::format("Apply to Prefab failed: {}", errorMessage));
		return false;
	}

	static_cast<void>(DeclareResourceForPath(prefabPath, Resources::ResourceKind::Scene));
	m_AssetFileSystem.RequestRefresh();
	AppendAssetLog(std::format("Applied entity {} to prefab: {}", entityId, prefabPath.string()));
	return true;
}

bool Engine::SavePrefabInspectionRoot(const std::filesystem::path& prefabPath, const ScenePersistence::LoadedSceneEntity& root)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Prefab property apply is only available in Project Scene.");
		return false;
	}
	if (prefabPath.empty())
	{
		AppendAssetLog("Prefab property apply failed: empty prefab path.");
		return false;
	}

	ScenePersistence::PrefabSaveOptions options;
	options.AmbientColor = m_AmbientColor;
	options.AmbientIntensity = m_AmbientIntensity;
	options.Exposure = m_Exposure;
	options.Skybox = m_SkyboxSettings;
	options.IncludePrefabInstanceComponent = false;

	std::string errorMessage;
	const std::filesystem::path normalizedPrefabPath = prefabPath.lexically_normal();
	if (!ScenePersistence::PrefabService::SaveLoadedEntityAsPrefab(root, *m_Project, normalizedPrefabPath, options, errorMessage))
	{
		AppendAssetLog(std::format("Prefab property apply failed: {}", errorMessage));
		return false;
	}

	static_cast<void>(DeclareResourceForPath(normalizedPrefabPath, Resources::ResourceKind::Scene));
	m_AssetFileSystem.RequestRefresh();
	AppendAssetLog(std::format("Applied reflected prefab property to {}", normalizedPrefabPath.string()));
	return true;
}

bool Engine::RevertEntityMeshToPrefabSource(
	EntityId entityId,
	const ScenePersistence::LoadedSceneEntity& prefabRoot,
	const std::filesystem::path& prefabPath)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Prefab Mesh revert is only available in Project Scene.");
		return false;
	}
	if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Prefab Mesh revert failed: invalid entity.");
		return false;
	}
	if (!prefabRoot.HasMesh)
	{
		AppendAssetLog("Prefab Mesh revert failed: prefab source has no Mesh component.");
		return false;
	}

	const std::optional<ScenePersistence::LoadedSceneEntity> beforeSnapshot = BuildLoadedSceneEntityFromEntity(entityId);
	if (!beforeSnapshot || !beforeSnapshot->HasMesh)
	{
		AppendAssetLog("Prefab Mesh revert failed: current Entity has no restorable Mesh snapshot.");
		return false;
	}

	ScenePersistence::LoadedSceneEntity afterSnapshot = prefabRoot;
	afterSnapshot.Name = beforeSnapshot->Name;
	afterSnapshot.HasTransform = beforeSnapshot->HasTransform;
	afterSnapshot.Transform = beforeSnapshot->Transform;
	afterSnapshot.HasAnimator = beforeSnapshot->HasAnimator;
	afterSnapshot.AnimatorEnabled = beforeSnapshot->AnimatorEnabled;
	afterSnapshot.Animator = beforeSnapshot->Animator;

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Revert Mesh from Prefab",
		.Execute = [this, entityId, afterSnapshot, prefabPath]()
		{
			static_cast<void>(ApplyMeshSnapshotToEntity(entityId, afterSnapshot, "Reverted Mesh from prefab", prefabPath));
		},
		.Undo = [this, entityId, before = *beforeSnapshot]()
		{
			static_cast<void>(ApplyMeshSnapshotToEntity(entityId, before, "Undo Mesh prefab revert"));
		}
	});
	return true;
}

Editor::MeshRestoreRuntimeStatus Engine::GetMeshRestoreRuntimeStatus(EntityId entityId) const
{
	const auto restoreIt = m_MeshRestoreRequests.find(entityId);
	if (restoreIt == m_MeshRestoreRequests.end())
	{
		return {};
	}

	const MeshRestoreRequestState& state = restoreIt->second;
	return Editor::MeshRestoreRuntimeStatus{
		.HasStatus = true,
		.Pending = state.Pending,
		.Failed = state.Failed,
		.Cancelled = state.Cancelled,
		.Conflicted = state.Conflicted,
		.Generation = state.Generation,
		.SourcePath = state.SourcePath,
		.SourcePrefabPath = state.SourcePrefabPath,
		.ImportedVertexCount = state.ImportedVertexCount,
		.ImportedIndexCount = state.ImportedIndexCount,
		.ImportedMaterialCount = state.ImportedMaterialCount,
		.MaterialDiffLines = state.MaterialDiffLines,
		.MaterialDiffRows = state.MaterialDiffRows,
		.Message = state.Message
	};
}

bool Engine::CancelMeshRestore(EntityId entityId)
{
	const auto restoreIt = m_MeshRestoreRequests.find(entityId);
	if (restoreIt == m_MeshRestoreRequests.end())
	{
		m_MeshRestoreConflictResults.erase(entityId);
		return false;
	}

	MeshRestoreRequestState& state = restoreIt->second;
	if (!state.Pending)
	{
		m_MeshRestoreRequests.erase(restoreIt);
		m_MeshRestoreConflictResults.erase(entityId);
		return true;
	}

	const uint64_t nextGeneration = ++m_MeshRestoreGenerations[entityId];
	state.Generation = nextGeneration;
	state.Pending = false;
	state.Failed = false;
	state.Cancelled = true;
	state.Conflicted = false;
	state.ImportedVertexCount = 0;
	state.ImportedIndexCount = 0;
	state.ImportedMaterialCount = 0;
	state.MaterialDiffLines.clear();
	state.MaterialDiffRows.clear();
	state.Message = "Cancelled by editor. Any in-flight restore result will be discarded.";
	m_MeshRestoreConflictResults.erase(entityId);
	AppendAssetLog(std::format("Cancelled pending Mesh restore for entity {}.", entityId));
	return true;
}

bool Engine::ApplyConflictedMeshRestore(EntityId entityId)
{
	const auto resultIt = m_MeshRestoreConflictResults.find(entityId);
	if (resultIt == m_MeshRestoreConflictResults.end())
	{
		AppendAssetLog(std::format("Apply conflicted Mesh restore failed: no stored result for entity {}.", entityId));
		return false;
	}
	if (!m_Scene.ContainsEntity(entityId))
	{
		m_MeshRestoreConflictResults.erase(resultIt);
		m_MeshRestoreRequests.erase(entityId);
		AppendAssetLog(std::format("Apply conflicted Mesh restore failed: entity {} no longer exists.", entityId));
		return false;
	}

	Asset::AssetImportResult result = std::move(resultIt->second);
	m_MeshRestoreConflictResults.erase(resultIt);
	AppendAssetLog(std::format("Applying conflicted Mesh restore anyway for entity {}.", entityId));
	ApplyImportedModel(std::move(result), true);
	return true;
}

bool Engine::ReloadMeshRestoreFromPrefabSource(EntityId entityId)
{
	const auto restoreIt = m_MeshRestoreRequests.find(entityId);
	if (restoreIt == m_MeshRestoreRequests.end() || restoreIt->second.SourcePrefabPath.empty())
	{
		AppendAssetLog(std::format("Reload Mesh restore source failed: entity {} has no source prefab path.", entityId));
		return false;
	}
	if (!m_Project || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog(std::format("Reload Mesh restore source failed: invalid project or entity {}.", entityId));
		return false;
	}

	const std::filesystem::path prefabPath = restoreIt->second.SourcePrefabPath;
	ScenePersistence::LoadPrefabResult prefabResult = ScenePersistence::PrefabService::LoadPrefab(prefabPath, *m_Project);
	if (!prefabResult.Success)
	{
		restoreIt->second.Pending = false;
		restoreIt->second.Failed = true;
		restoreIt->second.Cancelled = false;
		restoreIt->second.Conflicted = false;
		restoreIt->second.Message = std::format("Reload Prefab Source failed: {}", prefabResult.ErrorMessage);
		AppendAssetLog(restoreIt->second.Message);
		return false;
	}

	m_MeshRestoreConflictResults.erase(entityId);
	AppendAssetLog(std::format("Reloading Mesh restore from updated prefab source: {}", prefabPath.string()));
	return RevertEntityMeshToPrefabSource(entityId, prefabResult.Root, prefabPath);
}

bool Engine::ApplyMeshSnapshotToEntity(
	EntityId entityId,
	const ScenePersistence::LoadedSceneEntity& meshSnapshot,
	std::string_view logLabel,
	const std::filesystem::path& sourcePrefabPath)
{
	if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Mesh snapshot apply failed: invalid entity.");
		return false;
	}
	if (!meshSnapshot.HasMesh)
	{
		AppendAssetLog("Mesh snapshot apply failed: snapshot has no Mesh component.");
		return false;
	}

	ScenePersistence::LoadedSceneEntity restore = meshSnapshot;
	if (const auto currentSnapshot = BuildLoadedSceneEntityFromEntity(entityId))
	{
		restore.Name = currentSnapshot->Name;
		restore.HasTransform = true;
		restore.Transform = currentSnapshot->Transform;
		restore.HasAnimator = currentSnapshot->HasAnimator;
		restore.AnimatorEnabled = currentSnapshot->AnimatorEnabled;
		restore.Animator = currentSnapshot->Animator;
	}

	const auto cleanupRuntimeAssetLinks = [this, entityId]()
	{
		DestroyTextureResourcesForEntity(entityId);
		for (const auto& removedSourcePath : m_RuntimeAssetRegistry.UnregisterEntity(entityId))
		{
			m_AssetHotReloadService.UnwatchLoadedAsset(removedSourcePath);
		}
	};

	const std::string actionLabel = logLabel.empty() ? std::string("Applied Mesh snapshot") : std::string(logLabel);
	if (restore.PrimitiveKind != Asset::PrimitiveMeshKind::None)
	{
		++m_MeshRestoreGenerations[entityId];
		m_MeshRestoreRequests.erase(entityId);
		m_MeshRestoreConflictResults.erase(entityId);
		cleanupRuntimeAssetLinks();

		if (!ApplyPrimitiveMeshToEntity(entityId, restore.PrimitiveKind, restore.Transform, false))
		{
			AppendAssetLog("Mesh snapshot apply failed: primitive mesh creation failed.");
			return false;
		}
		if (!restore.MaterialOverrides.empty())
		{
			if (Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId))
			{
				ApplyMaterialOverrides(*meshAsset, restore.MaterialOverrides);
				static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
			}
		}
		static_cast<void>(m_Scene.SetMeshEnabled(entityId, restore.MeshEnabled));
		MarkPhysicsActorDirty(entityId);
		MarkSceneDirty();
		AppendAssetLog(std::format("{} for entity {} using primitive Mesh.", actionLabel, entityId));
		return true;
	}

	if (!restore.MeshAssetPath.empty())
	{
		QueueModelImportForSceneEntity(restore, entityId, sourcePrefabPath);
		MarkSceneDirty();
		AppendAssetLog(std::format(
			"{} queued for entity {}: {}",
			actionLabel,
			entityId,
			restore.MeshAssetPath.string()));
		return true;
	}

	++m_MeshRestoreGenerations[entityId];
	m_MeshRestoreRequests.erase(entityId);
	m_MeshRestoreConflictResults.erase(entityId);
	cleanupRuntimeAssetLinks();
	MeshComponent& mesh = m_Scene.EnsureMeshComponent(entityId);
	mesh.Asset.reset();
	mesh.MaterialTextures.clear();
	static_cast<void>(m_Scene.SetMeshEnabled(entityId, restore.MeshEnabled));
	m_RenderState.EntityMaterialTransparency.erase(entityId);
	MarkPhysicsActorDirty(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("{} for entity {} using empty Mesh component.", actionLabel, entityId));
	return true;
}

bool Engine::LoadSceneReference(EntityId entityId, bool markDirty)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Scene reference load is only available in Project Scene.");
		return false;
	}
	if (!m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Scene reference load failed: entity no longer exists.");
		return false;
	}

	const SceneReferenceComponent* sceneReference = m_Scene.GetSceneReferenceComponent(entityId);
	if (!sceneReference)
	{
		AppendAssetLog(std::format("Scene reference load failed: entity {} has no SceneReference component.", entityId));
		return false;
	}
	if (!m_Scene.IsComponentEnabled<SceneReferenceComponent>(entityId))
	{
		AppendAssetLog(std::format("Scene reference load skipped: SceneReference component is disabled on entity {}.", entityId));
		return false;
	}
	if (sceneReference->ScenePath.empty())
	{
		AppendAssetLog(std::format("Scene reference load failed: entity {} has no scene path.", entityId));
		return false;
	}
	if (!sceneReference->LoadAdditively)
	{
		AppendAssetLog("Scene reference load skipped: Load Additively is disabled. Use Open Scene to replace the active scene.");
		return false;
	}

	const std::filesystem::path scenePath = ResolveProjectScenePath(sceneReference->ScenePath);
	std::error_code errorCode;
	if (!std::filesystem::is_regular_file(scenePath, errorCode))
	{
		AppendAssetLog(std::format("Scene reference load failed: file not found {}", scenePath.string()));
		return false;
	}
	if (!m_CurrentScenePath.empty() && std::filesystem::equivalent(scenePath, m_CurrentScenePath, errorCode))
	{
		AppendAssetLog("Scene reference load blocked: a scene cannot load itself as a child in v1.");
		return false;
	}

	if (m_LoadedSceneReferenceEntities.contains(entityId))
	{
		static_cast<void>(UnloadSceneReference(entityId, markDirty));
	}

	const ScenePersistence::LoadSceneResult loadResult = ScenePersistence::ScenePersistenceService::LoadScene(scenePath, *m_Project);
	if (!loadResult.Success)
	{
		AppendAssetLog(std::format("Scene reference load failed: {}", loadResult.ErrorMessage));
		return false;
	}
	if (loadResult.Entities.empty())
	{
		AppendAssetLog(std::format("Scene reference load skipped: referenced scene has no entities {}", scenePath.string()));
		return false;
	}

	std::vector<EntityId> createdEntityIds;
	createdEntityIds.reserve(loadResult.Entities.size());
	for (const ScenePersistence::LoadedSceneEntity& loadedEntity : loadResult.Entities)
	{
		createdEntityIds.push_back(CreateEntityFromLoadedSceneEntity(loadedEntity, {}));
	}

	for (size_t entityIndex = 0; entityIndex < loadResult.Entities.size(); ++entityIndex)
	{
		if (entityIndex >= createdEntityIds.size() || createdEntityIds[entityIndex] == InvalidEntityId)
		{
			continue;
		}

		const ScenePersistence::LoadedSceneEntity& loadedEntity = loadResult.Entities[entityIndex];
		EntityId parentEntity = InvalidEntityId;
		if (loadedEntity.HasHierarchy && loadedEntity.ParentIndex < createdEntityIds.size())
		{
			parentEntity = createdEntityIds[loadedEntity.ParentIndex];
		}
		if (parentEntity == InvalidEntityId)
		{
			parentEntity = entityId;
		}
		static_cast<void>(m_Scene.SetParentEntity(createdEntityIds[entityIndex], parentEntity, false));
		if (loadedEntity.HasHierarchy)
		{
			m_Scene.EnsureHierarchyComponent(createdEntityIds[entityIndex]).Expanded = loadedEntity.HierarchyExpanded;
		}
	}

	std::erase(createdEntityIds, InvalidEntityId);
	std::filesystem::file_time_type lastWriteTime = {};
	std::error_code writeTimeError;
	lastWriteTime = std::filesystem::last_write_time(scenePath, writeTimeError);
	m_LoadedSceneReferenceEntities[entityId] = LoadedSceneReferenceState{
		.ScenePath = scenePath,
		.LastWriteTime = writeTimeError ? std::filesystem::file_time_type{} : lastWriteTime,
		.LoadedEntities = createdEntityIds,
		.PendingExternalReload = false
	};
	m_Scene.UpdateWorldTransforms();
	RebuildPhysicsWorldFromScene();
	m_Scene.SetSelectedEntity(entityId);
	if (markDirty)
	{
		MarkSceneDirty();
	}
	AppendAssetLog(std::format(
		"Loaded scene reference: {} under entity {} ({} entity/entities)",
		scenePath.string(),
		entityId,
		createdEntityIds.size()));
	return true;
}

bool Engine::UnloadSceneReference(EntityId entityId, bool markDirty)
{
	const auto loadedIt = m_LoadedSceneReferenceEntities.find(entityId);
	if (loadedIt == m_LoadedSceneReferenceEntities.end())
	{
		AppendAssetLog(std::format("Scene reference unload skipped: no loaded children tracked for entity {}.", entityId));
		return false;
	}

	std::vector<EntityId> loadedEntityIds = std::move(loadedIt->second.LoadedEntities);
	m_LoadedSceneReferenceEntities.erase(loadedIt);
	size_t removedCount = 0;
	const bool previousDirtyState = m_SceneDirty;
	for (auto entityIt = loadedEntityIds.rbegin(); entityIt != loadedEntityIds.rend(); ++entityIt)
	{
		if (*entityIt != InvalidEntityId && m_Scene.ContainsEntity(*entityIt))
		{
			DeleteEntityFromHierarchy(*entityIt);
			++removedCount;
		}
	}
	if (m_Scene.ContainsEntity(entityId))
	{
		m_Scene.SetSelectedEntity(entityId);
	}
	RebuildPhysicsWorldFromScene();
	if (markDirty)
	{
		MarkSceneDirty();
	}
	else
	{
		SetSceneDirty(previousDirtyState);
	}
	AppendAssetLog(std::format("Unloaded scene reference children from entity {} ({} removed)", entityId, removedCount));
	return removedCount > 0;
}

bool Engine::InstantiatePrefabAsset(const std::filesystem::path& prefabPath)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Prefab instantiate is only available in Project Scene.");
		return false;
	}

	ScenePersistence::LoadPrefabResult result = ScenePersistence::PrefabService::LoadPrefab(prefabPath, *m_Project);
	if (!result.Success)
	{
		AppendAssetLog(std::format("Prefab instantiate failed: {}", result.ErrorMessage));
		return false;
	}

	EntityId entityId = CreateEntityFromLoadedSceneEntity(result.Root, prefabPath);
	if (TransformComponent* transform = m_Scene.GetTransformComponent(entityId))
	{
		const DirectX::XMFLOAT3 cameraPosition = m_SceneCamera.GetPosition();
		const DirectX::XMFLOAT3 cameraForward = m_SceneCamera.GetForward();
		const DirectX::XMVECTOR target = DirectX::XMVectorAdd(
			DirectX::XMLoadFloat3(&cameraPosition),
			DirectX::XMVectorScale(DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraForward)), 5.0f));
		DirectX::XMStoreFloat3(&transform->LocalTransform.Translation, target);
		transform->UpdateWorld();
	}

	m_Scene.SetSelectedEntity(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("Instantiated prefab entity {} from {}", entityId, prefabPath.string()));
	return true;
}

bool Engine::CreateOrUpdateImportSettingsForAsset(const std::filesystem::path& assetPath)
{
	if (assetPath.empty())
	{
		return false;
	}

	Asset::AssetImportSettingsResult loadResult = Asset::AssetImportSettingsService::LoadOrDefault(assetPath);
	Asset::AssetImportSettings settings = loadResult.Settings;
	if (!loadResult.Success)
	{
		settings.SourcePath = assetPath;
		AppendAssetLog(std::format("Import settings reset to defaults: {}", loadResult.ErrorMessage));
	}

	std::string errorMessage;
	if (!Asset::AssetImportSettingsService::Save(settings, errorMessage))
	{
		AppendAssetLog(std::format("Import settings save failed: {}", errorMessage));
		return false;
	}

	const std::filesystem::path settingsPath = Asset::AssetImportSettingsService::GetSettingsPathForAsset(assetPath);
	m_AssetFileSystem.RequestRefresh();
	AppendAssetLog(std::format("Import settings saved: {}", settingsPath.string()));
	if (Asset::IsModelAssetPath(assetPath) && !m_RuntimeAssetRegistry.GetEntities(assetPath).empty())
	{
		QueueModelReload(assetPath, settingsPath);
	}
	else if (Asset::IsModelAssetPath(assetPath))
	{
		AppendAssetLog("Import settings will be applied the next time this model is imported.");
	}
	return true;
}

bool Engine::CreateProjectAsset(Editor::ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory)
{
	return CreateProjectAsset(kind, targetDirectory, {});
}

bool Engine::CreateProjectAsset(Editor::ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory, std::string_view requestedName)
{
	if (!m_Project)
	{
		AppendAssetLog("Create asset failed: no active project.");
		return false;
	}

	const std::filesystem::path assetRoot = (m_Project->RootPath / m_Project->AssetRoot).lexically_normal();
	std::filesystem::path directory = targetDirectory.empty() ? assetRoot : targetDirectory.lexically_normal();
	std::error_code errorCode;
	if (!std::filesystem::is_directory(directory, errorCode))
	{
		directory = directory.parent_path();
	}
	if (directory.empty())
	{
		directory = assetRoot;
	}

	const std::filesystem::path relativeDirectory = std::filesystem::relative(directory, assetRoot, errorCode);
	if (errorCode || (!relativeDirectory.empty() && *relativeDirectory.begin() == ".."))
	{
		AppendAssetLog(std::format("Create asset failed: target is outside Assets {}", directory.string()));
		return false;
	}

	std::filesystem::create_directories(directory, errorCode);
	if (errorCode)
	{
		AppendAssetLog(std::format("Create asset failed: {}", errorCode.message()));
		return false;
	}

	const auto makeUniquePath = [&directory](std::string_view baseName, std::string_view extension) -> std::filesystem::path
	{
		std::filesystem::path candidate = directory / std::filesystem::path(std::string(baseName)).concat(std::string(extension));
		if (!std::filesystem::exists(candidate))
		{
			return candidate;
		}
		for (uint32_t suffix = 2; suffix < 10000; ++suffix)
		{
			candidate = directory / std::filesystem::path(std::format("{} {}{}", baseName, suffix, extension));
			if (!std::filesystem::exists(candidate))
			{
				return candidate;
			}
		}
		return {};
	};

	const auto defaultNameAndExtension = [](Editor::ProjectCreateAssetKind createKind) -> std::pair<std::string_view, std::string_view>
	{
		switch (createKind)
		{
		case Editor::ProjectCreateAssetKind::Folder:
			return { "New Folder", "" };
		case Editor::ProjectCreateAssetKind::Scene:
			return { "New Scene", ".scene" };
		case Editor::ProjectCreateAssetKind::Material:
			return { "New Material", ".material" };
		case Editor::ProjectCreateAssetKind::Skybox:
			return { "New Skybox", ".skybox" };
		case Editor::ProjectCreateAssetKind::Script:
			return { "NewScript", ".cpp" };
		case Editor::ProjectCreateAssetKind::Prefab:
			return { "New Prefab", ".prefab" };
		default:
			return { "New Asset", "" };
		}
	};

	const auto sanitizeProjectAssetName = [](std::string_view value, std::string_view fallback) -> std::string
	{
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
		{
			value.remove_prefix(1);
		}
		while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
		{
			value.remove_suffix(1);
		}

		std::string name(value.empty() ? fallback : value);
		if (const std::filesystem::path namePath(name); namePath.has_extension())
		{
			name = namePath.stem().string();
		}

		static constexpr std::string_view kInvalidCharacters = "<>:\"/\\|?*";
		for (char& character : name)
		{
			const auto unsignedCharacter = static_cast<unsigned char>(character);
			if (unsignedCharacter < 32 || kInvalidCharacters.find(character) != std::string_view::npos)
			{
				character = '_';
			}
		}

		while (!name.empty() && (name.back() == '.' || std::isspace(static_cast<unsigned char>(name.back()))))
		{
			name.pop_back();
		}
		while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
		{
			name.erase(name.begin());
		}

		return name.empty() ? std::string(fallback) : name;
	};

	const auto makeScriptTypeName = [](std::string value) -> std::string
	{
		for (char& character : value)
		{
			if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
			{
				character = '_';
			}
		}
		if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_'))
		{
			value.insert(value.begin(), '_');
		}
		return value;
	};

	const auto [defaultBaseName, extension] = defaultNameAndExtension(kind);
	const std::string baseName = sanitizeProjectAssetName(requestedName, defaultBaseName);
	std::filesystem::path createdPath;
	switch (kind)
	{
	case Editor::ProjectCreateAssetKind::Folder:
		createdPath = makeUniquePath(baseName, "");
		if (createdPath.empty())
		{
			AppendAssetLog("Create folder failed: could not allocate unique name.");
			return false;
		}
		std::filesystem::create_directory(createdPath, errorCode);
		break;
	case Editor::ProjectCreateAssetKind::Scene:
		createdPath = makeUniquePath(baseName, extension);
		break;
	case Editor::ProjectCreateAssetKind::Material:
		createdPath = makeUniquePath(baseName, extension);
		break;
	case Editor::ProjectCreateAssetKind::Skybox:
		createdPath = makeUniquePath(baseName, extension);
		break;
	case Editor::ProjectCreateAssetKind::Script:
		createdPath = makeUniquePath(baseName, extension);
		break;
	case Editor::ProjectCreateAssetKind::Prefab:
		createdPath = makeUniquePath(baseName, extension);
		break;
	default:
		return false;
	}

	if (errorCode)
	{
		AppendAssetLog(std::format("Create asset failed: {}", errorCode.message()));
		return false;
	}
	if (createdPath.empty())
	{
		AppendAssetLog("Create asset failed: could not allocate unique name.");
		return false;
	}

	if (kind != Editor::ProjectCreateAssetKind::Folder)
	{
		std::ofstream file(createdPath, std::ios::trunc);
		if (!file)
		{
			AppendAssetLog(std::format("Create asset failed: cannot write {}", createdPath.string()));
			return false;
		}
		switch (kind)
		{
		case Editor::ProjectCreateAssetKind::Scene:
			file << "{\n"
				<< "  \"fileVersion\": 1,\n"
				<< "  \"name\": \"" << createdPath.stem().string() << "\",\n"
				<< "  \"lighting\": {\n"
				<< "    \"ambientColor\": [0.62, 0.68, 0.78],\n"
				<< "    \"ambientIntensity\": 0.35,\n"
				<< "    \"exposure\": 1.0,\n"
				<< "    \"skybox\": {\n"
				<< "      \"enabled\": true,\n"
				<< "      \"zenithColor\": [0.32, 0.55, 0.95],\n"
				<< "      \"horizonColor\": [0.78, 0.88, 1.0],\n"
				<< "      \"groundColor\": [0.34, 0.39, 0.46],\n"
				<< "      \"sunColor\": [1.0, 0.86, 0.58],\n"
				<< "      \"sunDirection\": [-0.35, 0.78, -0.42],\n"
				<< "      \"intensity\": 1.0,\n"
				<< "      \"horizonHeight\": -0.04,\n"
				<< "      \"horizonBlend\": 1.35,\n"
				<< "      \"sunSize\": 0.035,\n"
				<< "      \"sunIntensity\": 1.15\n"
				<< "    }\n"
				<< "  },\n"
				<< "  \"entities\": []\n"
				<< "}\n";
			break;
		case Editor::ProjectCreateAssetKind::Material:
			file << "{\n"
				<< "  \"fileVersion\": 1,\n"
				<< "  \"type\": \"Material\",\n"
				<< "  \"shadingModel\": \"Phong\",\n"
				<< "  \"baseColor\": [1.0, 1.0, 1.0, 1.0],\n"
				<< "  \"useVertexColor\": false,\n"
				<< "  \"shininess\": 32.0\n"
				<< "}\n";
			break;
		case Editor::ProjectCreateAssetKind::Skybox:
			file << Rendering::BuildSkyboxAssetJson(Rendering::SkyboxSettings{}) << '\n';
			break;
		case Editor::ProjectCreateAssetKind::Script:
		{
			const std::string scriptTypeName = makeScriptTypeName(createdPath.stem().string());
			file << "// Native script placeholder.\n"
				<< "// Register a matching NativeScriptDefinition in ScriptRuntime before use.\n\n"
				<< "struct " << scriptTypeName << "\n"
				<< "{\n"
				<< "    void Start() {}\n"
				<< "    void Update(float deltaTime) { (void)deltaTime; }\n"
				<< "};\n";
			break;
		}
		case Editor::ProjectCreateAssetKind::Prefab:
			file << "{\n"
				<< "  \"fileVersion\": 1,\n"
				<< "  \"type\": \"Prefab\",\n"
				<< "  \"root\": null\n"
				<< "}\n";
			break;
		default:
			break;
		}
	}

	m_AssetFileSystem.RequestRefresh();
	ConfigureResourceSystem();
	AppendAssetLog(std::format("Created project asset: {}", createdPath.string()));
	return true;
}

bool Engine::ApplySkyboxAsset(const std::filesystem::path& skyboxPath)
{
	if (BlockEditSceneMutationDuringPlay("Apply Skybox"))
	{
		return false;
	}

	const Rendering::SkyboxAssetLoadResult loadResult = Rendering::LoadSkyboxAsset(skyboxPath);
	if (!loadResult.Success)
	{
		AppendAssetLog(std::format("Apply skybox failed: {}", loadResult.ErrorMessage));
		return false;
	}

	m_SkyboxSettings = Rendering::ClampSkyboxSettings(loadResult.Settings);
	MarkSceneDirty();
	AppendAssetLog(std::format("Applied skybox: {}", skyboxPath.string()));
	return true;
}

bool Engine::ExportProjectPackage()
{
	if (!m_Project)
	{
		AppendAssetLog("Export failed: no active project.");
		return false;
	}

	Editor::ExportProfileSettings profile;
	profile.OutputDirectory = (m_Project->RootPath / "Builds" / "Windows").lexically_normal();
	return ExportProjectPackage(profile);
}

bool Engine::ExportProjectPackage(const Editor::ExportProfileSettings& profile)
{
	if (!m_Project)
	{
		AppendAssetLog("Export failed: no active project.");
		return false;
	}
	if (!ConfirmSaveDirtyScene())
	{
		return false;
	}

	Projects::ProjectBuildRequest request;
	request.Project = *m_Project;
	request.OutputDirectory = profile.OutputDirectory.empty()
		? (m_Project->RootPath / "Builds" / "Windows").lexically_normal()
		: profile.OutputDirectory.lexically_normal();
	request.CopyAssets = profile.CopyAssets;
	request.CopyScenes = profile.CopyScenes;
	request.WriteManifest = profile.WriteManifest;
	Projects::ProjectBuildResult result = Projects::ProjectBuildService::BuildRuntimePackage(request);
	if (!result.Success)
	{
		AppendAssetLog(std::format("Export failed: {}", result.ErrorMessage));
		return false;
	}

	AppendAssetLog(std::format("Exported project package: {} ({} file(s))", result.OutputDirectory.string(), result.WrittenFiles.size()));
	if (profile.RevealAfterExport)
	{
		RevealAssetPath(result.OutputDirectory);
	}
	return true;
}

bool Engine::RestoreSceneFromLoadResult(const ScenePersistence::LoadSceneResult& loadResult, const std::filesystem::path& scenePath, bool restoreDirtyState)
{
	if (!loadResult.Success)
	{
		return false;
	}

	ClearProjectSceneRuntimeState();
	m_CurrentScenePath = scenePath.empty() ? GetDefaultScenePath() : std::filesystem::absolute(scenePath).lexically_normal();
	m_SampleMode = Samples::Benchmark::SampleMode::ProjectScene;
	m_LastSampleMode = m_SampleMode;
	m_AmbientColor = loadResult.AmbientColor;
	m_AmbientIntensity = loadResult.AmbientIntensity;
	m_Exposure = loadResult.Exposure;
	m_SkyboxSettings = loadResult.Skybox;

	std::vector<EntityId> createdEntityIds;
	createdEntityIds.reserve(loadResult.Entities.size());
	for (const ScenePersistence::LoadedSceneEntity& loadedEntity : loadResult.Entities)
	{
		createdEntityIds.push_back(CreateEntityFromLoadedSceneEntity(loadedEntity, {}));
	}
	for (size_t entityIndex = 0; entityIndex < loadResult.Entities.size(); ++entityIndex)
	{
		const ScenePersistence::LoadedSceneEntity& loadedEntity = loadResult.Entities[entityIndex];
		if (!loadedEntity.HasHierarchy || entityIndex >= createdEntityIds.size() || createdEntityIds[entityIndex] == InvalidEntityId)
		{
			continue;
		}
		EntityId parentEntityId = loadedEntity.ParentEntityId;
		if (parentEntityId == InvalidEntityId && loadedEntity.ParentIndex < createdEntityIds.size())
		{
			parentEntityId = createdEntityIds[loadedEntity.ParentIndex];
		}
		if (parentEntityId != InvalidEntityId)
		{
			static_cast<void>(m_Scene.SetParentEntity(createdEntityIds[entityIndex], parentEntityId, false));
		}
		m_Scene.EnsureHierarchyComponent(createdEntityIds[entityIndex]).Expanded = loadedEntity.HierarchyExpanded;
	}
	m_Scene.UpdateWorldTransforms();

	CreateEditorSceneEntities();
	ProcessSceneReferenceAutoLoads(false);
	SyncGameCameraFromSceneEntity();
	RebuildPhysicsWorldFromScene();
	SetSceneDirty(restoreDirtyState);
	RebuildWindowTitleBase();
	ResetFpsCounter();
	return true;
}

void Engine::SetPlayModeEnabled(bool enabled)
{
	if (enabled)
	{
		if (m_PlayState == Editor::EditorPlayState::Paused)
		{
			SetPlayPaused(false);
			return;
		}
		static_cast<void>(EnterPlayMode());
		return;
	}
	static_cast<void>(ExitPlayMode());
}

void Engine::SetPlayPaused(bool paused)
{
	if (IsRuntimeMode())
	{
		return;
	}

	if (paused)
	{
		if (m_PlayState != Editor::EditorPlayState::Play)
		{
			return;
		}
		m_PlayState = Editor::EditorPlayState::Paused;
		m_PlayStepRequested = false;
		AppendAssetLog("Play mode paused.");
		return;
	}

	if (m_PlayState != Editor::EditorPlayState::Paused)
	{
		return;
	}
	m_PlayState = Editor::EditorPlayState::Play;
	m_PlayStepRequested = false;
	AppendAssetLog("Play mode resumed.");
}

void Engine::StepPlayMode()
{
	if (IsRuntimeMode() || m_PlayState != Editor::EditorPlayState::Paused)
	{
		return;
	}

	m_PlayStepRequested = true;
	AppendAssetLog("Play mode single-frame step requested.");
}

void Engine::ResetPlayRuntimeScene()
{
	if (IsRuntimeMode() || !m_PlayScene || !IsRuntimePlaying())
	{
		AppendAssetLog("Play runtime reset skipped: Play mode is not active.");
		return;
	}

	m_SceneCommandBuffer.Clear();
	m_PlayStepRequested = false;
	m_PlayScene = m_Scene;
	const std::unordered_set<EntityId> excludedEntities = BuildRuntimeExpandedSceneReferenceSet();
	for (EntityId runtimeExpandedEntity : excludedEntities)
	{
		if (m_PlayScene->ContainsEntity(runtimeExpandedEntity))
		{
			static_cast<void>(m_PlayScene->DeleteEntity(runtimeExpandedEntity));
		}
	}
	m_PlayScene->UpdateWorldTransforms();
	m_ScriptRuntime.Reset();
	m_RuntimeCommandStack.Clear();
	SyncGameCameraFromSceneEntity();

	if (m_PhysicsSimulationEnabled)
	{
		m_PhysicsSimulationSnapshot.clear();
		for (const SceneEntity& entity : m_PlayScene->GetEntities())
		{
			if (const TransformComponent* transform = m_PlayScene->GetTransformComponent(entity.Id))
			{
				m_PhysicsSimulationSnapshot[entity.Id] = transform->LocalTransform;
			}
		}
		RebuildPhysicsWorldFromScene();
	}

	AppendAssetLog(std::format(
		"Play runtime scene reset from edit scene snapshot ({} entity/ies).",
		m_PlayScene->GetEntities().size()));
}

bool Engine::EnterPlayMode()
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Play mode is only available in Project Scene.");
		return false;
	}
	if (m_PlayState == Editor::EditorPlayState::Play
		|| m_PlayState == Editor::EditorPlayState::Paused
		|| m_PlayState == Editor::EditorPlayState::EnteringPlay)
	{
		return true;
	}
	if (!ConfirmSaveDirtyScene())
	{
		AppendAssetLog("Play mode entry cancelled.");
		return false;
	}

	m_PlayState = Editor::EditorPlayState::EnteringPlay;
	m_PlayStepRequested = false;
	m_PlayModeEditSceneSnapshot.reset();
	m_PlayModeRestoreScenePath = m_CurrentScenePath.empty() ? GetDefaultScenePath() : m_CurrentScenePath;
	m_PlayModeRestoreDirty = m_SceneDirty;
	m_PlayModeSnapshotScenePath = (m_Project->RootPath / "Temp" / "PlayModeSnapshot.scene").lexically_normal();

	const std::unordered_set<EntityId> excludedEntities = BuildRuntimeExpandedSceneReferenceSet();
	std::string errorMessage;
	if (!ScenePersistence::ScenePersistenceService::SaveScene(
		m_Scene,
		m_RenderState,
		*m_Project,
		m_PlayModeSnapshotScenePath,
		m_AmbientColor,
		m_AmbientIntensity,
		m_Exposure,
		m_SkyboxSettings,
		errorMessage,
		&excludedEntities))
	{
		m_PlayState = Editor::EditorPlayState::Edit;
		AppendAssetLog(std::format("Play mode snapshot failed: {}", errorMessage));
		return false;
	}

	ScenePersistence::LoadSceneResult editSnapshot = ScenePersistence::ScenePersistenceService::LoadScene(m_PlayModeSnapshotScenePath, *m_Project);
	if (!editSnapshot.Success)
	{
		m_PlayState = Editor::EditorPlayState::Edit;
		std::error_code errorCode;
		std::filesystem::remove(m_PlayModeSnapshotScenePath, errorCode);
		m_PlayModeSnapshotScenePath.clear();
		m_PlayModeRestoreScenePath.clear();
		m_PlayModeRestoreDirty = false;
		AppendAssetLog(std::format("Play mode snapshot clone failed: {}", editSnapshot.ErrorMessage));
		return false;
	}
	m_PlayModeEditSceneSnapshot = std::move(editSnapshot);
	const size_t editSnapshotEntityCount = m_PlayModeEditSceneSnapshot->Entities.size();
	m_PlayScene = m_Scene;
	for (EntityId runtimeExpandedEntity : excludedEntities)
	{
		if (m_PlayScene->ContainsEntity(runtimeExpandedEntity))
		{
			static_cast<void>(m_PlayScene->DeleteEntity(runtimeExpandedEntity));
		}
	}
	m_PlayScene->UpdateWorldTransforms();
	m_EditorCommandStack.Clear();
	m_RuntimeCommandStack.Clear();
	m_ScriptRuntime.Reset();
	m_PlayState = Editor::EditorPlayState::Play;
	RebuildPhysicsWorldFromScene();
	AppendAssetLog(std::format(
		"Entered Play mode with runtime scene clone ({} edit entity/ies, file fallback: {})",
		editSnapshotEntityCount,
		m_PlayModeSnapshotScenePath.string()));
	return true;
}

bool Engine::ExitPlayMode()
{
	if (m_PlayState == Editor::EditorPlayState::Edit)
	{
		return true;
	}
	if (!m_Project)
	{
		m_PlayState = Editor::EditorPlayState::Edit;
		return false;
	}

	m_PlayState = Editor::EditorPlayState::ExitingPlay;
	m_PlayScene.reset();
	ScenePersistence::LoadSceneResult loadResult;
	if (m_PlayModeEditSceneSnapshot)
	{
		loadResult = *m_PlayModeEditSceneSnapshot;
	}
	else
	{
		loadResult = ScenePersistence::ScenePersistenceService::LoadScene(m_PlayModeSnapshotScenePath, *m_Project);
	}
	if (!loadResult.Success)
	{
		m_PlayState = Editor::EditorPlayState::Edit;
		AppendAssetLog(std::format("Play mode restore failed: {}", loadResult.ErrorMessage));
		return false;
	}

	const std::filesystem::path restorePath = m_PlayModeRestoreScenePath.empty() ? GetDefaultScenePath() : m_PlayModeRestoreScenePath;
	if (!RestoreSceneFromLoadResult(loadResult, restorePath, m_PlayModeRestoreDirty))
	{
		m_PlayState = Editor::EditorPlayState::Edit;
		AppendAssetLog("Play mode restore failed: scene restore rejected snapshot.");
		return false;
	}

	std::error_code errorCode;
	std::filesystem::remove(m_PlayModeSnapshotScenePath, errorCode);
	m_PlayModeSnapshotScenePath.clear();
	m_PlayModeRestoreScenePath.clear();
	m_PlayScene.reset();
	m_PlayModeEditSceneSnapshot.reset();
	m_RuntimeCommandStack.Clear();
	m_PlayStepRequested = false;
	m_PlayModeRestoreDirty = false;
	m_ScriptRuntime.Reset();
	m_PlayState = Editor::EditorPlayState::Edit;
	AppendAssetLog("Exited Play mode; in-memory edit scene snapshot restored.");
	return true;
}

bool Engine::ConfirmSaveDirtyScene()
{
	if (IsRuntimeMode() || !m_SceneDirty || !m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		return true;
	}

	const int result = MessageBoxW(
		m_hMainWnd,
		L"현재 씬에 저장되지 않은 변경사항이 있습니다.\n저장할까요?",
		L"Unsaved Scene",
		MB_YESNOCANCEL | MB_ICONWARNING);

	if (result == IDCANCEL)
	{
		return false;
	}
	if (result == IDYES)
	{
		return SaveCurrentScene();
	}
	return true;
}

std::optional<std::filesystem::path> Engine::ShowOpenSceneDialog() const
{
	if (!m_Project)
	{
		return std::nullopt;
	}

	wchar_t filePathBuffer[MAX_PATH] = {};
	const std::filesystem::path initialDirectory = m_Project->RootPath / m_Project->ScenesRoot;
	OPENFILENAMEW openFileName = {
		.lStructSize = sizeof(OPENFILENAMEW),
		.hwndOwner = m_hMainWnd,
		.lpstrFilter = L"Engine Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0",
		.lpstrFile = filePathBuffer,
		.nMaxFile = static_cast<DWORD>(std::size(filePathBuffer)),
		.lpstrInitialDir = initialDirectory.c_str(),
		.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
		.lpstrDefExt = L"scene"
	};

	if (!GetOpenFileNameW(&openFileName))
	{
		return std::nullopt;
	}

	return std::filesystem::path(filePathBuffer);
}

std::optional<std::filesystem::path> Engine::ShowSaveSceneDialog() const
{
	if (!m_Project)
	{
		return std::nullopt;
	}

	wchar_t filePathBuffer[MAX_PATH] = {};
	const std::filesystem::path defaultPath = m_CurrentScenePath.empty() ? GetDefaultScenePath() : m_CurrentScenePath;
	const std::wstring defaultPathText = defaultPath.wstring();
	wcsncpy_s(filePathBuffer, defaultPathText.c_str(), _TRUNCATE);

	const std::filesystem::path initialDirectory = m_Project->RootPath / m_Project->ScenesRoot;
	OPENFILENAMEW openFileName = {
		.lStructSize = sizeof(OPENFILENAMEW),
		.hwndOwner = m_hMainWnd,
		.lpstrFilter = L"Engine Scene (*.scene)\0*.scene\0All Files (*.*)\0*.*\0",
		.lpstrFile = filePathBuffer,
		.nMaxFile = static_cast<DWORD>(std::size(filePathBuffer)),
		.lpstrInitialDir = initialDirectory.c_str(),
		.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR,
		.lpstrDefExt = L"scene"
	};

	if (!GetSaveFileNameW(&openFileName))
	{
		return std::nullopt;
	}

	return std::filesystem::path(filePathBuffer);
}

std::optional<std::filesystem::path> Engine::ShowOpenTextureDialog() const
{
	wchar_t filePathBuffer[MAX_PATH] = {};
	const std::filesystem::path initialDirectory = m_Project
		? m_Project->RootPath / m_Project->AssetRoot
		: std::filesystem::current_path();
	OPENFILENAMEW openFileName = {
		.lStructSize = sizeof(OPENFILENAMEW),
		.hwndOwner = m_hMainWnd,
		.lpstrFilter = L"Texture Files (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds\0All Files (*.*)\0*.*\0",
		.lpstrFile = filePathBuffer,
		.nMaxFile = static_cast<DWORD>(std::size(filePathBuffer)),
		.lpstrInitialDir = initialDirectory.c_str(),
		.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
	};

	if (!GetOpenFileNameW(&openFileName))
	{
		return std::nullopt;
	}

	return std::filesystem::path(filePathBuffer);
}

std::filesystem::path Engine::GetDefaultScenePath() const
{
	if (!m_Project)
	{
		return {};
	}

	return (m_Project->RootPath / m_Project->StartupScene).lexically_normal();
}

std::filesystem::path Engine::ResolveProjectScenePath(const std::filesystem::path& scenePath) const
{
	if (scenePath.empty())
	{
		return {};
	}
	if (scenePath.is_absolute() || !m_Project)
	{
		return std::filesystem::absolute(scenePath).lexically_normal();
	}

	std::array candidates = {
		(m_Project->RootPath / scenePath).lexically_normal(),
		(m_Project->RootPath / m_Project->ScenesRoot / scenePath).lexically_normal(),
		(m_Project->RootPath / m_Project->AssetRoot / scenePath).lexically_normal()
	};
	for (const std::filesystem::path& candidate : candidates)
	{
		std::error_code errorCode;
		if (std::filesystem::is_regular_file(candidate, errorCode))
		{
			return std::filesystem::absolute(candidate).lexically_normal();
		}
	}
	return std::filesystem::absolute(candidates.front()).lexically_normal();
}

std::unordered_set<EntityId> Engine::BuildRuntimeExpandedSceneReferenceSet() const
{
	std::unordered_set<EntityId> excludedEntities;
	for (const auto& loadedSceneReference : m_LoadedSceneReferenceEntities)
	{
		for (EntityId loadedEntityId : loadedSceneReference.second.LoadedEntities)
		{
			if (loadedEntityId != InvalidEntityId && m_Scene.ContainsEntity(loadedEntityId))
			{
				excludedEntities.insert(loadedEntityId);
			}
		}
	}
	return excludedEntities;
}

Editor::SceneReferenceRuntimeStatus Engine::GetSceneReferenceRuntimeStatus(EntityId entityId) const
{
	Editor::SceneReferenceRuntimeStatus status;
	const SceneReferenceComponent* sceneReference = m_Scene.GetSceneReferenceComponent(entityId);
	if (!sceneReference)
	{
		status.StatusText = "No SceneReference component.";
		return status;
	}

	status.ResolvedScenePath = ResolveProjectScenePath(sceneReference->ScenePath);
	std::error_code errorCode;
	status.FileExists = !status.ResolvedScenePath.empty() && std::filesystem::is_regular_file(status.ResolvedScenePath, errorCode);
	if (const auto loadedIt = m_LoadedSceneReferenceEntities.find(entityId); loadedIt != m_LoadedSceneReferenceEntities.end())
	{
		status.Loaded = true;
		status.Watching = loadedIt->second.LastWriteTime != std::filesystem::file_time_type{};
		status.PendingExternalReload = loadedIt->second.PendingExternalReload;
		const auto liveLoadedEntityCount = std::ranges::count_if(loadedIt->second.LoadedEntities, [this](EntityId loadedEntityId)
			{
				return loadedEntityId != InvalidEntityId && m_Scene.ContainsEntity(loadedEntityId);
			});
		status.LoadedEntityCount = static_cast<size_t>(liveLoadedEntityCount);
		if (!loadedIt->second.ScenePath.empty())
		{
			status.ResolvedScenePath = loadedIt->second.ScenePath;
		}
		if (status.PendingExternalReload)
		{
			status.StatusText = "External file changed; reload is pending until the scene is saved or manually reloaded.";
		}
		else
		{
			status.StatusText = status.Watching
				? "Loaded and watching for changes."
				: "Loaded; file timestamp is not available.";
		}
		return status;
	}

	if (sceneReference->ScenePath.empty())
	{
		status.StatusText = "No scene path assigned.";
	}
	else if (!m_Scene.IsComponentEnabled<SceneReferenceComponent>(entityId))
	{
		status.StatusText = "SceneReference component is disabled.";
	}
	else if (!sceneReference->LoadAdditively)
	{
		status.StatusText = "Load Additively is off; Open Scene will replace the active scene.";
	}
	else if (!status.FileExists)
	{
		status.StatusText = "Referenced scene file was not found.";
	}
	else if (sceneReference->AutoLoad)
	{
		status.StatusText = "Auto Load is enabled; this reference will expand when the scene opens.";
	}
	else
	{
		status.StatusText = "Ready to load as nested scene children.";
	}
	return status;
}

Editor::NestedSceneChildStatus Engine::GetNestedSceneChildStatus(EntityId entityId) const
{
	Editor::NestedSceneChildStatus status;
	for (const auto& [ownerEntity, loadedSceneReference] : m_LoadedSceneReferenceEntities)
	{
		if (std::ranges::find(loadedSceneReference.LoadedEntities, entityId) == loadedSceneReference.LoadedEntities.end())
		{
			continue;
		}

		const auto liveSiblingCount = std::ranges::count_if(loadedSceneReference.LoadedEntities, [this](EntityId loadedEntityId)
			{
				return loadedEntityId != InvalidEntityId && m_Scene.ContainsEntity(loadedEntityId);
			});
		status.IsNestedSceneChild = true;
		status.OwnerEntity = ownerEntity;
		status.SourceScenePath = loadedSceneReference.ScenePath;
		status.SiblingCount = static_cast<size_t>(liveSiblingCount);
		break;
	}
	return status;
}

bool Engine::MakeNestedSceneChildLocal(EntityId entityId)
{
	if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Make Local failed: entity no longer exists.");
		return false;
	}

	auto ownerIt = std::ranges::find_if(m_LoadedSceneReferenceEntities, [entityId](const auto& loadedSceneReference)
		{
			return std::ranges::find(loadedSceneReference.second.LoadedEntities, entityId) != loadedSceneReference.second.LoadedEntities.end();
		});
	if (ownerIt == m_LoadedSceneReferenceEntities.end())
	{
		AppendAssetLog(std::format("Make Local skipped: entity {} is not a nested scene child.", entityId));
		return false;
	}

	std::unordered_set<EntityId> trackedEntities(
		ownerIt->second.LoadedEntities.begin(),
		ownerIt->second.LoadedEntities.end());
	std::unordered_set<EntityId> localEntities;
	const auto collectTrackedSubtree = [this, &trackedEntities, &localEntities](auto&& self, EntityId currentEntity) -> void
		{
			if (currentEntity == InvalidEntityId ||
				!m_Scene.ContainsEntity(currentEntity) ||
				!trackedEntities.contains(currentEntity) ||
				!localEntities.insert(currentEntity).second)
			{
				return;
			}

			for (const EntityId childEntity : m_Scene.GetChildEntities(currentEntity))
			{
				self(self, childEntity);
			}
		};
	collectTrackedSubtree(collectTrackedSubtree, entityId);
	if (localEntities.empty())
	{
		AppendAssetLog(std::format("Make Local skipped: entity {} has no tracked nested scene subtree.", entityId));
		return false;
	}

	const EntityId ownerEntity = ownerIt->first;
	const std::filesystem::path sourceScenePath = ownerIt->second.ScenePath;
	std::erase_if(ownerIt->second.LoadedEntities, [&localEntities](EntityId loadedEntityId)
		{
			return localEntities.contains(loadedEntityId);
		});
	if (ownerIt->second.LoadedEntities.empty())
	{
		m_LoadedSceneReferenceEntities.erase(ownerIt);
	}

	m_Scene.SetSelectedEntity(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format(
		"Made nested scene subtree local: entity {} from owner {} ({} entity/entities, source {})",
		entityId,
		ownerEntity,
		localEntities.size(),
		sourceScenePath.empty() ? std::string("<unknown>") : sourceScenePath.string()));
	return true;
}

bool Engine::MakeNestedSceneChildLocalWithUndo(EntityId entityId)
{
	if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId))
	{
		AppendAssetLog("Make Local failed: entity no longer exists.");
		return false;
	}

	const auto ownerIt = std::ranges::find_if(m_LoadedSceneReferenceEntities, [entityId](const auto& loadedSceneReference)
		{
			return std::ranges::find(loadedSceneReference.second.LoadedEntities, entityId) != loadedSceneReference.second.LoadedEntities.end();
		});
	if (ownerIt == m_LoadedSceneReferenceEntities.end())
	{
		AppendAssetLog(std::format("Make Local skipped: entity {} is not a nested scene child.", entityId));
		return false;
	}

	std::unordered_set<EntityId> trackedEntities(ownerIt->second.LoadedEntities.begin(), ownerIt->second.LoadedEntities.end());
	std::vector<EntityId> affectedEntities;
	const auto collectTrackedSubtree = [this, &trackedEntities, &affectedEntities](auto&& self, EntityId currentEntity) -> void
		{
			if (currentEntity == InvalidEntityId ||
				!m_Scene.ContainsEntity(currentEntity) ||
				!trackedEntities.contains(currentEntity) ||
				std::ranges::find(affectedEntities, currentEntity) != affectedEntities.end())
			{
				return;
			}

			affectedEntities.push_back(currentEntity);
			for (const EntityId childEntity : m_Scene.GetChildEntities(currentEntity))
			{
				self(self, childEntity);
			}
		};
	collectTrackedSubtree(collectTrackedSubtree, entityId);
	if (affectedEntities.empty())
	{
		AppendAssetLog(std::format("Make Local skipped: entity {} has no tracked nested scene subtree.", entityId));
		return false;
	}

	const EntityId ownerEntity = ownerIt->first;
	const LoadedSceneReferenceState previousState = ownerIt->second;
	const std::string* entityName = m_Scene.GetEntityName(entityId);
	const std::string commandName = std::format(
		"Make Local {}",
		entityName && !entityName->empty() ? *entityName : std::format("Entity {}", entityId));

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = commandName,
		.Execute = [this, entityId, ownerEntity, affectedEntities]()
		{
			const auto loadedIt = m_LoadedSceneReferenceEntities.find(ownerEntity);
			if (loadedIt == m_LoadedSceneReferenceEntities.end())
			{
				AppendAssetLog(std::format("Make Local redo skipped: SceneReference owner {} is no longer tracking children.", ownerEntity));
				return;
			}

			const std::unordered_set<EntityId> affectedSet(affectedEntities.begin(), affectedEntities.end());
			std::erase_if(loadedIt->second.LoadedEntities, [&affectedSet](EntityId loadedEntityId)
				{
					return affectedSet.contains(loadedEntityId);
				});
			if (loadedIt->second.LoadedEntities.empty())
			{
				m_LoadedSceneReferenceEntities.erase(loadedIt);
			}
			if (m_Scene.ContainsEntity(entityId))
			{
				m_Scene.SetSelectedEntity(entityId);
			}
			MarkSceneDirty();
			AppendAssetLog(std::format(
				"Made nested scene subtree local: entity {} ({} entity/entities)",
				entityId,
				affectedEntities.size()));
		},
		.Undo = [this, entityId, ownerEntity, previousState]()
		{
			if (!m_Scene.ContainsEntity(ownerEntity))
			{
				AppendAssetLog(std::format("Undo Make Local skipped: SceneReference owner {} no longer exists.", ownerEntity));
				return;
			}

			LoadedSceneReferenceState restoredState = previousState;
			std::erase_if(restoredState.LoadedEntities, [this](EntityId loadedEntityId)
				{
					return loadedEntityId == InvalidEntityId || !m_Scene.ContainsEntity(loadedEntityId);
				});
			if (restoredState.LoadedEntities.empty())
			{
				m_LoadedSceneReferenceEntities.erase(ownerEntity);
				AppendAssetLog("Undo Make Local skipped: no live nested scene children remain to track.");
				return;
			}

			m_LoadedSceneReferenceEntities[ownerEntity] = std::move(restoredState);
			if (m_Scene.ContainsEntity(entityId))
			{
				m_Scene.SetSelectedEntity(entityId);
			}
			MarkSceneDirty();
			AppendAssetLog(std::format("Undo Make Local: entity {} is tracked as a nested scene child again.", entityId));
		}
	});
	return true;
}

void Engine::ProcessSceneReferenceAutoLoads(bool markDirty)
{
	if (!m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		return;
	}

	std::vector<EntityId> autoLoadEntities;
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		const SceneReferenceComponent* sceneReference = m_Scene.GetSceneReferenceComponent(entity.Id);
		if (!sceneReference ||
			!m_Scene.IsComponentEnabled<SceneReferenceComponent>(entity.Id) ||
			!sceneReference->AutoLoad ||
			!sceneReference->LoadAdditively ||
			sceneReference->ScenePath.empty())
		{
			continue;
		}
		autoLoadEntities.push_back(entity.Id);
	}

	size_t loadedCount = 0;
	for (EntityId entityId : autoLoadEntities)
	{
		if (m_Scene.ContainsEntity(entityId) && LoadSceneReference(entityId, markDirty))
		{
			++loadedCount;
		}
	}
	if (loadedCount > 0)
	{
		AppendAssetLog(std::format("SceneReference auto-load expanded {} reference(s).", loadedCount));
	}
}

void Engine::UpdateSceneReferenceHotReload(float deltaTime)
{
	if (m_LoadedSceneReferenceEntities.empty() ||
		!m_Project ||
		m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene ||
		IsRuntimePlaying())
	{
		m_SceneReferenceHotReloadElapsedSeconds = 0.0f;
		return;
	}

	m_SceneReferenceHotReloadElapsedSeconds += (std::max)(0.0f, deltaTime);
	constexpr float kSceneReferenceHotReloadIntervalSeconds = 1.0f;
	if (m_SceneReferenceHotReloadElapsedSeconds < kSceneReferenceHotReloadIntervalSeconds)
	{
		return;
	}
	m_SceneReferenceHotReloadElapsedSeconds = 0.0f;

	std::vector<EntityId> reloadEntities;
	for (auto& [entityId, state] : m_LoadedSceneReferenceEntities)
	{
		if (entityId == InvalidEntityId || !m_Scene.ContainsEntity(entityId) || state.ScenePath.empty())
		{
			continue;
		}

		if (state.PendingExternalReload)
		{
			if (!m_SceneDirty)
			{
				reloadEntities.push_back(entityId);
			}
			continue;
		}

		std::error_code errorCode;
		const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(state.ScenePath, errorCode);
		if (errorCode || lastWriteTime == state.LastWriteTime)
		{
			continue;
		}

		state.LastWriteTime = lastWriteTime;
		if (m_SceneDirty)
		{
			state.PendingExternalReload = true;
			AppendAssetLog(std::format(
				"Scene reference changed on disk; reload pending until dirty scene is saved or manual reload is requested: {}",
				state.ScenePath.string()));
			continue;
		}
		reloadEntities.push_back(entityId);
	}

	for (EntityId entityId : reloadEntities)
	{
		if (!m_Scene.ContainsEntity(entityId))
		{
			continue;
		}
		AppendAssetLog(std::format("Scene reference changed on disk, reloading entity {}.", entityId));
		static_cast<void>(LoadSceneReference(entityId, false));
	}
}

void Engine::ClearProjectSceneRuntimeState()
{
	SetPhysicsSimulationEnabled(false);
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		DestroyTextureResourcesForEntity(entityId);
	}

	m_AssetHotReloadService.Clear();
	m_RuntimeAssetRegistry.Clear();
	++m_AssetSceneGeneration;
	m_RenderState.Reset();
	m_EditorCommandStack.Clear();
	m_RuntimeCommandStack.Clear();
	m_ScriptRuntime.Reset();
	m_Scene.Clear();
	m_PlayScene.reset();
	m_LoadedSceneReferenceEntities.clear();
	m_MeshRestoreGenerations.clear();
	m_MeshRestoreRequests.clear();
	m_MeshRestoreConflictResults.clear();
	m_PlayState = Editor::EditorPlayState::Edit;
	m_SpiderEntity = InvalidEntityId;
	m_GameCameraEntity = InvalidEntityId;
	m_KeyLightEntity = InvalidEntityId;
	m_PhysicsWorld.Clear();
	m_PhysicsSimulationSnapshot.clear();
}

bool Engine::IsStaleMeshRestoreResult(const Asset::AssetImportResult& result) const
{
	if (!result.Restore.HasTargetEntity || result.Restore.RestoreGeneration == 0)
	{
		return false;
	}

	const auto generationIt = m_MeshRestoreGenerations.find(result.Restore.TargetEntity);
	return generationIt == m_MeshRestoreGenerations.end() || generationIt->second != result.Restore.RestoreGeneration;
}

bool Engine::HasMeshRestoreConflict(const Asset::AssetImportResult& result, std::string& conflictReason) const
{
	if (!result.Restore.HasTargetEntity || result.Restore.RestoreGeneration == 0)
	{
		return false;
	}

	const auto restoreIt = m_MeshRestoreRequests.find(result.Restore.TargetEntity);
	if (restoreIt == m_MeshRestoreRequests.end() || restoreIt->second.Generation != result.Restore.RestoreGeneration)
	{
		return false;
	}

	const MeshRestoreRequestState& state = restoreIt->second;
	if (!state.Pending)
	{
		conflictReason = state.Cancelled
			? "Mesh restore was cancelled before the import completed."
			: "Mesh restore is no longer pending.";
		return true;
	}

	if (!state.SourcePrefabPath.empty() && state.SourcePrefabWriteTime)
	{
		std::error_code writeTimeError;
		const std::filesystem::file_time_type currentWriteTime = std::filesystem::last_write_time(state.SourcePrefabPath, writeTimeError);
		if (writeTimeError)
		{
			conflictReason = std::format(
				"Source prefab became unavailable while Mesh restore was importing: {}",
				state.SourcePrefabPath.string());
			return true;
		}
		if (currentWriteTime != *state.SourcePrefabWriteTime)
		{
			conflictReason = std::format(
				"Source prefab changed on disk while Mesh restore was importing: {}",
				state.SourcePrefabPath.string());
			return true;
		}
	}

	const std::string currentSignature = BuildMeshRestoreSignature(result.Restore.TargetEntity);
	if (!state.ExpectedCurrentMeshSignature.empty() && currentSignature != state.ExpectedCurrentMeshSignature)
	{
		conflictReason = "Current Entity Mesh changed while restore import was running. Restore result was discarded.";
		return true;
	}

	return false;
}

void Engine::MarkMeshRestoreFailed(const Asset::AssetImportResult& result)
{
	if (!result.Restore.HasTargetEntity || result.Restore.RestoreGeneration == 0)
	{
		return;
	}

	const EntityId entityId = result.Restore.TargetEntity;
	if (IsStaleMeshRestoreResult(result))
	{
		return;
	}

	MeshRestoreRequestState& state = m_MeshRestoreRequests[entityId];
	state.Generation = result.Restore.RestoreGeneration;
	state.SourcePath = result.SourcePath;
	state.Pending = false;
	state.Failed = true;
	state.Cancelled = false;
	state.Conflicted = false;
	state.ImportedVertexCount = 0;
	state.ImportedIndexCount = 0;
	state.ImportedMaterialCount = 0;
	state.MaterialDiffLines.clear();
	state.MaterialDiffRows.clear();
	state.Message = result.ErrorMessage.empty() ? "Mesh restore import failed." : result.ErrorMessage;
}

void Engine::MarkSceneDirty()
{
	if (IsRuntimeMode() || !m_Project || m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		return;
	}

	SetSceneDirty(true);
}

void Engine::SetSceneDirty(bool dirty)
{
	if (m_SceneDirty == dirty)
	{
		return;
	}

	m_SceneDirty = dirty;
	RebuildWindowTitleBase();
	ResetFpsCounter();
}

void Engine::MoveEntityInHierarchy(EntityId movedEntity, EntityId targetEntity, Editor::EntityDropPlacement placement)
{
	if (movedEntity == InvalidEntityId
		|| targetEntity == InvalidEntityId
		|| movedEntity == targetEntity
		|| !m_Scene.ContainsEntity(movedEntity)
		|| !m_Scene.ContainsEntity(targetEntity)
		|| m_Scene.IsDescendantOf(targetEntity, movedEntity))
	{
		return;
	}

	bool moved = false;
	if (placement == Editor::EntityDropPlacement::AsChild)
	{
		moved = m_Scene.SetParentEntity(movedEntity, targetEntity, true);
	}
	else
	{
		const EntityId targetParent = m_Scene.GetParentEntity(targetEntity);
		static_cast<void>(m_Scene.SetParentEntity(movedEntity, targetParent, true));
		moved = placement == Editor::EntityDropPlacement::Before
			? m_Scene.MoveEntityBefore(movedEntity, targetEntity)
			: m_Scene.MoveEntityAfter(movedEntity, targetEntity);
	}
	if (moved)
	{
		MarkSceneDirty();
		const char* placementName = placement == Editor::EntityDropPlacement::Before
			? "before"
			: placement == Editor::EntityDropPlacement::After ? "after" : "under";
		AppendAssetLog(std::format("Moved entity {} {} entity {}", movedEntity, placementName, targetEntity));
	}
}

void Engine::MoveEntitiesInHierarchyWithUndo(std::vector<EntityId> movedEntities, EntityId targetEntity, Editor::EntityDropPlacement placement)
{
	if (BlockEditSceneMutationDuringPlay("Move Entity"))
	{
		return;
	}

	if (targetEntity == InvalidEntityId || !m_Scene.ContainsEntity(targetEntity))
	{
		return;
	}

	std::vector<EntityId> orderedMovedEntities;
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		if (entity.Id == targetEntity || std::ranges::find(movedEntities, entity.Id) == movedEntities.end())
		{
			continue;
		}
		if (!m_Scene.ContainsEntity(entity.Id) || m_Scene.IsDescendantOf(targetEntity, entity.Id))
		{
			return;
		}
		if (std::ranges::find(orderedMovedEntities, entity.Id) == orderedMovedEntities.end())
		{
			orderedMovedEntities.push_back(entity.Id);
		}
	}

	std::erase_if(orderedMovedEntities, [this, &orderedMovedEntities](EntityId entityId)
		{
			const EntityId parentEntity = m_Scene.GetParentEntity(entityId);
			return parentEntity != InvalidEntityId
				&& std::ranges::find(orderedMovedEntities, parentEntity) != orderedMovedEntities.end();
		});

	if (orderedMovedEntities.empty())
	{
		return;
	}

	struct MoveRecord
	{
		EntityId Entity = InvalidEntityId;
		EntityId PreviousParent = InvalidEntityId;
		size_t PreviousIndex = static_cast<size_t>(-1);
		Math::Transform PreviousLocalTransform = Math::Transform::Identity();
	};

	auto records = std::make_shared<std::vector<MoveRecord>>();
	records->reserve(orderedMovedEntities.size());
	for (EntityId entityId : orderedMovedEntities)
	{
		Math::Transform previousTransform = Math::Transform::Identity();
		if (const TransformComponent* transform = m_Scene.GetTransformComponent(entityId))
		{
			previousTransform = transform->LocalTransform;
		}
		records->push_back(MoveRecord{
			.Entity = entityId,
			.PreviousParent = m_Scene.GetParentEntity(entityId),
			.PreviousIndex = m_Scene.GetEntityIndex(entityId),
			.PreviousLocalTransform = previousTransform
		});
	}

	const auto applyMove = [this, records, targetEntity, placement]() -> bool
	{
		if (!m_Scene.ContainsEntity(targetEntity))
		{
			return false;
		}

		std::vector<EntityId> currentEntities;
		currentEntities.reserve(records->size());
		for (const MoveRecord& record : *records)
		{
			if (record.Entity != targetEntity && m_Scene.ContainsEntity(record.Entity))
			{
				currentEntities.push_back(record.Entity);
			}
		}
		if (currentEntities.empty())
		{
			return false;
		}

		if (placement == Editor::EntityDropPlacement::AsChild)
		{
			EntityId insertAfter = targetEntity;
			for (EntityId entityId : currentEntities)
			{
				static_cast<void>(m_Scene.SetParentEntity(entityId, targetEntity, true));
				static_cast<void>(m_Scene.MoveEntityAfter(entityId, insertAfter));
				insertAfter = entityId;
			}
		}
		else
		{
			const EntityId targetParent = m_Scene.GetParentEntity(targetEntity);
			if (placement == Editor::EntityDropPlacement::Before)
			{
				for (EntityId entityId : currentEntities)
				{
					static_cast<void>(m_Scene.SetParentEntity(entityId, targetParent, true));
					static_cast<void>(m_Scene.MoveEntityBefore(entityId, targetEntity));
				}
			}
			else
			{
				EntityId insertAfter = targetEntity;
				for (EntityId entityId : currentEntities)
				{
					static_cast<void>(m_Scene.SetParentEntity(entityId, targetParent, true));
					static_cast<void>(m_Scene.MoveEntityAfter(entityId, insertAfter));
					insertAfter = entityId;
				}
			}
		}

		m_Scene.UpdateWorldTransforms();
		return true;
	};

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Move {} entities", records->size()),
		.Execute = [this, records, targetEntity, placement, applyMove]()
		{
			if (applyMove())
			{
				MarkSceneDirty();
				const char* placementName = placement == Editor::EntityDropPlacement::Before
					? "before"
					: placement == Editor::EntityDropPlacement::After ? "after" : "under";
				AppendAssetLog(std::format("Moved {} selected entities {} entity {}", records->size(), placementName, targetEntity));
			}
		},
		.Undo = [this, records]()
		{
			for (MoveRecord& record : *records)
			{
				if (!m_Scene.ContainsEntity(record.Entity))
				{
					continue;
				}
				if (TransformComponent* transform = m_Scene.GetTransformComponent(record.Entity))
				{
					transform->LocalTransform = record.PreviousLocalTransform;
				}
				static_cast<void>(m_Scene.SetParentEntity(record.Entity, record.PreviousParent, false));
				static_cast<void>(m_Scene.MoveEntityToIndex(record.Entity, record.PreviousIndex));
			}
			m_Scene.UpdateWorldTransforms();
			MarkSceneDirty();
			AppendAssetLog(std::format("Undo move for {} selected entities", records->size()));
		}
	});
}

void Engine::SetEntitySceneVisibilityWithUndo(EntityId entityId, bool visible)
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	const bool previousVisible = m_Scene.IsEntityVisibleInScene(entityId);
	if (previousVisible == visible)
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = visible ? "Show Entity In Scene" : "Hide Entity In Scene",
		.Execute = [this, entityId, visible]()
		{
			if (m_Scene.SetEntityVisibleInScene(entityId, visible))
			{
				MarkSceneDirty();
				AppendAssetLog(std::format("{} entity {} in Scene View", visible ? "Showed" : "Hid", entityId));
			}
		},
		.Undo = [this, entityId, previousVisible]()
		{
			if (m_Scene.SetEntityVisibleInScene(entityId, previousVisible))
			{
				MarkSceneDirty();
			}
		}
	});
}

void Engine::SetEntityScenePickabilityWithUndo(EntityId entityId, bool pickable)
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	const bool previousPickable = m_Scene.IsEntityPickableInScene(entityId);
	if (previousPickable == pickable)
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = pickable ? "Enable Scene Picking" : "Disable Scene Picking",
		.Execute = [this, entityId, pickable]()
		{
			if (m_Scene.SetEntityPickableInScene(entityId, pickable))
			{
				MarkSceneDirty();
				AppendAssetLog(std::format("{} scene picking for entity {}", pickable ? "Enabled" : "Disabled", entityId));
			}
		},
		.Undo = [this, entityId, previousPickable]()
		{
			if (m_Scene.SetEntityPickableInScene(entityId, previousPickable))
			{
				MarkSceneDirty();
			}
		}
	});
}

void Engine::AlignGameCameraToSceneCamera()
{
	if (m_GameCameraEntity == InvalidEntityId)
	{
		CreateEditorSceneEntities();
	}
	if (m_GameCameraEntity == InvalidEntityId)
	{
		return;
	}

	m_Camera.SetTransform(m_SceneCamera.GetTransform());
	m_Camera.SetLens(m_SceneCamera.GetFovY(), m_Camera.GetAspect(), m_SceneCamera.GetNearZ(), m_SceneCamera.GetFarZ());
	SyncRuntimeCameraToGameCameraEntity();
	m_BenchmarkRunner.SetSpawnView(m_Camera);
	MarkSceneDirty();
	AppendAssetLog("Aligned Game Camera to Scene camera.");
}

void Engine::AlignSceneCameraToGameCamera()
{
	m_SceneCamera.SetTransform(m_Camera.GetTransform());
	m_SceneCamera.SetLens(m_Camera.GetFovY(), m_SceneCamera.GetAspect(), m_Camera.GetNearZ(), m_Camera.GetFarZ());
	AppendAssetLog("Aligned Scene camera to Game Camera.");
}

void Engine::SetPhysicsSimulationEnabled(bool enabled)
{
	if (enabled == m_PhysicsSimulationEnabled)
	{
		return;
	}

	if (enabled && m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
	{
		AppendAssetLog("Physics simulation is only available in Project Scene.");
		return;
	}

	if (enabled)
	{
		Scene& runtimeScene = GetRuntimeScene();
		if (!m_PhysicsWorld.IsInitialized() && !m_PhysicsWorld.Initialize())
		{
			AppendAssetLog("Physics simulation failed to initialize PhysX.");
			return;
		}

		m_PhysicsSimulationSnapshot.clear();
		for (const SceneEntity& entity : runtimeScene.GetEntities())
		{
			if (const TransformComponent* transform = runtimeScene.GetTransformComponent(entity.Id))
			{
				m_PhysicsSimulationSnapshot[entity.Id] = transform->LocalTransform;
			}
		}
		RebuildPhysicsWorldFromScene();
		m_PhysicsSimulationEnabled = true;
		uint32_t dynamicGravityActorCount = 0;
		uint32_t staticColliderCount = 0;
		for (const SceneEntity& entity : runtimeScene.GetEntities())
		{
			if (!runtimeScene.GetColliderComponent(entity.Id) || !runtimeScene.IsColliderEnabled(entity.Id))
			{
				continue;
			}

			const RigidBodyComponent* rigidBody = runtimeScene.GetRigidBodyComponent(entity.Id);
			if (rigidBody && runtimeScene.IsRigidBodyEnabled(entity.Id) && rigidBody->Type == Physics::RigidBodyType::Dynamic && rigidBody->UseGravity)
			{
				++dynamicGravityActorCount;
			}
			else
			{
				++staticColliderCount;
			}
		}
		AppendAssetLog(std::format(
			"Physics simulation enabled. Gravity=(0.00, -9.81, 0.00), dynamic gravity actors={}, static/kinematic colliders={}",
			dynamicGravityActorCount,
			staticColliderCount));
		if (dynamicGravityActorCount == 0)
		{
			AppendAssetLog("Physics note: primitives are Collider-only static until a Dynamic Rigidbody is added.");
		}
		return;
	}

	m_PhysicsSimulationEnabled = false;
	m_PhysicsWorld.Clear();
	Scene& runtimeScene = GetRuntimeScene();
	for (const auto& [entityId, transformSnapshot] : m_PhysicsSimulationSnapshot)
	{
		if (TransformComponent* transform = runtimeScene.GetTransformComponent(entityId))
		{
			transform->LocalTransform = transformSnapshot;
			transform->UpdateWorld();
		}
	}
	m_PhysicsSimulationSnapshot.clear();
	AppendAssetLog("Physics simulation disabled; transforms restored.");
}

void Engine::RebuildPhysicsWorldFromScene()
{
	m_PhysicsWorld.Clear();
	if (!m_PhysicsWorld.IsInitialized())
	{
		return;
	}

	Scene& runtimeScene = GetRuntimeScene();
	for (const SceneEntity& entity : runtimeScene.GetEntities())
	{
		if (runtimeScene.GetColliderComponent(entity.Id) && runtimeScene.IsColliderEnabled(entity.Id))
		{
			m_PhysicsWorld.CreateOrUpdateActor(entity.Id, runtimeScene);
		}
	}
}

void Engine::MarkPhysicsActorDirty(EntityId entityId)
{
	if (!m_PhysicsSimulationEnabled)
	{
		return;
	}

	Scene& runtimeScene = GetRuntimeScene();
	if (!runtimeScene.ContainsEntity(entityId) || !runtimeScene.GetColliderComponent(entityId) || !runtimeScene.IsColliderEnabled(entityId))
	{
		m_PhysicsWorld.RemoveActor(entityId);
		return;
	}

	m_PhysicsWorld.CreateOrUpdateActor(entityId, runtimeScene);
}

void Engine::CreateDefaultColliderForPrimitive(EntityId entityId, Asset::PrimitiveMeshKind kind)
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	ColliderComponent& collider = m_Scene.EnsureColliderComponent(entityId);
	collider.Offset = { 0.0f, 0.0f, 0.0f };
	collider.IsTrigger = false;
	switch (kind)
	{
	case Asset::PrimitiveMeshKind::Sphere:
		collider.Shape = Physics::ColliderShape::Sphere;
		collider.Radius = 0.5f;
		collider.Size = { 1.0f, 1.0f, 1.0f };
		collider.Height = 1.0f;
		break;
	case Asset::PrimitiveMeshKind::Capsule:
		collider.Shape = Physics::ColliderShape::Capsule;
		collider.Radius = 0.35f;
		collider.Height = 1.6f;
		collider.Size = { 0.7f, 1.6f, 0.7f };
		break;
	case Asset::PrimitiveMeshKind::Plane:
		collider.Shape = Physics::ColliderShape::Plane;
		collider.Size = { 10.0f, 0.01f, 10.0f };
		collider.Radius = 0.5f;
		collider.Height = 1.0f;
		break;
	case Asset::PrimitiveMeshKind::Cube:
	default:
		collider.Shape = Physics::ColliderShape::Box;
		collider.Size = { 1.0f, 1.0f, 1.0f };
		collider.Radius = 0.5f;
		collider.Height = 1.0f;
		break;
	}

	static_cast<void>(m_Scene.EnsurePhysicsMaterialComponent(entityId));
	MarkPhysicsActorDirty(entityId);
}

void Engine::CreateGeneratedColliderForImportedModel(EntityId entityId, const BoundsComponent& bounds)
{
	if (!m_Scene.ContainsEntity(entityId) || m_Scene.GetColliderComponent(entityId))
	{
		return;
	}

	const DirectX::XMFLOAT3 size = {
		(std::max)(bounds.LocalMax.x - bounds.LocalMin.x, 0.01f),
		(std::max)(bounds.LocalMax.y - bounds.LocalMin.y, 0.01f),
		(std::max)(bounds.LocalMax.z - bounds.LocalMin.z, 0.01f)
	};
	ColliderComponent& collider = m_Scene.EnsureColliderComponent(entityId);
	collider.Shape = Physics::ColliderShape::Box;
	collider.Size = size;
	collider.Offset = {
		(bounds.LocalMin.x + bounds.LocalMax.x) * 0.5f,
		(bounds.LocalMin.y + bounds.LocalMax.y) * 0.5f,
		(bounds.LocalMin.z + bounds.LocalMax.z) * 0.5f
	};
	collider.Radius = (std::max)((std::max)(size.x, size.z) * 0.5f, 0.01f);
	collider.Height = (std::max)(size.y, 0.01f);
	collider.IsTrigger = false;
	static_cast<void>(m_Scene.EnsurePhysicsMaterialComponent(entityId));
	MarkPhysicsActorDirty(entityId);
	AppendAssetLog(std::format("Generated Box collider for imported model entity {}", entityId));
}

EntityId Engine::CreatePrimitiveEntity(Asset::PrimitiveMeshKind kind, EntityId parentEntity)
{
	if (kind == Asset::PrimitiveMeshKind::None)
	{
		return InvalidEntityId;
	}

	const std::string primitiveName(Asset::PrimitiveMeshKindToString(kind));
	const EntityId entityId = m_Scene.CreateEntity(primitiveName);
	const DirectX::XMFLOAT3 cameraPosition = m_SceneCamera.GetPosition();
	const DirectX::XMFLOAT3 cameraForward = m_SceneCamera.GetForward();
	const DirectX::XMVECTOR target = DirectX::XMVectorAdd(
		DirectX::XMLoadFloat3(&cameraPosition),
		DirectX::XMVectorScale(DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraForward)), 5.0f));
	DirectX::XMFLOAT3 translation = {};
	DirectX::XMStoreFloat3(&translation, target);

	if (!ApplyPrimitiveMeshToEntity(entityId, kind, Math::Transform(translation, Math::IdentityQuaternion(), Math::OneVector3())))
	{
		static_cast<void>(m_Scene.DeleteEntity(entityId));
		return InvalidEntityId;
	}
	if (parentEntity != InvalidEntityId && m_Scene.ContainsEntity(parentEntity))
	{
		static_cast<void>(m_Scene.SetParentEntity(entityId, parentEntity, true));
	}

	m_Scene.SetSelectedEntity(entityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("Created primitive {} entity {}", primitiveName, entityId));
	return entityId;
}

void Engine::CreatePrimitiveEntityWithUndo(Asset::PrimitiveMeshKind kind, EntityId parentEntity)
{
	if (BlockEditSceneMutationDuringPlay("Create Primitive"))
	{
		return;
	}

	if (kind == Asset::PrimitiveMeshKind::None)
	{
		return;
	}

	const std::string primitiveName(Asset::PrimitiveMeshKindToString(kind));
	const auto createdEntity = std::make_shared<EntityId>(InvalidEntityId);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Create {}", primitiveName),
		.Execute = [this, kind, parentEntity, createdEntity]()
		{
			*createdEntity = CreatePrimitiveEntity(kind, parentEntity);
		},
		.Undo = [this, createdEntity]()
		{
			if (*createdEntity != InvalidEntityId && m_Scene.ContainsEntity(*createdEntity))
			{
				DeleteEntityFromHierarchy(*createdEntity);
				*createdEntity = InvalidEntityId;
			}
		}
	});
}

bool Engine::ApplyPrimitiveMeshToEntity(EntityId entityId, Asset::PrimitiveMeshKind kind, const Math::Transform& localTransform, bool createDefaultCollider)
{
	std::unique_ptr<Asset::StaticMeshAsset> mesh = Asset::CreatePrimitiveMesh(kind);
	if (!mesh || !m_Scene.ContainsEntity(entityId))
	{
		return false;
	}

	const BoundsComponent bounds = ComputeMeshBounds(*mesh);
	m_Scene.ReplaceEntityModel(entityId, std::move(mesh), { CpuMaterialTexture{} }, bounds);

	TransformComponent& transform = m_Scene.EnsureTransformComponent(entityId);
	transform.LocalTransform = localTransform;
	transform.UpdateWorld();

	if (std::ranges::find(m_RenderState.RenderEntities, entityId) == m_RenderState.RenderEntities.end())
	{
		m_RenderState.RenderEntities.push_back(entityId);
	}
	if (m_Scene.GetPrimaryRenderableEntity() == InvalidEntityId)
	{
		m_Scene.SetPrimaryRenderableEntity(entityId);
	}
	m_RenderState.EntityMaterialTransparency[entityId] = { false };
	if (createDefaultCollider)
	{
		CreateDefaultColliderForPrimitive(entityId, kind);
	}

	const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (meshAsset && !EnsureGeometryBufferCapacity(meshAsset->Vertices.size(), meshAsset->Indices.size()))
	{
		AppendAssetLog(std::format("Geometry buffer resize failed for primitive entity {}", entityId));
	}

	if (!CreateTextureResourcesForEntity(entityId))
	{
		AppendAssetLog(std::format("GPU texture upload failed for primitive entity {}", entityId));
	}

	return true;
}

void Engine::AddComponentToEntity(EntityId entityId, SceneComponentKind kind)
{
	if (BlockEditSceneMutationDuringPlay("Add Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	bool added = false;
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		if (!m_Scene.GetMeshComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureMeshComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::Animator:
		if (!m_Scene.GetAnimatorComponent(entityId))
		{
			const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
			if (meshAsset && meshAsset->IsAnimated && !meshAsset->Animations.empty())
			{
				static_cast<void>(m_Scene.EnsureAnimatorComponent(entityId));
				added = true;
			}
		}
		break;
	case SceneComponentKind::Camera:
		if (!m_Scene.GetCameraComponent(entityId))
		{
			CameraComponent& camera = m_Scene.EnsureCameraComponent(entityId);
			camera.IsGameCamera = false;
			added = true;
		}
		break;
	case SceneComponentKind::Light:
		if (!m_Scene.GetLightComponent(entityId))
		{
			LightComponent& light = m_Scene.EnsureLightComponent(entityId);
			light.Type = LightType::Directional;
			light.Color = { 1.0f, 0.95f, 0.82f };
			light.Intensity = 3.25f;
			light.Range = 450.0f;
			light.Enabled = true;
			added = true;
		}
		break;
	case SceneComponentKind::RigidBody:
		if (!m_Scene.GetRigidBodyComponent(entityId))
		{
			RigidBodyComponent& rigidBody = m_Scene.EnsureRigidBodyComponent(entityId);
			rigidBody.Type = Physics::RigidBodyType::Dynamic;
			rigidBody.Mass = 1.0f;
			rigidBody.LinearDamping = 0.05f;
			rigidBody.AngularDamping = 0.05f;
			rigidBody.UseGravity = true;
			rigidBody.LinearVelocity = { 0.0f, 0.0f, 0.0f };
			rigidBody.AngularVelocity = { 0.0f, 0.0f, 0.0f };
			added = true;
		}
		break;
	case SceneComponentKind::Collider:
		if (!m_Scene.GetColliderComponent(entityId))
		{
			ColliderComponent& collider = m_Scene.EnsureColliderComponent(entityId);
			collider.Shape = Physics::ColliderShape::Box;
			collider.Size = { 1.0f, 1.0f, 1.0f };
			collider.Radius = 0.5f;
			collider.Height = 1.0f;
			collider.Offset = { 0.0f, 0.0f, 0.0f };
			collider.IsTrigger = false;
			added = true;
		}
		break;
	case SceneComponentKind::PhysicsMaterial:
		if (!m_Scene.GetPhysicsMaterialComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsurePhysicsMaterialComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::PrefabInstance:
		if (!m_Scene.GetPrefabInstanceComponent(entityId))
		{
			PrefabInstanceComponent& prefab = m_Scene.EnsurePrefabInstanceComponent(entityId);
			if (const std::string* name = m_Scene.GetEntityName(entityId))
			{
				prefab.SourceName = *name;
			}
			added = true;
		}
		break;
	case SceneComponentKind::SceneReference:
		if (!m_Scene.GetSceneReferenceComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureSceneReferenceComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::Script:
		if (!m_Scene.GetScriptComponent(entityId))
		{
			ScriptComponent& script = m_Scene.EnsureScriptComponent(entityId);
			script.ClassName = "GameScript";
			script.Language = ScriptLanguage::Native;
			added = true;
		}
		break;
	case SceneComponentKind::Sprite2D:
		if (!m_Scene.GetSprite2DComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureSprite2DComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::UiElement:
		if (!m_Scene.GetUiElementComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureUiElementComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::AudioSource:
		if (!m_Scene.GetAudioSourceComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureAudioSourceComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::NavigationAgent:
		if (!m_Scene.GetNavigationAgentComponent(entityId))
		{
			static_cast<void>(m_Scene.EnsureNavigationAgentComponent(entityId));
			added = true;
		}
		break;
	case SceneComponentKind::NetworkIdentity:
		if (!m_Scene.GetNetworkIdentityComponent(entityId))
		{
			NetworkIdentityComponent& network = m_Scene.EnsureNetworkIdentityComponent(entityId);
			network.NetworkId = (static_cast<uint64_t>(entityId) << 32u) | static_cast<uint64_t>(m_AssetSceneGeneration & 0xffffffffu);
			added = true;
		}
		break;
	default:
		break;
	}

	if (!added)
	{
		return;
	}

	if (kind == SceneComponentKind::RigidBody || kind == SceneComponentKind::Collider || kind == SceneComponentKind::PhysicsMaterial)
	{
		MarkPhysicsActorDirty(entityId);
	}
	MarkSceneDirty();
	AppendAssetLog(std::format("Added {} component to entity {}", SceneComponentKindName(kind), entityId));
}

void Engine::AddComponentToEntityWithUndo(EntityId entityId, SceneComponentKind kind)
{
	if (BlockEditSceneMutationDuringPlay("Add Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	const bool alreadyHasComponent = [&]()
	{
		switch (kind)
		{
		case SceneComponentKind::Mesh:
			return m_Scene.GetMeshComponent(entityId) != nullptr;
		case SceneComponentKind::Animator:
			return m_Scene.GetAnimatorComponent(entityId) != nullptr;
		case SceneComponentKind::Camera:
			return m_Scene.GetCameraComponent(entityId) != nullptr;
		case SceneComponentKind::Light:
			return m_Scene.GetLightComponent(entityId) != nullptr;
		case SceneComponentKind::RigidBody:
			return m_Scene.GetRigidBodyComponent(entityId) != nullptr;
		case SceneComponentKind::Collider:
			return m_Scene.GetColliderComponent(entityId) != nullptr;
		case SceneComponentKind::PhysicsMaterial:
			return m_Scene.GetPhysicsMaterialComponent(entityId) != nullptr;
		case SceneComponentKind::PrefabInstance:
			return m_Scene.GetPrefabInstanceComponent(entityId) != nullptr;
		case SceneComponentKind::SceneReference:
			return m_Scene.GetSceneReferenceComponent(entityId) != nullptr;
		case SceneComponentKind::Script:
			return m_Scene.GetScriptComponent(entityId) != nullptr;
		case SceneComponentKind::Sprite2D:
			return m_Scene.GetSprite2DComponent(entityId) != nullptr;
		case SceneComponentKind::UiElement:
			return m_Scene.GetUiElementComponent(entityId) != nullptr;
		case SceneComponentKind::AudioSource:
			return m_Scene.GetAudioSourceComponent(entityId) != nullptr;
		case SceneComponentKind::NavigationAgent:
			return m_Scene.GetNavigationAgentComponent(entityId) != nullptr;
		case SceneComponentKind::NetworkIdentity:
			return m_Scene.GetNetworkIdentityComponent(entityId) != nullptr;
		default:
			return true;
		}
	}();
	if (alreadyHasComponent)
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Add {} component", SceneComponentKindName(kind)),
		.Execute = [this, entityId, kind]()
		{
			AddComponentToEntity(entityId, kind);
		},
		.Undo = [this, entityId, kind]()
		{
			if (m_Scene.ContainsEntity(entityId))
			{
				RemoveComponentFromEntity(entityId, kind);
			}
		}
	});
}

void Engine::RemoveComponentFromEntity(EntityId entityId, SceneComponentKind kind)
{
	if (BlockEditSceneMutationDuringPlay("Remove Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	bool removed = false;
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		DestroyTextureResourcesForEntity(entityId);
		for (const auto& removedSourcePath : m_RuntimeAssetRegistry.UnregisterEntity(entityId))
		{
			m_AssetHotReloadService.UnwatchLoadedAsset(removedSourcePath);
		}
		RemoveEntityFromRenderState(entityId);
		removed = m_Scene.RemoveMeshComponent(entityId);
		static_cast<void>(m_Scene.RemoveAnimatorComponent(entityId));
		static_cast<void>(m_Scene.RemoveComponent<BoundsComponent>(entityId));
		if (m_Scene.GetPrimaryRenderableEntity() == entityId)
		{
			m_Scene.SetPrimaryRenderableEntity(m_RenderState.RenderEntities.empty() ? InvalidEntityId : m_RenderState.RenderEntities.front());
		}
		break;
	case SceneComponentKind::Animator:
		removed = m_Scene.RemoveAnimatorComponent(entityId);
		break;
	case SceneComponentKind::Camera:
		if (entityId == m_GameCameraEntity)
		{
			m_GameCameraEntity = InvalidEntityId;
		}
		removed = m_Scene.RemoveCameraComponent(entityId);
		break;
	case SceneComponentKind::Light:
		if (entityId == m_KeyLightEntity)
		{
			m_KeyLightEntity = InvalidEntityId;
		}
		removed = m_Scene.RemoveLightComponent(entityId);
		break;
	case SceneComponentKind::RigidBody:
		removed = m_Scene.RemoveRigidBodyComponent(entityId);
		break;
	case SceneComponentKind::Collider:
		removed = m_Scene.RemoveColliderComponent(entityId);
		break;
	case SceneComponentKind::PhysicsMaterial:
		removed = m_Scene.RemovePhysicsMaterialComponent(entityId);
		break;
	case SceneComponentKind::PrefabInstance:
		removed = m_Scene.RemovePrefabInstanceComponent(entityId);
		break;
	case SceneComponentKind::SceneReference:
		removed = m_Scene.RemoveSceneReferenceComponent(entityId);
		break;
	case SceneComponentKind::Script:
		m_ScriptRuntime.ClearEntity(entityId);
		removed = m_Scene.RemoveScriptComponent(entityId);
		break;
	case SceneComponentKind::Sprite2D:
		removed = m_Scene.RemoveSprite2DComponent(entityId);
		break;
	case SceneComponentKind::UiElement:
		removed = m_Scene.RemoveUiElementComponent(entityId);
		break;
	case SceneComponentKind::AudioSource:
		removed = m_Scene.RemoveAudioSourceComponent(entityId);
		break;
	case SceneComponentKind::NavigationAgent:
		removed = m_Scene.RemoveNavigationAgentComponent(entityId);
		break;
	case SceneComponentKind::NetworkIdentity:
		removed = m_Scene.RemoveNetworkIdentityComponent(entityId);
		break;
	default:
		break;
	}

	if (!removed)
	{
		return;
	}

	if (kind == SceneComponentKind::RigidBody || kind == SceneComponentKind::Collider || kind == SceneComponentKind::PhysicsMaterial)
	{
		MarkPhysicsActorDirty(entityId);
	}
	if (kind == SceneComponentKind::Script)
	{
		m_ScriptRuntime.ClearEntity(entityId);
	}
	MarkSceneDirty();
	AppendAssetLog(std::format("Removed {} component from entity {}", SceneComponentKindName(kind), entityId));
}

void Engine::RestoreComponentFromSnapshot(EntityId entityId, SceneComponentKind kind, const ScenePersistence::LoadedSceneEntity& snapshot)
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	switch (kind)
	{
	case SceneComponentKind::Mesh:
		if (snapshot.HasMesh)
		{
			bool queuedModelRestore = false;
			const Math::Transform transform = snapshot.HasTransform
				? snapshot.Transform
				: (m_Scene.GetTransformComponent(entityId) ? m_Scene.GetTransformComponent(entityId)->LocalTransform : Math::Transform::Identity());
			if (snapshot.PrimitiveKind != Asset::PrimitiveMeshKind::None)
			{
				static_cast<void>(ApplyPrimitiveMeshToEntity(entityId, snapshot.PrimitiveKind, transform, false));
				if (!snapshot.MaterialOverrides.empty())
				{
					if (Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId))
					{
						ApplyMaterialOverrides(*meshAsset, snapshot.MaterialOverrides);
						static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
					}
				}
			}
			else if (!snapshot.MeshAssetPath.empty())
			{
				static_cast<void>(m_Scene.SetMeshEnabled(entityId, snapshot.MeshEnabled));
				QueueModelImportForSceneEntity(snapshot, entityId);
				queuedModelRestore = true;
			}
			else
			{
				static_cast<void>(m_Scene.EnsureMeshComponent(entityId));
			}
			if (!queuedModelRestore)
			{
				static_cast<void>(m_Scene.SetMeshEnabled(entityId, snapshot.MeshEnabled));
			}
		}
		if (snapshot.HasAnimator)
		{
			m_Scene.EnsureAnimatorComponent(entityId) = snapshot.Animator;
			static_cast<void>(m_Scene.SetAnimatorEnabled(entityId, snapshot.AnimatorEnabled));
		}
		if (snapshot.HasTransform)
		{
			ApplyEntityTransform(entityId, snapshot.Transform, {});
		}
		break;
	case SceneComponentKind::Animator:
		if (snapshot.HasAnimator)
		{
			m_Scene.EnsureAnimatorComponent(entityId) = snapshot.Animator;
			static_cast<void>(m_Scene.SetAnimatorEnabled(entityId, snapshot.AnimatorEnabled));
		}
		break;
	case SceneComponentKind::Camera:
		if (snapshot.HasCamera)
		{
			m_Scene.EnsureCameraComponent(entityId) = snapshot.Camera;
			static_cast<void>(m_Scene.SetCameraEnabled(entityId, snapshot.CameraEnabled));
			if (snapshot.Camera.IsGameCamera)
			{
				m_GameCameraEntity = entityId;
			}
		}
		break;
	case SceneComponentKind::Light:
		if (snapshot.HasLight)
		{
			m_Scene.EnsureLightComponent(entityId) = snapshot.Light;
			static_cast<void>(m_Scene.SetLightEnabled(entityId, snapshot.LightEnabled));
			if (m_KeyLightEntity == InvalidEntityId)
			{
				m_KeyLightEntity = entityId;
			}
		}
		break;
	case SceneComponentKind::RigidBody:
		if (snapshot.HasRigidBody)
		{
			m_Scene.EnsureRigidBodyComponent(entityId) = snapshot.RigidBody;
			static_cast<void>(m_Scene.SetRigidBodyEnabled(entityId, snapshot.RigidBodyEnabled));
			MarkPhysicsActorDirty(entityId);
		}
		break;
	case SceneComponentKind::Collider:
		if (snapshot.HasCollider)
		{
			m_Scene.EnsureColliderComponent(entityId) = snapshot.Collider;
			static_cast<void>(m_Scene.SetColliderEnabled(entityId, snapshot.ColliderEnabled));
			MarkPhysicsActorDirty(entityId);
		}
		break;
	case SceneComponentKind::PhysicsMaterial:
		if (snapshot.HasPhysicsMaterial)
		{
			m_Scene.EnsurePhysicsMaterialComponent(entityId) = snapshot.PhysicsMaterial;
			static_cast<void>(m_Scene.SetPhysicsMaterialEnabled(entityId, snapshot.PhysicsMaterialEnabled));
			MarkPhysicsActorDirty(entityId);
		}
		break;
	case SceneComponentKind::PrefabInstance:
		if (snapshot.HasPrefabInstance)
		{
			m_Scene.EnsurePrefabInstanceComponent(entityId) = snapshot.PrefabInstance;
			static_cast<void>(m_Scene.SetComponentEnabled<PrefabInstanceComponent>(entityId, snapshot.PrefabInstanceEnabled));
		}
		break;
	case SceneComponentKind::SceneReference:
		if (snapshot.HasSceneReference)
		{
			m_Scene.EnsureSceneReferenceComponent(entityId) = snapshot.SceneReference;
			static_cast<void>(m_Scene.SetComponentEnabled<SceneReferenceComponent>(entityId, snapshot.SceneReferenceEnabled));
		}
		break;
	case SceneComponentKind::Script:
		if (snapshot.HasScript)
		{
			m_Scene.EnsureScriptComponent(entityId) = snapshot.Script;
			static_cast<void>(m_Scene.SetComponentEnabled<ScriptComponent>(entityId, snapshot.ScriptEnabled));
		}
		break;
	case SceneComponentKind::Sprite2D:
		if (snapshot.HasSprite2D)
		{
			m_Scene.EnsureSprite2DComponent(entityId) = snapshot.Sprite2D;
			static_cast<void>(m_Scene.SetComponentEnabled<Sprite2DComponent>(entityId, snapshot.Sprite2DEnabled));
		}
		break;
	case SceneComponentKind::UiElement:
		if (snapshot.HasUiElement)
		{
			m_Scene.EnsureUiElementComponent(entityId) = snapshot.UiElement;
			static_cast<void>(m_Scene.SetComponentEnabled<UiElementComponent>(entityId, snapshot.UiElementEnabled));
		}
		break;
	case SceneComponentKind::AudioSource:
		if (snapshot.HasAudioSource)
		{
			m_Scene.EnsureAudioSourceComponent(entityId) = snapshot.AudioSource;
			static_cast<void>(m_Scene.SetComponentEnabled<AudioSourceComponent>(entityId, snapshot.AudioSourceEnabled));
		}
		break;
	case SceneComponentKind::NavigationAgent:
		if (snapshot.HasNavigationAgent)
		{
			m_Scene.EnsureNavigationAgentComponent(entityId) = snapshot.NavigationAgent;
			static_cast<void>(m_Scene.SetComponentEnabled<NavigationAgentComponent>(entityId, snapshot.NavigationAgentEnabled));
		}
		break;
	case SceneComponentKind::NetworkIdentity:
		if (snapshot.HasNetworkIdentity)
		{
			m_Scene.EnsureNetworkIdentityComponent(entityId) = snapshot.NetworkIdentity;
			static_cast<void>(m_Scene.SetComponentEnabled<NetworkIdentityComponent>(entityId, snapshot.NetworkIdentityEnabled));
		}
		break;
	default:
		break;
	}

	MarkSceneDirty();
	AppendAssetLog(std::format("Restored {} component on entity {}", SceneComponentKindName(kind), entityId));
}

void Engine::RemoveComponentFromEntityWithUndo(EntityId entityId, SceneComponentKind kind)
{
	if (BlockEditSceneMutationDuringPlay("Remove Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId) || !HasSceneComponentKind(entityId, kind))
	{
		return;
	}

	std::optional<ScenePersistence::LoadedSceneEntity> snapshot = BuildLoadedSceneEntityFromEntity(entityId);
	if (!snapshot)
	{
		RemoveComponentFromEntity(entityId, kind);
		return;
	}

	const auto componentSnapshot = std::make_shared<ScenePersistence::LoadedSceneEntity>(std::move(*snapshot));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Remove {} component", SceneComponentKindName(kind)),
		.Execute = [this, entityId, kind]()
		{
			if (m_Scene.ContainsEntity(entityId))
			{
				RemoveComponentFromEntity(entityId, kind);
			}
		},
		.Undo = [this, entityId, kind, componentSnapshot]()
		{
			RestoreComponentFromSnapshot(entityId, kind, *componentSnapshot);
		}
	});
}

void Engine::SetComponentEnabledForEntity(EntityId entityId, SceneComponentKind kind, bool enabled)
{
	if (BlockEditSceneMutationDuringPlay("Set Component Enabled"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	bool changed = false;
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		changed = m_Scene.SetMeshEnabled(entityId, enabled);
		break;
	case SceneComponentKind::Animator:
		changed = m_Scene.SetAnimatorEnabled(entityId, enabled);
		break;
	case SceneComponentKind::Camera:
		changed = m_Scene.SetCameraEnabled(entityId, enabled);
		if (!enabled && entityId == m_GameCameraEntity)
		{
			SyncGameCameraFromSceneEntity();
		}
		break;
	case SceneComponentKind::Light:
		changed = m_Scene.SetLightEnabled(entityId, enabled);
		break;
	case SceneComponentKind::RigidBody:
		changed = m_Scene.SetRigidBodyEnabled(entityId, enabled);
		break;
	case SceneComponentKind::Collider:
		changed = m_Scene.SetColliderEnabled(entityId, enabled);
		break;
	case SceneComponentKind::PhysicsMaterial:
		changed = m_Scene.SetPhysicsMaterialEnabled(entityId, enabled);
		break;
	case SceneComponentKind::PrefabInstance:
		changed = m_Scene.SetComponentEnabled<PrefabInstanceComponent>(entityId, enabled);
		break;
	case SceneComponentKind::SceneReference:
		changed = m_Scene.SetComponentEnabled<SceneReferenceComponent>(entityId, enabled);
		break;
	case SceneComponentKind::Script:
		changed = m_Scene.SetComponentEnabled<ScriptComponent>(entityId, enabled);
		if (!enabled)
		{
			m_ScriptRuntime.ClearEntity(entityId);
		}
		break;
	case SceneComponentKind::Sprite2D:
		changed = m_Scene.SetComponentEnabled<Sprite2DComponent>(entityId, enabled);
		break;
	case SceneComponentKind::UiElement:
		changed = m_Scene.SetComponentEnabled<UiElementComponent>(entityId, enabled);
		break;
	case SceneComponentKind::AudioSource:
		changed = m_Scene.SetComponentEnabled<AudioSourceComponent>(entityId, enabled);
		break;
	case SceneComponentKind::NavigationAgent:
		changed = m_Scene.SetComponentEnabled<NavigationAgentComponent>(entityId, enabled);
		break;
	case SceneComponentKind::NetworkIdentity:
		changed = m_Scene.SetComponentEnabled<NetworkIdentityComponent>(entityId, enabled);
		break;
	default:
		break;
	}

	if (!changed)
	{
		return;
	}

	if (kind == SceneComponentKind::RigidBody || kind == SceneComponentKind::Collider || kind == SceneComponentKind::PhysicsMaterial)
	{
		MarkPhysicsActorDirty(entityId);
	}
	MarkSceneDirty();
	AppendAssetLog(std::format("{} {} component on entity {}", enabled ? "Enabled" : "Disabled", SceneComponentKindName(kind), entityId));
}

bool Engine::HasSceneComponentKind(EntityId entityId, SceneComponentKind kind) const
{
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		return m_Scene.GetMeshComponent(entityId) != nullptr;
	case SceneComponentKind::Animator:
		return m_Scene.GetAnimatorComponent(entityId) != nullptr;
	case SceneComponentKind::Camera:
		return m_Scene.GetCameraComponent(entityId) != nullptr;
	case SceneComponentKind::Light:
		return m_Scene.GetLightComponent(entityId) != nullptr;
	case SceneComponentKind::RigidBody:
		return m_Scene.GetRigidBodyComponent(entityId) != nullptr;
	case SceneComponentKind::Collider:
		return m_Scene.GetColliderComponent(entityId) != nullptr;
	case SceneComponentKind::PhysicsMaterial:
		return m_Scene.GetPhysicsMaterialComponent(entityId) != nullptr;
	case SceneComponentKind::PrefabInstance:
		return m_Scene.GetPrefabInstanceComponent(entityId) != nullptr;
	case SceneComponentKind::SceneReference:
		return m_Scene.GetSceneReferenceComponent(entityId) != nullptr;
	case SceneComponentKind::Script:
		return m_Scene.GetScriptComponent(entityId) != nullptr;
	case SceneComponentKind::Sprite2D:
		return m_Scene.GetSprite2DComponent(entityId) != nullptr;
	case SceneComponentKind::UiElement:
		return m_Scene.GetUiElementComponent(entityId) != nullptr;
	case SceneComponentKind::AudioSource:
		return m_Scene.GetAudioSourceComponent(entityId) != nullptr;
	case SceneComponentKind::NavigationAgent:
		return m_Scene.GetNavigationAgentComponent(entityId) != nullptr;
	case SceneComponentKind::NetworkIdentity:
		return m_Scene.GetNetworkIdentityComponent(entityId) != nullptr;
	default:
		return false;
	}
}

bool Engine::IsSceneComponentKindEnabled(EntityId entityId, SceneComponentKind kind) const
{
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		return m_Scene.IsMeshEnabled(entityId);
	case SceneComponentKind::Animator:
		return m_Scene.IsAnimatorEnabled(entityId);
	case SceneComponentKind::Camera:
		return m_Scene.IsCameraEnabled(entityId);
	case SceneComponentKind::Light:
		return m_Scene.IsLightEnabled(entityId);
	case SceneComponentKind::RigidBody:
		return m_Scene.IsRigidBodyEnabled(entityId);
	case SceneComponentKind::Collider:
		return m_Scene.IsColliderEnabled(entityId);
	case SceneComponentKind::PhysicsMaterial:
		return m_Scene.IsPhysicsMaterialEnabled(entityId);
	case SceneComponentKind::PrefabInstance:
		return m_Scene.IsComponentEnabled<PrefabInstanceComponent>(entityId);
	case SceneComponentKind::SceneReference:
		return m_Scene.IsComponentEnabled<SceneReferenceComponent>(entityId);
	case SceneComponentKind::Script:
		return m_Scene.IsComponentEnabled<ScriptComponent>(entityId);
	case SceneComponentKind::Sprite2D:
		return m_Scene.IsComponentEnabled<Sprite2DComponent>(entityId);
	case SceneComponentKind::UiElement:
		return m_Scene.IsComponentEnabled<UiElementComponent>(entityId);
	case SceneComponentKind::AudioSource:
		return m_Scene.IsComponentEnabled<AudioSourceComponent>(entityId);
	case SceneComponentKind::NavigationAgent:
		return m_Scene.IsComponentEnabled<NavigationAgentComponent>(entityId);
	case SceneComponentKind::NetworkIdentity:
		return m_Scene.IsComponentEnabled<NetworkIdentityComponent>(entityId);
	default:
		return false;
	}
}

bool Engine::SceneComponentSnapshotHasKind(SceneComponentKind kind, const ScenePersistence::LoadedSceneEntity& snapshot) const
{
	switch (kind)
	{
	case SceneComponentKind::Mesh:
		return snapshot.HasMesh;
	case SceneComponentKind::Animator:
		return snapshot.HasAnimator;
	case SceneComponentKind::Camera:
		return snapshot.HasCamera;
	case SceneComponentKind::Light:
		return snapshot.HasLight;
	case SceneComponentKind::RigidBody:
		return snapshot.HasRigidBody;
	case SceneComponentKind::Collider:
		return snapshot.HasCollider;
	case SceneComponentKind::PhysicsMaterial:
		return snapshot.HasPhysicsMaterial;
	case SceneComponentKind::PrefabInstance:
		return snapshot.HasPrefabInstance;
	case SceneComponentKind::SceneReference:
		return snapshot.HasSceneReference;
	case SceneComponentKind::Script:
		return snapshot.HasScript;
	case SceneComponentKind::Sprite2D:
		return snapshot.HasSprite2D;
	case SceneComponentKind::UiElement:
		return snapshot.HasUiElement;
	case SceneComponentKind::AudioSource:
		return snapshot.HasAudioSource;
	case SceneComponentKind::NavigationAgent:
		return snapshot.HasNavigationAgent;
	case SceneComponentKind::NetworkIdentity:
		return snapshot.HasNetworkIdentity;
	default:
		return false;
	}
}

std::optional<ScenePersistence::LoadedSceneEntity> Engine::BuildDefaultComponentSnapshot(EntityId entityId, SceneComponentKind kind) const
{
	if (!m_Scene.ContainsEntity(entityId) || kind == SceneComponentKind::Mesh)
	{
		return std::nullopt;
	}

	ScenePersistence::LoadedSceneEntity snapshot;
	if (const std::string* name = m_Scene.GetEntityName(entityId))
	{
		snapshot.Name = *name;
	}

	const bool currentEnabled = IsSceneComponentKindEnabled(entityId, kind);
	switch (kind)
	{
	case SceneComponentKind::Animator:
		snapshot.HasAnimator = true;
		snapshot.AnimatorEnabled = currentEnabled;
		snapshot.Animator = {};
		break;
	case SceneComponentKind::Camera:
		snapshot.HasCamera = true;
		snapshot.CameraEnabled = currentEnabled;
		snapshot.Camera = {};
		if (const CameraComponent* currentCamera = m_Scene.GetCameraComponent(entityId))
		{
			snapshot.Camera.IsGameCamera = currentCamera->IsGameCamera;
		}
		break;
	case SceneComponentKind::Light:
		snapshot.HasLight = true;
		snapshot.LightEnabled = currentEnabled;
		snapshot.Light = {};
		snapshot.Light.Type = LightType::Directional;
		snapshot.Light.Color = { 1.0f, 0.95f, 0.82f };
		snapshot.Light.Intensity = 3.25f;
		snapshot.Light.Range = 450.0f;
		snapshot.Light.Enabled = true;
		break;
	case SceneComponentKind::RigidBody:
		snapshot.HasRigidBody = true;
		snapshot.RigidBodyEnabled = currentEnabled;
		snapshot.RigidBody = {};
		break;
	case SceneComponentKind::Collider:
		snapshot.HasCollider = true;
		snapshot.ColliderEnabled = currentEnabled;
		snapshot.Collider = {};
		break;
	case SceneComponentKind::PhysicsMaterial:
		snapshot.HasPhysicsMaterial = true;
		snapshot.PhysicsMaterialEnabled = currentEnabled;
		snapshot.PhysicsMaterial = {};
		break;
	case SceneComponentKind::PrefabInstance:
		snapshot.HasPrefabInstance = true;
		snapshot.PrefabInstanceEnabled = currentEnabled;
		snapshot.PrefabInstance = {};
		if (const std::string* name = m_Scene.GetEntityName(entityId))
		{
			snapshot.PrefabInstance.SourceName = *name;
		}
		break;
	case SceneComponentKind::SceneReference:
		snapshot.HasSceneReference = true;
		snapshot.SceneReferenceEnabled = currentEnabled;
		snapshot.SceneReference = {};
		break;
	case SceneComponentKind::Script:
		snapshot.HasScript = true;
		snapshot.ScriptEnabled = currentEnabled;
		snapshot.Script = {};
		break;
	case SceneComponentKind::Sprite2D:
		snapshot.HasSprite2D = true;
		snapshot.Sprite2DEnabled = currentEnabled;
		snapshot.Sprite2D = {};
		break;
	case SceneComponentKind::UiElement:
		snapshot.HasUiElement = true;
		snapshot.UiElementEnabled = currentEnabled;
		snapshot.UiElement = {};
		break;
	case SceneComponentKind::AudioSource:
		snapshot.HasAudioSource = true;
		snapshot.AudioSourceEnabled = currentEnabled;
		snapshot.AudioSource = {};
		break;
	case SceneComponentKind::NavigationAgent:
		snapshot.HasNavigationAgent = true;
		snapshot.NavigationAgentEnabled = currentEnabled;
		snapshot.NavigationAgent = {};
		break;
	case SceneComponentKind::NetworkIdentity:
		snapshot.HasNetworkIdentity = true;
		snapshot.NetworkIdentityEnabled = currentEnabled;
		snapshot.NetworkIdentity = {};
		snapshot.NetworkIdentity.NetworkId = (static_cast<uint64_t>(entityId) << 32u) | static_cast<uint64_t>(m_AssetSceneGeneration & 0xffffffffu);
		break;
	default:
		return std::nullopt;
	}

	return snapshot;
}

void Engine::ApplyComponentSnapshotToEntity(
	EntityId entityId,
	SceneComponentKind kind,
	const ScenePersistence::LoadedSceneEntity& snapshot,
	std::string_view logLabel)
{
	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	if (!SceneComponentSnapshotHasKind(kind, snapshot))
	{
		RemoveComponentFromEntity(entityId, kind);
		return;
	}

	ScenePersistence::LoadedSceneEntity adjustedSnapshot = snapshot;
	if (kind == SceneComponentKind::Camera && adjustedSnapshot.HasCamera)
	{
		const CameraComponent* currentCamera = m_Scene.GetCameraComponent(entityId);
		adjustedSnapshot.Camera.IsGameCamera = currentCamera ? currentCamera->IsGameCamera : false;
	}
	if (kind == SceneComponentKind::NetworkIdentity && adjustedSnapshot.HasNetworkIdentity)
	{
		if (const NetworkIdentityComponent* currentNetwork = m_Scene.GetNetworkIdentityComponent(entityId);
			currentNetwork && currentNetwork->NetworkId != 0)
		{
			adjustedSnapshot.NetworkIdentity.NetworkId = currentNetwork->NetworkId;
		}
		else
		{
			adjustedSnapshot.NetworkIdentity.NetworkId =
				(static_cast<uint64_t>(entityId) << 32u) | static_cast<uint64_t>(m_AssetSceneGeneration & 0xffffffffu);
		}
	}

	RestoreComponentFromSnapshot(entityId, kind, adjustedSnapshot);
	if (!logLabel.empty())
	{
		AppendAssetLog(std::format("{} {} component on entity {}", logLabel, SceneComponentKindName(kind), entityId));
	}
}

void Engine::ApplyComponentSnapshotToEntity(
	Scene& targetScene,
	EntityId entityId,
	SceneComponentKind kind,
	const ScenePersistence::LoadedSceneEntity& snapshot,
	std::string_view logLabel,
	bool markDirty)
{
	if (!targetScene.ContainsEntity(entityId) || !SceneComponentSnapshotHasKind(kind, snapshot))
	{
		return;
	}

	ScenePersistence::LoadedSceneEntity adjustedSnapshot = snapshot;
	if (kind == SceneComponentKind::Camera && adjustedSnapshot.HasCamera)
	{
		const CameraComponent* currentCamera = targetScene.GetCameraComponent(entityId);
		adjustedSnapshot.Camera.IsGameCamera = currentCamera ? currentCamera->IsGameCamera : false;
	}
	if (kind == SceneComponentKind::NetworkIdentity && adjustedSnapshot.HasNetworkIdentity)
	{
		if (const NetworkIdentityComponent* currentNetwork = targetScene.GetNetworkIdentityComponent(entityId);
			currentNetwork && currentNetwork->NetworkId != 0)
		{
			adjustedSnapshot.NetworkIdentity.NetworkId = currentNetwork->NetworkId;
		}
		else
		{
			adjustedSnapshot.NetworkIdentity.NetworkId =
				(static_cast<uint64_t>(entityId) << 32u) | static_cast<uint64_t>(m_AssetSceneGeneration & 0xffffffffu);
		}
	}

	bool applied = false;
	bool requiresPhysicsRebuild = false;
	switch (kind)
	{
	case SceneComponentKind::Animator:
		if (adjustedSnapshot.HasAnimator)
		{
			targetScene.EnsureAnimatorComponent(entityId) = adjustedSnapshot.Animator;
			static_cast<void>(targetScene.SetAnimatorEnabled(entityId, adjustedSnapshot.AnimatorEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::Camera:
		if (adjustedSnapshot.HasCamera)
		{
			targetScene.EnsureCameraComponent(entityId) = adjustedSnapshot.Camera;
			static_cast<void>(targetScene.SetCameraEnabled(entityId, adjustedSnapshot.CameraEnabled));
			if (IsGameCameraEntity(entityId))
			{
				SyncGameCameraFromSceneEntity();
			}
			applied = true;
		}
		break;
	case SceneComponentKind::Light:
		if (adjustedSnapshot.HasLight)
		{
			targetScene.EnsureLightComponent(entityId) = adjustedSnapshot.Light;
			static_cast<void>(targetScene.SetLightEnabled(entityId, adjustedSnapshot.LightEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::RigidBody:
		if (adjustedSnapshot.HasRigidBody)
		{
			targetScene.EnsureRigidBodyComponent(entityId) = adjustedSnapshot.RigidBody;
			static_cast<void>(targetScene.SetRigidBodyEnabled(entityId, adjustedSnapshot.RigidBodyEnabled));
			requiresPhysicsRebuild = true;
			applied = true;
		}
		break;
	case SceneComponentKind::Collider:
		if (adjustedSnapshot.HasCollider)
		{
			targetScene.EnsureColliderComponent(entityId) = adjustedSnapshot.Collider;
			static_cast<void>(targetScene.SetColliderEnabled(entityId, adjustedSnapshot.ColliderEnabled));
			requiresPhysicsRebuild = true;
			applied = true;
		}
		break;
	case SceneComponentKind::PhysicsMaterial:
		if (adjustedSnapshot.HasPhysicsMaterial)
		{
			targetScene.EnsurePhysicsMaterialComponent(entityId) = adjustedSnapshot.PhysicsMaterial;
			static_cast<void>(targetScene.SetPhysicsMaterialEnabled(entityId, adjustedSnapshot.PhysicsMaterialEnabled));
			requiresPhysicsRebuild = true;
			applied = true;
		}
		break;
	case SceneComponentKind::PrefabInstance:
		if (adjustedSnapshot.HasPrefabInstance)
		{
			targetScene.EnsurePrefabInstanceComponent(entityId) = adjustedSnapshot.PrefabInstance;
			static_cast<void>(targetScene.SetComponentEnabled<PrefabInstanceComponent>(entityId, adjustedSnapshot.PrefabInstanceEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::SceneReference:
		if (adjustedSnapshot.HasSceneReference)
		{
			targetScene.EnsureSceneReferenceComponent(entityId) = adjustedSnapshot.SceneReference;
			static_cast<void>(targetScene.SetComponentEnabled<SceneReferenceComponent>(entityId, adjustedSnapshot.SceneReferenceEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::Script:
		if (adjustedSnapshot.HasScript)
		{
			targetScene.EnsureScriptComponent(entityId) = adjustedSnapshot.Script;
			static_cast<void>(targetScene.SetComponentEnabled<ScriptComponent>(entityId, adjustedSnapshot.ScriptEnabled));
			m_ScriptRuntime.ClearEntity(entityId);
			applied = true;
		}
		break;
	case SceneComponentKind::Sprite2D:
		if (adjustedSnapshot.HasSprite2D)
		{
			targetScene.EnsureSprite2DComponent(entityId) = adjustedSnapshot.Sprite2D;
			static_cast<void>(targetScene.SetComponentEnabled<Sprite2DComponent>(entityId, adjustedSnapshot.Sprite2DEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::UiElement:
		if (adjustedSnapshot.HasUiElement)
		{
			targetScene.EnsureUiElementComponent(entityId) = adjustedSnapshot.UiElement;
			static_cast<void>(targetScene.SetComponentEnabled<UiElementComponent>(entityId, adjustedSnapshot.UiElementEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::AudioSource:
		if (adjustedSnapshot.HasAudioSource)
		{
			targetScene.EnsureAudioSourceComponent(entityId) = adjustedSnapshot.AudioSource;
			static_cast<void>(targetScene.SetComponentEnabled<AudioSourceComponent>(entityId, adjustedSnapshot.AudioSourceEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::NavigationAgent:
		if (adjustedSnapshot.HasNavigationAgent)
		{
			targetScene.EnsureNavigationAgentComponent(entityId) = adjustedSnapshot.NavigationAgent;
			static_cast<void>(targetScene.SetComponentEnabled<NavigationAgentComponent>(entityId, adjustedSnapshot.NavigationAgentEnabled));
			applied = true;
		}
		break;
	case SceneComponentKind::NetworkIdentity:
		if (adjustedSnapshot.HasNetworkIdentity)
		{
			targetScene.EnsureNetworkIdentityComponent(entityId) = adjustedSnapshot.NetworkIdentity;
			static_cast<void>(targetScene.SetComponentEnabled<NetworkIdentityComponent>(entityId, adjustedSnapshot.NetworkIdentityEnabled));
			applied = true;
		}
		break;
	default:
		break;
	}

	if (!applied)
	{
		return;
	}

	if (requiresPhysicsRebuild)
	{
		MarkPhysicsActorDirty(entityId);
	}
	if (markDirty)
	{
		MarkSceneDirty();
	}
	if (!logLabel.empty())
	{
		AppendAssetLog(std::format("{} {} component on entity {}", logLabel, SceneComponentKindName(kind), entityId));
	}
}

void Engine::ResetComponentForEntityWithUndo(EntityId entityId, SceneComponentKind kind)
{
	if (BlockEditSceneMutationDuringPlay("Reset Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId) || !HasSceneComponentKind(entityId, kind))
	{
		return;
	}

	std::optional<ScenePersistence::LoadedSceneEntity> beforeSnapshot = BuildLoadedSceneEntityFromEntity(entityId);
	std::optional<ScenePersistence::LoadedSceneEntity> defaultSnapshot = BuildDefaultComponentSnapshot(entityId, kind);
	if (!beforeSnapshot || !defaultSnapshot)
	{
		AppendAssetLog(std::format("Reset {} component is not available for entity {}", SceneComponentKindName(kind), entityId));
		return;
	}

	const auto before = std::make_shared<ScenePersistence::LoadedSceneEntity>(std::move(*beforeSnapshot));
	const auto after = std::make_shared<ScenePersistence::LoadedSceneEntity>(std::move(*defaultSnapshot));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Reset {} component", SceneComponentKindName(kind)),
		.Execute = [this, entityId, kind, after]()
		{
			ApplyComponentSnapshotToEntity(entityId, kind, *after, "Reset");
		},
		.Undo = [this, entityId, kind, before]()
		{
			ApplyComponentSnapshotToEntity(entityId, kind, *before, "Undo reset");
		}
	});
}

void Engine::PasteComponentValuesToEntityWithUndo(
	EntityId entityId,
	SceneComponentKind kind,
	const ScenePersistence::LoadedSceneEntity& snapshot)
{
	if (BlockEditSceneMutationDuringPlay("Paste Component"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId) || !SceneComponentSnapshotHasKind(kind, snapshot))
	{
		return;
	}

	std::optional<ScenePersistence::LoadedSceneEntity> beforeSnapshot = BuildLoadedSceneEntityFromEntity(entityId);
	if (!beforeSnapshot)
	{
		return;
	}

	const auto before = std::make_shared<ScenePersistence::LoadedSceneEntity>(std::move(*beforeSnapshot));
	const auto after = std::make_shared<ScenePersistence::LoadedSceneEntity>(snapshot);
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Paste {} component values", SceneComponentKindName(kind)),
		.Execute = [this, entityId, kind, after]()
		{
			ApplyComponentSnapshotToEntity(entityId, kind, *after, "Pasted");
		},
		.Undo = [this, entityId, kind, before]()
		{
			if (SceneComponentSnapshotHasKind(kind, *before))
			{
				ApplyComponentSnapshotToEntity(entityId, kind, *before, "Undo paste");
			}
			else
			{
				RemoveComponentFromEntity(entityId, kind);
			}
		}
	});
}

void Engine::CommitComponentBatchEdit(std::vector<Editor::ComponentEditRecord> records)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		std::erase_if(records, [this](const Editor::ComponentEditRecord& record)
			{
				return record.Entity == InvalidEntityId ||
					!m_PlayScene->ContainsEntity(record.Entity) ||
					!SceneComponentSnapshotHasKind(record.Kind, record.Before) ||
					!SceneComponentSnapshotHasKind(record.Kind, record.After);
			});
		if (records.empty())
		{
			return;
		}

		const auto componentRecords = std::make_shared<std::vector<Editor::ComponentEditRecord>>(std::move(records));
		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = std::format("Runtime edit {} components", componentRecords->size()),
			.Execute = [this, componentRecords]()
			{
				for (const Editor::ComponentEditRecord& record : *componentRecords)
				{
					ApplyComponentSnapshotToEntity(GetRuntimeScene(), record.Entity, record.Kind, record.After, {}, false);
				}
				AppendAssetLog(std::format("Applied runtime component edit for {} entities", componentRecords->size()));
			},
			.Undo = [this, componentRecords]()
			{
				for (const Editor::ComponentEditRecord& record : *componentRecords)
				{
					ApplyComponentSnapshotToEntity(GetRuntimeScene(), record.Entity, record.Kind, record.Before, {}, false);
				}
				AppendAssetLog(std::format("Undo runtime component edit for {} entities", componentRecords->size()));
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Component"))
	{
		return;
	}

	std::erase_if(records, [this](const Editor::ComponentEditRecord& record)
		{
			return record.Entity == InvalidEntityId ||
				!m_Scene.ContainsEntity(record.Entity) ||
				!SceneComponentSnapshotHasKind(record.Kind, record.Before) ||
				!SceneComponentSnapshotHasKind(record.Kind, record.After);
		});
	if (records.empty())
	{
		return;
	}

	const auto componentRecords = std::make_shared<std::vector<Editor::ComponentEditRecord>>(std::move(records));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Edit {} components", componentRecords->size()),
		.Execute = [this, componentRecords]()
		{
			for (const Editor::ComponentEditRecord& record : *componentRecords)
			{
				ApplyComponentSnapshotToEntity(record.Entity, record.Kind, record.After, {});
			}
			AppendAssetLog(std::format("Applied component edit for {} entities", componentRecords->size()));
		},
		.Undo = [this, componentRecords]()
		{
			for (const Editor::ComponentEditRecord& record : *componentRecords)
			{
				ApplyComponentSnapshotToEntity(record.Entity, record.Kind, record.Before, {});
			}
			AppendAssetLog(std::format("Undo component edit for {} entities", componentRecords->size()));
		}
	});
}

void Engine::SetComponentEnabledForEntityWithUndo(EntityId entityId, SceneComponentKind kind, bool enabled)
{
	if (BlockEditSceneMutationDuringPlay("Set Component Enabled"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId) || !HasSceneComponentKind(entityId, kind))
	{
		return;
	}

	const bool beforeEnabled = IsSceneComponentKindEnabled(entityId, kind);
	if (beforeEnabled == enabled)
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("{} {} component", enabled ? "Enable" : "Disable", SceneComponentKindName(kind)),
		.Execute = [this, entityId, kind, enabled]()
		{
			SetComponentEnabledForEntity(entityId, kind, enabled);
		},
		.Undo = [this, entityId, kind, beforeEnabled]()
		{
			SetComponentEnabledForEntity(entityId, kind, beforeEnabled);
		}
	});
}

void Engine::ApplyEntityTransform(EntityId entityId, const Math::Transform& transformValue, std::string_view logLabel)
{
	ApplyEntityTransform(m_Scene, entityId, transformValue, logLabel, true);
}

void Engine::ApplyEntityTransform(Scene& targetScene, EntityId entityId, const Math::Transform& transformValue, std::string_view logLabel, bool markDirty)
{
	TransformComponent* transform = targetScene.GetTransformComponent(entityId);
	if (!transform)
	{
		return;
	}

	transform->LocalTransform = transformValue;
	transform->LocalTransform.Rotation = Math::NormalizeQuaternionOrIdentity(transform->LocalTransform.Rotation);
	transform->UpdateWorld();
	MarkPhysicsActorDirty(entityId);
	if (markDirty)
	{
		MarkSceneDirty();
	}
	if (!logLabel.empty())
	{
		AppendAssetLog(std::format("{} entity {}", logLabel, entityId));
	}
}

void Engine::CommitTransformEdit(EntityId entityId, const Math::Transform& beforeTransform, const Math::Transform& afterTransform)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		if (!m_PlayScene->ContainsEntity(entityId))
		{
			return;
		}

		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = std::format("Runtime transform entity {}", entityId),
			.Execute = [this, entityId, afterTransform]()
			{
				ApplyEntityTransform(GetRuntimeScene(), entityId, afterTransform, "Applied runtime transform edit for", false);
			},
			.Undo = [this, entityId, beforeTransform]()
			{
				ApplyEntityTransform(GetRuntimeScene(), entityId, beforeTransform, "Undo runtime transform edit for", false);
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Transform"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Transform entity {}", entityId),
		.Execute = [this, entityId, afterTransform]()
		{
			ApplyEntityTransform(entityId, afterTransform, "Applied transform edit for");
		},
		.Undo = [this, entityId, beforeTransform]()
		{
			ApplyEntityTransform(entityId, beforeTransform, "Undo transform edit for");
		}
	});
}

void Engine::CommitTransformBatchEdit(std::vector<Editor::TransformEditRecord> records)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		std::erase_if(records, [this](const Editor::TransformEditRecord& record)
			{
				return record.Entity == InvalidEntityId || !m_PlayScene->ContainsEntity(record.Entity);
			});
		if (records.empty())
		{
			return;
		}

		const auto transformRecords = std::make_shared<std::vector<Editor::TransformEditRecord>>(std::move(records));
		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = std::format("Runtime transform {} entities", transformRecords->size()),
			.Execute = [this, transformRecords]()
			{
				for (const Editor::TransformEditRecord& record : *transformRecords)
				{
					ApplyEntityTransform(GetRuntimeScene(), record.Entity, record.After, {}, false);
				}
				AppendAssetLog(std::format("Applied runtime transform edit for {} entities", transformRecords->size()));
			},
			.Undo = [this, transformRecords]()
			{
				for (const Editor::TransformEditRecord& record : *transformRecords)
				{
					ApplyEntityTransform(GetRuntimeScene(), record.Entity, record.Before, {}, false);
				}
				AppendAssetLog(std::format("Undo runtime transform edit for {} entities", transformRecords->size()));
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Transform"))
	{
		return;
	}

	std::erase_if(records, [this](const Editor::TransformEditRecord& record)
		{
			return record.Entity == InvalidEntityId || !m_Scene.ContainsEntity(record.Entity);
		});
	if (records.empty())
	{
		return;
	}

	const auto transformRecords = std::make_shared<std::vector<Editor::TransformEditRecord>>(std::move(records));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Transform {} entities", transformRecords->size()),
		.Execute = [this, transformRecords]()
		{
			for (const Editor::TransformEditRecord& record : *transformRecords)
			{
				ApplyEntityTransform(record.Entity, record.After, {});
			}
			AppendAssetLog(std::format("Applied transform edit for {} entities", transformRecords->size()));
		},
		.Undo = [this, transformRecords]()
		{
			for (const Editor::TransformEditRecord& record : *transformRecords)
			{
				ApplyEntityTransform(record.Entity, record.Before, {});
			}
			AppendAssetLog(std::format("Undo transform edit for {} entities", transformRecords->size()));
		}
	});
}

std::filesystem::path Engine::NormalizeTexturePathForProject(const std::filesystem::path& texturePath)
{
	if (texturePath.empty())
	{
		return {};
	}

	std::error_code errorCode;
	const std::filesystem::path normalizedSource = std::filesystem::weakly_canonical(texturePath, errorCode).lexically_normal();
	const std::filesystem::path sourcePath = errorCode ? std::filesystem::absolute(texturePath).lexically_normal() : normalizedSource;
	if (!m_Project)
	{
		return sourcePath;
	}

	const std::filesystem::path rawAssetRoot = m_Project->RootPath / m_Project->AssetRoot;
	std::filesystem::path assetRoot = std::filesystem::weakly_canonical(rawAssetRoot, errorCode).lexically_normal();
	if (errorCode)
	{
		assetRoot = std::filesystem::absolute(rawAssetRoot).lexically_normal();
		errorCode.clear();
	}
	const std::filesystem::path relativePath = std::filesystem::relative(sourcePath, assetRoot, errorCode);
	if (!errorCode)
	{
		bool insideAssets = true;
		for (const auto& part : relativePath)
		{
			if (part == "..")
			{
				insideAssets = false;
				break;
			}
		}
		if (insideAssets)
		{
			return sourcePath;
		}
	}

	const std::filesystem::path importDirectory = assetRoot / "Textures" / "Imported";
	std::filesystem::create_directories(importDirectory, errorCode);
	if (errorCode)
	{
		AppendAssetLog(std::format("Texture import directory create failed: {}", errorCode.message()));
		return sourcePath;
	}

	std::filesystem::path destination = importDirectory / sourcePath.filename();
	const std::string stem = sourcePath.stem().string();
	const std::string extension = sourcePath.extension().string();
	for (uint32_t suffix = 1; std::filesystem::exists(destination, errorCode); ++suffix)
	{
		destination = importDirectory / std::format("{}_{}{}", stem, suffix, extension);
	}

	std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing, errorCode);
	if (errorCode)
	{
		AppendAssetLog(std::format("External texture copy failed: {} ({})", sourcePath.string(), errorCode.message()));
		return sourcePath;
	}

	AppendAssetLog(std::format("Imported external texture: {}", destination.string()));
	m_AssetFileSystem.RequestRefresh();
	return destination.lexically_normal();
}

bool Engine::RefreshMaterialResourcesForEntity(EntityId entityId)
{
	return RefreshMaterialResourcesForEntity(m_Scene, entityId, true, true);
}

bool Engine::RefreshMaterialResourcesForEntity(Scene& targetScene, EntityId entityId, bool markDirty, bool updateRegistry)
{
	Asset::StaticMeshAsset* meshAsset = targetScene.GetMeshAsset(entityId);
	std::vector<CpuMaterialTexture>* materialTextures = targetScene.GetMaterialTextures(entityId);
	if (!meshAsset || !materialTextures)
	{
		return false;
	}

	std::vector<bool> materialTransparency;
	if (!Rendering::MaterialTextureSystem::LoadCpuMaterialTextures(
		*meshAsset,
		*materialTextures,
		&materialTransparency,
		[this](std::string_view message)
		{
			AppendAssetLog(std::string(message));
		}))
	{
		AppendAssetLog(std::format("Material texture reload failed for entity {}", entityId));
		return false;
	}

	m_RenderState.EntityMaterialTransparency[entityId] = materialTransparency;
	if (entityId == targetScene.GetPrimaryRenderableEntity())
	{
		m_RenderState.PrimaryMaterialTransparency = materialTransparency;
	}

	if (!RecreateTextureResourcesForEntity(entityId))
	{
		AppendAssetLog(std::format("GPU material texture recreate failed for entity {}", entityId));
		return false;
	}

	if (updateRegistry && !meshAsset->SourcePath.empty())
	{
		const std::vector<std::filesystem::path> watchedTexturePaths = CollectWatchedAssetPaths(meshAsset->SourcePath, *materialTextures);
		m_RuntimeAssetRegistry.RegisterEntity(meshAsset->SourcePath, entityId, watchedTexturePaths, "Material Edited");
		m_AssetHotReloadService.WatchLoadedAsset(meshAsset->SourcePath, watchedTexturePaths);
	}

	if (markDirty)
	{
		MarkSceneDirty();
	}
	return true;
}

void Engine::SetMaterialShadingModel(EntityId entityId, size_t materialIndex, Asset::MaterialShadingModel model)
{
	if (BlockEditSceneMutationDuringPlay("Set Material Shading Model"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	meshAsset->Materials[materialIndex].ShadingModel = model;
	static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
	AppendAssetLog(std::format(
		"Material shading model changed - Entity={} Material={} Model={}",
		entityId,
		materialIndex,
		Asset::MaterialShadingModelName(model)));
}

void Engine::SetMaterialShadingModelWithUndo(EntityId entityId, size_t materialIndex, Asset::MaterialShadingModel model)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		Asset::StaticMeshAsset* meshAsset = m_PlayScene->GetMeshAsset(entityId);
		if (!meshAsset || materialIndex >= meshAsset->Materials.size())
		{
			return;
		}

		const Asset::StaticMeshMaterial beforeMaterial = meshAsset->Materials[materialIndex];
		if (beforeMaterial.ShadingModel == model)
		{
			return;
		}

		Asset::StaticMeshMaterial afterMaterial = beforeMaterial;
		afterMaterial.ShadingModel = model;
		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = "Runtime material shading model",
			.Execute = [this, entityId, materialIndex, afterMaterial]()
			{
				ApplyMaterialSnapshot(GetRuntimeScene(), entityId, materialIndex, afterMaterial, "Applied runtime material shading model", false, false);
			},
			.Undo = [this, entityId, materialIndex, beforeMaterial]()
			{
				ApplyMaterialSnapshot(GetRuntimeScene(), entityId, materialIndex, beforeMaterial, "Undo runtime material shading model", false, false);
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Set Material Shading Model"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const Asset::StaticMeshMaterial beforeMaterial = meshAsset->Materials[materialIndex];
	if (beforeMaterial.ShadingModel == model)
	{
		return;
	}

	Asset::StaticMeshMaterial afterMaterial = beforeMaterial;
	afterMaterial.ShadingModel = model;
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Change material shading model",
		.Execute = [this, entityId, materialIndex, afterMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, afterMaterial, "Applied material shading model");
		},
		.Undo = [this, entityId, materialIndex, beforeMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, beforeMaterial, "Undo material shading model");
		}
	});
}

void Engine::AssignMaterialTexture(EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot, const std::filesystem::path& texturePath)
{
	if (BlockEditSceneMutationDuringPlay("Assign Material Texture"))
	{
		return;
	}

	if (Asset::ClassifyAssetPath(texturePath) != Asset::AssetFileKind::Image)
	{
		AppendAssetLog(std::format("Texture assignment ignored: unsupported image path {}", texturePath.string()));
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const std::filesystem::path projectTexturePath = NormalizeTexturePathForProject(texturePath);
	Asset::StaticMeshMaterial& material = meshAsset->Materials[materialIndex];
	Asset::SetMaterialTexturePath(material, slot, projectTexturePath, true);
	if (slot == Asset::MaterialTextureSlot::BaseColor)
	{
		material.ImportedDiffuseTint = material.DiffuseColor;
		material.DiffuseColor = { 1.0f, 1.0f, 1.0f, material.Opacity };
		material.UseVertexColor = false;
	}
	static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
	AppendAssetLog(std::format(
		"Assigned material texture - Entity={} Material={} Slot={} Path={}",
		entityId,
		materialIndex,
		Asset::MaterialTextureSlotName(slot),
		projectTexturePath.string()));
}

void Engine::AssignMaterialTextureWithUndo(EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot, const std::filesystem::path& texturePath)
{
	if (BlockEditSceneMutationDuringPlay("Assign Material Texture"))
	{
		return;
	}

	if (Asset::ClassifyAssetPath(texturePath) != Asset::AssetFileKind::Image)
	{
		AppendAssetLog(std::format("Texture assignment ignored: unsupported image path {}", texturePath.string()));
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const Asset::StaticMeshMaterial beforeMaterial = meshAsset->Materials[materialIndex];
	const std::filesystem::path projectTexturePath = NormalizeTexturePathForProject(texturePath);
	Asset::StaticMeshMaterial afterMaterial = beforeMaterial;
	Asset::SetMaterialTexturePath(afterMaterial, slot, projectTexturePath, true);
	if (slot == Asset::MaterialTextureSlot::BaseColor)
	{
		afterMaterial.ImportedDiffuseTint = afterMaterial.DiffuseColor;
		afterMaterial.DiffuseColor = { 1.0f, 1.0f, 1.0f, afterMaterial.Opacity };
		afterMaterial.UseVertexColor = false;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Assign {} texture", Asset::MaterialTextureSlotName(slot)),
		.Execute = [this, entityId, materialIndex, afterMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, afterMaterial, "Applied material texture assignment");
		},
		.Undo = [this, entityId, materialIndex, beforeMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, beforeMaterial, "Undo material texture assignment");
		}
	});
}

void Engine::AssignMaterialTexturesWithUndo(EntityId entityId, size_t materialIndex, const std::vector<Editor::MaterialTextureAssignment>& assignments)
{
	if (BlockEditSceneMutationDuringPlay("Assign Material Texture"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const Asset::StaticMeshMaterial beforeMaterial = meshAsset->Materials[materialIndex];
	Asset::StaticMeshMaterial afterMaterial = beforeMaterial;
	size_t appliedCount = 0;
	for (const Editor::MaterialTextureAssignment& assignment : assignments)
	{
		if (assignment.Slot == Asset::MaterialTextureSlot::Count ||
			Asset::ClassifyAssetPath(assignment.Path) != Asset::AssetFileKind::Image)
		{
			continue;
		}

		const std::filesystem::path projectTexturePath = NormalizeTexturePathForProject(assignment.Path);
		Asset::SetMaterialTexturePath(afterMaterial, assignment.Slot, projectTexturePath, true);
		if (assignment.Slot == Asset::MaterialTextureSlot::BaseColor)
		{
			afterMaterial.ImportedDiffuseTint = afterMaterial.DiffuseColor;
			afterMaterial.DiffuseColor = { 1.0f, 1.0f, 1.0f, afterMaterial.Opacity };
			afterMaterial.UseVertexColor = false;
		}
		++appliedCount;
	}

	if (appliedCount == 0)
	{
		AppendAssetLog(std::format("Material texture batch assignment ignored - Entity={} Material={}", entityId, materialIndex));
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Auto remap {} material textures", appliedCount),
		.Execute = [this, entityId, materialIndex, afterMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, afterMaterial, "Applied material texture auto remap");
		},
		.Undo = [this, entityId, materialIndex, beforeMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, beforeMaterial, "Undo material texture auto remap");
		}
	});
}

void Engine::AssignMaterialTextureBatchWithUndo(EntityId entityId, const std::vector<Editor::MaterialTextureBatchAssignment>& batchAssignments)
{
	if (BlockEditSceneMutationDuringPlay("Assign Material Texture"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset)
	{
		return;
	}

	struct MaterialBatchRecord
	{
		size_t MaterialIndex = static_cast<size_t>(-1);
		Asset::StaticMeshMaterial Before;
		Asset::StaticMeshMaterial After;
		size_t AppliedSlotCount = 0;
	};

	std::vector<MaterialBatchRecord> records;
	size_t appliedSlotCount = 0;
	for (const Editor::MaterialTextureBatchAssignment& batch : batchAssignments)
	{
		if (batch.MaterialIndex >= meshAsset->Materials.size() || batch.Assignments.empty())
		{
			continue;
		}

		MaterialBatchRecord record;
		record.MaterialIndex = batch.MaterialIndex;
		record.Before = meshAsset->Materials[batch.MaterialIndex];
		record.After = record.Before;

		for (const Editor::MaterialTextureAssignment& assignment : batch.Assignments)
		{
			if (assignment.Slot == Asset::MaterialTextureSlot::Count ||
				Asset::ClassifyAssetPath(assignment.Path) != Asset::AssetFileKind::Image)
			{
				continue;
			}

			const std::filesystem::path projectTexturePath = NormalizeTexturePathForProject(assignment.Path);
			Asset::SetMaterialTexturePath(record.After, assignment.Slot, projectTexturePath, true);
			if (assignment.Slot == Asset::MaterialTextureSlot::BaseColor)
			{
				record.After.ImportedDiffuseTint = record.After.DiffuseColor;
				record.After.DiffuseColor = { 1.0f, 1.0f, 1.0f, record.After.Opacity };
				record.After.UseVertexColor = false;
			}
			++record.AppliedSlotCount;
		}

		if (record.AppliedSlotCount > 0)
		{
			appliedSlotCount += record.AppliedSlotCount;
			records.push_back(std::move(record));
		}
	}

	if (records.empty())
	{
		AppendAssetLog(std::format("Material texture mesh batch assignment ignored - Entity={}", entityId));
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Auto remap {} textures across {} materials", appliedSlotCount, records.size()),
		.Execute = [this, entityId, records]()
		{
			for (const MaterialBatchRecord& record : records)
			{
				ApplyMaterialSnapshot(entityId, record.MaterialIndex, record.After, "Applied mesh texture auto remap");
			}
		},
		.Undo = [this, entityId, records]()
		{
			for (const MaterialBatchRecord& record : records)
			{
				ApplyMaterialSnapshot(entityId, record.MaterialIndex, record.Before, "Undo mesh texture auto remap");
			}
		}
	});
}

void Engine::ClearMaterialTexture(EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot)
{
	if (BlockEditSceneMutationDuringPlay("Clear Material Texture"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	Asset::StaticMeshMaterial& material = meshAsset->Materials[materialIndex];
	Asset::SetMaterialTexturePath(material, slot, {});
	if (slot == Asset::MaterialTextureSlot::BaseColor)
	{
		material.UseVertexColor = true;
	}
	static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
	AppendAssetLog(std::format(
		"Cleared material texture - Entity={} Material={} Slot={}",
		entityId,
		materialIndex,
		Asset::MaterialTextureSlotName(slot)));
}

void Engine::ClearMaterialTextureWithUndo(EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot)
{
	if (BlockEditSceneMutationDuringPlay("Clear Material Texture"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	const Asset::StaticMeshMaterial beforeMaterial = meshAsset->Materials[materialIndex];
	Asset::StaticMeshMaterial afterMaterial = beforeMaterial;
	Asset::SetMaterialTexturePath(afterMaterial, slot, {});
	if (slot == Asset::MaterialTextureSlot::BaseColor)
	{
		afterMaterial.UseVertexColor = true;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Clear {} texture", Asset::MaterialTextureSlotName(slot)),
		.Execute = [this, entityId, materialIndex, afterMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, afterMaterial, "Applied material texture clear");
		},
		.Undo = [this, entityId, materialIndex, beforeMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, beforeMaterial, "Undo material texture clear");
		}
	});
}

void Engine::BrowseMaterialTexture(EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot)
{
	const std::optional<std::filesystem::path> selectedPath = ShowOpenTextureDialog();
	if (selectedPath)
	{
		AssignMaterialTextureWithUndo(entityId, materialIndex, slot, *selectedPath);
	}
}

void Engine::MarkMaterialEdited(EntityId entityId, size_t materialIndex)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		Asset::StaticMeshAsset* meshAsset = m_PlayScene->GetMeshAsset(entityId);
		if (!meshAsset || materialIndex >= meshAsset->Materials.size())
		{
			return;
		}

		static_cast<void>(RefreshMaterialResourcesForEntity(*m_PlayScene, entityId, false, false));
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Material"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	static_cast<void>(RefreshMaterialResourcesForEntity(entityId));
}

void Engine::ApplyMaterialSnapshot(EntityId entityId, size_t materialIndex, const Asset::StaticMeshMaterial& material, std::string_view logLabel)
{
	if (BlockEditSceneMutationDuringPlay("Apply Material Snapshot"))
	{
		return;
	}

	ApplyMaterialSnapshot(m_Scene, entityId, materialIndex, material, logLabel, true, true);
}

void Engine::ApplyMaterialSnapshot(Scene& targetScene, EntityId entityId, size_t materialIndex, const Asset::StaticMeshMaterial& material, std::string_view logLabel, bool markDirty, bool updateRegistry)
{
	Asset::StaticMeshAsset* meshAsset = targetScene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	meshAsset->Materials[materialIndex] = material;
	static_cast<void>(RefreshMaterialResourcesForEntity(targetScene, entityId, markDirty, updateRegistry));
	if (!logLabel.empty())
	{
		AppendAssetLog(std::format("{} - Entity={} Material={}", logLabel, entityId, materialIndex));
	}
}

void Engine::CommitMaterialEdit(EntityId entityId, size_t materialIndex, const Asset::StaticMeshMaterial& beforeMaterial, const Asset::StaticMeshMaterial& afterMaterial)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		Asset::StaticMeshAsset* meshAsset = m_PlayScene->GetMeshAsset(entityId);
		if (!meshAsset || materialIndex >= meshAsset->Materials.size())
		{
			return;
		}

		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = "Runtime edit material",
			.Execute = [this, entityId, materialIndex, afterMaterial]()
			{
				ApplyMaterialSnapshot(GetRuntimeScene(), entityId, materialIndex, afterMaterial, "Applied runtime material edit", false, false);
			},
			.Undo = [this, entityId, materialIndex, beforeMaterial]()
			{
				ApplyMaterialSnapshot(GetRuntimeScene(), entityId, materialIndex, beforeMaterial, "Undo runtime material edit", false, false);
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Material"))
	{
		return;
	}

	Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(entityId);
	if (!meshAsset || materialIndex >= meshAsset->Materials.size())
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = "Edit material",
		.Execute = [this, entityId, materialIndex, afterMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, afterMaterial, "Applied material edit");
		},
		.Undo = [this, entityId, materialIndex, beforeMaterial]()
		{
			ApplyMaterialSnapshot(entityId, materialIndex, beforeMaterial, "Undo material edit");
		}
	});
}

void Engine::CommitMaterialBatchEdit(std::vector<Editor::MaterialEditRecord> records)
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		std::erase_if(records, [this](const Editor::MaterialEditRecord& record)
			{
				const Asset::StaticMeshAsset* meshAsset = m_PlayScene->GetMeshAsset(record.Entity);
				return record.Entity == InvalidEntityId ||
					!meshAsset ||
					record.MaterialIndex >= meshAsset->Materials.size();
			});
		if (records.empty())
		{
			return;
		}

		const auto materialRecords = std::make_shared<std::vector<Editor::MaterialEditRecord>>(std::move(records));
		m_RuntimeCommandStack.Execute(Editor::EditorCommand{
			.Name = std::format("Runtime edit {} materials", materialRecords->size()),
			.Execute = [this, materialRecords]()
			{
				for (const Editor::MaterialEditRecord& record : *materialRecords)
				{
					ApplyMaterialSnapshot(GetRuntimeScene(), record.Entity, record.MaterialIndex, record.After, {}, false, false);
				}
				AppendAssetLog(std::format("Applied runtime material scalar edit for {} materials", materialRecords->size()));
			},
			.Undo = [this, materialRecords]()
			{
				for (const Editor::MaterialEditRecord& record : *materialRecords)
				{
					ApplyMaterialSnapshot(GetRuntimeScene(), record.Entity, record.MaterialIndex, record.Before, {}, false, false);
				}
				AppendAssetLog(std::format("Undo runtime material scalar edit for {} materials", materialRecords->size()));
			}
		});
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Edit Material"))
	{
		return;
	}

	std::erase_if(records, [this](const Editor::MaterialEditRecord& record)
		{
			const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(record.Entity);
			return record.Entity == InvalidEntityId ||
				!meshAsset ||
				record.MaterialIndex >= meshAsset->Materials.size();
		});
	if (records.empty())
	{
		return;
	}

	const auto materialRecords = std::make_shared<std::vector<Editor::MaterialEditRecord>>(std::move(records));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Edit {} materials", materialRecords->size()),
		.Execute = [this, materialRecords]()
		{
			for (const Editor::MaterialEditRecord& record : *materialRecords)
			{
				ApplyMaterialSnapshot(record.Entity, record.MaterialIndex, record.After, {});
			}
			AppendAssetLog(std::format("Applied material scalar edit for {} materials", materialRecords->size()));
		},
		.Undo = [this, materialRecords]()
		{
			for (const Editor::MaterialEditRecord& record : *materialRecords)
			{
				ApplyMaterialSnapshot(record.Entity, record.MaterialIndex, record.Before, {});
			}
			AppendAssetLog(std::format("Undo material scalar edit for {} materials", materialRecords->size()));
		}
	});
}

void Engine::RenameEntityFromHierarchy(EntityId entityId, std::string_view name)
{
	if (BlockEditSceneMutationDuringPlay("Rename Entity"))
	{
		return;
	}

	if (name.empty())
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	const std::string* currentName = m_Scene.GetEntityName(entityId);
	const std::string beforeName = currentName ? *currentName : "Entity";
	const std::string afterName(name);
	if (beforeName == afterName)
	{
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Rename {}", beforeName),
		.Execute = [this, entityId, afterName]()
		{
			if (m_Scene.RenameEntity(entityId, afterName))
			{
				MarkSceneDirty();
				AppendAssetLog(std::format("Renamed entity {} to {}", entityId, afterName));
			}
		},
		.Undo = [this, entityId, beforeName]()
		{
			if (m_Scene.RenameEntity(entityId, beforeName))
			{
				MarkSceneDirty();
				AppendAssetLog(std::format("Undo rename entity {} to {}", entityId, beforeName));
			}
		}
	});
}

EntityId Engine::DuplicateEntityFromHierarchy(EntityId entityId)
{
	if (BlockEditSceneMutationDuringPlay("Duplicate Entity"))
	{
		return InvalidEntityId;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return InvalidEntityId;
	}

	const EntityId duplicateEntityId = m_Scene.DuplicateEntity(entityId, MakeDuplicateEntityName(entityId), { 0.75f, 0.0f, 0.75f });
	if (duplicateEntityId == InvalidEntityId)
	{
		return InvalidEntityId;
	}

	if (entityId == m_GameCameraEntity)
	{
		if (CameraComponent* camera = m_Scene.GetCameraComponent(duplicateEntityId))
		{
			camera->IsGameCamera = false;
		}
	}

	if (std::ranges::find(m_RenderState.RenderEntities, entityId) != m_RenderState.RenderEntities.end())
	{
		m_RenderState.RenderEntities.push_back(duplicateEntityId);
	}
	if (m_RenderState.TransparentEntities.find(entityId) != m_RenderState.TransparentEntities.end())
	{
		m_RenderState.TransparentEntities.insert(duplicateEntityId);
	}
	if (const auto transparencyIt = m_RenderState.EntityMaterialTransparency.find(entityId);
		transparencyIt != m_RenderState.EntityMaterialTransparency.end())
	{
		m_RenderState.EntityMaterialTransparency[duplicateEntityId] = transparencyIt->second;
	}
	else if (entityId == m_Scene.GetPrimaryRenderableEntity())
	{
		m_RenderState.EntityMaterialTransparency[duplicateEntityId] = m_RenderState.PrimaryMaterialTransparency;
	}

	if (const Asset::StaticMeshAsset* meshAsset = m_Scene.GetMeshAsset(duplicateEntityId))
	{
		if (!EnsureGeometryBufferCapacity(meshAsset->Vertices.size(), meshAsset->Indices.size()))
		{
			AppendAssetLog(std::format("Geometry buffer resize failed for duplicated entity {}", duplicateEntityId));
		}
		if (!CreateTextureResourcesForEntity(duplicateEntityId))
		{
			AppendAssetLog(std::format("GPU texture upload failed for duplicated entity {}", duplicateEntityId));
		}
	}

	if (const auto sourcePath = m_RuntimeAssetRegistry.FindSourcePathForEntity(entityId))
	{
		const auto* materialTextures = m_Scene.GetMaterialTextures(duplicateEntityId);
		std::vector<std::filesystem::path> watchedTexturePaths = materialTextures
			? CollectWatchedAssetPaths(*sourcePath, *materialTextures)
			: std::vector<std::filesystem::path>{};
		m_RuntimeAssetRegistry.RegisterEntity(*sourcePath, duplicateEntityId, watchedTexturePaths, "Duplicated");
		m_AssetHotReloadService.WatchLoadedAsset(*sourcePath, watchedTexturePaths);
	}

	MarkPhysicsActorDirty(duplicateEntityId);
	m_Scene.SetSelectedEntity(duplicateEntityId);
	MarkSceneDirty();
	AppendAssetLog(std::format("Duplicated entity {} -> {}", entityId, duplicateEntityId));
	return duplicateEntityId;
}

void Engine::DuplicateEntityFromHierarchyWithUndo(EntityId entityId)
{
	if (BlockEditSceneMutationDuringPlay("Duplicate Entity"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	const auto duplicatedEntity = std::make_shared<EntityId>(InvalidEntityId);
	const auto duplicateSnapshot = std::make_shared<std::optional<ScenePersistence::LoadedSceneEntity>>();
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Duplicate entity {}", entityId),
		.Execute = [this, entityId, duplicatedEntity, duplicateSnapshot]()
		{
			if (*duplicateSnapshot)
			{
				*duplicatedEntity = CreateEntityFromLoadedSceneEntity(**duplicateSnapshot, {});
				m_Scene.SetSelectedEntity(*duplicatedEntity);
				MarkSceneDirty();
				AppendAssetLog(std::format("Reduplicated entity snapshot -> {}", *duplicatedEntity));
				return;
			}

			*duplicatedEntity = DuplicateEntityFromHierarchy(entityId);
			if (*duplicatedEntity != InvalidEntityId)
			{
				*duplicateSnapshot = BuildLoadedSceneEntityFromEntity(*duplicatedEntity);
			}
		},
		.Undo = [this, duplicatedEntity]()
		{
			if (*duplicatedEntity != InvalidEntityId && m_Scene.ContainsEntity(*duplicatedEntity))
			{
				DeleteEntityFromHierarchy(*duplicatedEntity);
				*duplicatedEntity = InvalidEntityId;
			}
		}
	});
}

void Engine::DuplicateEntitiesFromHierarchyWithUndo(std::vector<EntityId> entityIds)
{
	if (BlockEditSceneMutationDuringPlay("Duplicate Entity"))
	{
		return;
	}

	std::vector<EntityId> orderedEntityIds;
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		if (std::ranges::find(entityIds, entity.Id) != entityIds.end()
			&& std::ranges::find(orderedEntityIds, entity.Id) == orderedEntityIds.end())
		{
			orderedEntityIds.push_back(entity.Id);
		}
	}
	if (orderedEntityIds.empty())
	{
		return;
	}
	if (orderedEntityIds.size() == 1)
	{
		DuplicateEntityFromHierarchyWithUndo(orderedEntityIds.front());
		return;
	}

	struct DuplicateRecord
	{
		EntityId SourceEntity = InvalidEntityId;
		EntityId SnapshotEntity = InvalidEntityId;
		EntityId CurrentEntity = InvalidEntityId;
		std::optional<ScenePersistence::LoadedSceneEntity> Snapshot;
	};

	auto records = std::make_shared<std::vector<DuplicateRecord>>();
	records->reserve(orderedEntityIds.size());
	for (EntityId entityId : orderedEntityIds)
	{
		records->push_back(DuplicateRecord{ .SourceEntity = entityId });
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Duplicate {} entities", records->size()),
		.Execute = [this, records]()
		{
			const bool restoreFromSnapshots = std::ranges::all_of(*records, [](const DuplicateRecord& record)
				{
					return record.Snapshot.has_value();
				});
			std::unordered_map<EntityId, EntityId> sourceToDuplicate;
			std::unordered_map<EntityId, EntityId> snapshotToRestored;

			for (DuplicateRecord& record : *records)
			{
				if (restoreFromSnapshots)
				{
					record.CurrentEntity = CreateEntityFromLoadedSceneEntity(*record.Snapshot, {});
					if (record.SnapshotEntity != InvalidEntityId && record.CurrentEntity != InvalidEntityId)
					{
						snapshotToRestored[record.SnapshotEntity] = record.CurrentEntity;
					}
				}
				else if (m_Scene.ContainsEntity(record.SourceEntity))
				{
					record.CurrentEntity = DuplicateEntityFromHierarchy(record.SourceEntity);
					if (record.CurrentEntity != InvalidEntityId)
					{
						sourceToDuplicate[record.SourceEntity] = record.CurrentEntity;
					}
				}
			}

			for (DuplicateRecord& record : *records)
			{
				if (record.CurrentEntity == InvalidEntityId || !m_Scene.ContainsEntity(record.CurrentEntity))
				{
					continue;
				}

				if (restoreFromSnapshots && record.Snapshot && record.Snapshot->HasHierarchy)
				{
					EntityId parentEntity = record.Snapshot->ParentEntityId;
					if (const auto remappedParentIt = snapshotToRestored.find(parentEntity); remappedParentIt != snapshotToRestored.end())
					{
						parentEntity = remappedParentIt->second;
					}
					if (parentEntity != InvalidEntityId && m_Scene.ContainsEntity(parentEntity))
					{
						static_cast<void>(m_Scene.SetParentEntity(record.CurrentEntity, parentEntity, false));
					}
					m_Scene.EnsureHierarchyComponent(record.CurrentEntity).Expanded = record.Snapshot->HierarchyExpanded;
				}
				else if (!restoreFromSnapshots)
				{
					const EntityId sourceParent = m_Scene.GetParentEntity(record.SourceEntity);
					if (const auto remappedParentIt = sourceToDuplicate.find(sourceParent); remappedParentIt != sourceToDuplicate.end())
					{
						static_cast<void>(m_Scene.SetParentEntity(record.CurrentEntity, remappedParentIt->second, true));
					}
				}
			}

			if (!restoreFromSnapshots)
			{
				for (DuplicateRecord& record : *records)
				{
					if (record.CurrentEntity != InvalidEntityId && m_Scene.ContainsEntity(record.CurrentEntity))
					{
						record.SnapshotEntity = record.CurrentEntity;
						record.Snapshot = BuildLoadedSceneEntityFromEntity(record.CurrentEntity);
					}
				}
			}

			for (auto recordIt = records->rbegin(); recordIt != records->rend(); ++recordIt)
			{
				if (recordIt->CurrentEntity != InvalidEntityId && m_Scene.ContainsEntity(recordIt->CurrentEntity))
				{
					m_Scene.SetSelectedEntity(recordIt->CurrentEntity);
					break;
				}
			}
			m_Scene.UpdateWorldTransforms();
			MarkSceneDirty();
			AppendAssetLog(std::format("Duplicated {} selected entities", records->size()));
		},
		.Undo = [this, records]()
		{
			for (auto recordIt = records->rbegin(); recordIt != records->rend(); ++recordIt)
			{
				if (recordIt->CurrentEntity != InvalidEntityId && m_Scene.ContainsEntity(recordIt->CurrentEntity))
				{
					DeleteEntityFromHierarchy(recordIt->CurrentEntity);
					recordIt->CurrentEntity = InvalidEntityId;
				}
			}
		}
	});
}

void Engine::DeleteEntityFromHierarchy(EntityId entityId)
{
	if (BlockEditSceneMutationDuringPlay("Delete Entity"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	if (const auto loadedIt = m_LoadedSceneReferenceEntities.find(entityId); loadedIt != m_LoadedSceneReferenceEntities.end())
	{
		std::vector<EntityId> loadedEntityIds = loadedIt->second.LoadedEntities;
		m_LoadedSceneReferenceEntities.erase(loadedIt);
		for (auto childIt = loadedEntityIds.rbegin(); childIt != loadedEntityIds.rend(); ++childIt)
		{
			if (*childIt != InvalidEntityId && *childIt != entityId && m_Scene.ContainsEntity(*childIt))
			{
				DeleteEntityFromHierarchy(*childIt);
			}
		}
	}
	for (auto& loadedSceneReference : m_LoadedSceneReferenceEntities)
	{
		std::erase(loadedSceneReference.second.LoadedEntities, entityId);
	}

	const bool wasPrimaryRenderable = m_Scene.GetPrimaryRenderableEntity() == entityId;
	const bool wasGameCamera = m_GameCameraEntity == entityId;
	const bool wasSpider = m_SpiderEntity == entityId;
	const bool wasKeyLight = m_KeyLightEntity == entityId;

	DestroyTextureResourcesForEntity(entityId);
	m_PhysicsWorld.RemoveActor(entityId);
	m_ScriptRuntime.ClearEntity(entityId);
	for (const auto& removedSourcePath : m_RuntimeAssetRegistry.UnregisterEntity(entityId))
	{
		m_AssetHotReloadService.UnwatchLoadedAsset(removedSourcePath);
	}
	RemoveEntityFromRenderState(entityId);

	if (!m_Scene.DeleteEntity(entityId))
	{
		return;
	}

	if (wasPrimaryRenderable)
	{
		const EntityId fallbackPrimary = m_RenderState.RenderEntities.empty()
			? InvalidEntityId
			: m_RenderState.RenderEntities.front();
		m_Scene.SetPrimaryRenderableEntity(fallbackPrimary);
	}
	if (wasGameCamera)
	{
		m_GameCameraEntity = InvalidEntityId;
	}
	if (wasSpider)
	{
		m_SpiderEntity = InvalidEntityId;
	}
	if (wasKeyLight)
	{
		m_KeyLightEntity = InvalidEntityId;
	}

	MarkSceneDirty();
	AppendAssetLog(std::format("Deleted entity {}", entityId));
}

void Engine::DeleteEntityFromHierarchyWithUndo(EntityId entityId)
{
	if (BlockEditSceneMutationDuringPlay("Delete Entity"))
	{
		return;
	}

	if (!m_Scene.ContainsEntity(entityId))
	{
		return;
	}

	std::optional<ScenePersistence::LoadedSceneEntity> snapshot = BuildLoadedSceneEntityFromEntity(entityId);
	if (!snapshot)
	{
		DeleteEntityFromHierarchy(entityId);
		return;
	}

	const auto currentEntity = std::make_shared<EntityId>(entityId);
	const auto deletedSnapshot = std::make_shared<ScenePersistence::LoadedSceneEntity>(std::move(*snapshot));
	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Delete entity {}", entityId),
		.Execute = [this, currentEntity]()
		{
			if (*currentEntity != InvalidEntityId && m_Scene.ContainsEntity(*currentEntity))
			{
				DeleteEntityFromHierarchy(*currentEntity);
				*currentEntity = InvalidEntityId;
			}
		},
		.Undo = [this, currentEntity, deletedSnapshot]()
		{
			*currentEntity = CreateEntityFromLoadedSceneEntity(*deletedSnapshot, {});
			m_Scene.SetSelectedEntity(*currentEntity);
			MarkSceneDirty();
			AppendAssetLog(std::format("Restored deleted entity {}", *currentEntity));
		}
	});
}

void Engine::DeleteEntitiesFromHierarchyWithUndo(std::vector<EntityId> entityIds)
{
	if (BlockEditSceneMutationDuringPlay("Delete Entity"))
	{
		return;
	}

	struct DeletedRecord
	{
		EntityId OriginalEntity = InvalidEntityId;
		EntityId CurrentEntity = InvalidEntityId;
		size_t OriginalIndex = static_cast<size_t>(-1);
		ScenePersistence::LoadedSceneEntity Snapshot;
	};

	auto records = std::make_shared<std::vector<DeletedRecord>>();
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		if (std::ranges::find(entityIds, entity.Id) == entityIds.end())
		{
			continue;
		}
		if (std::ranges::any_of(*records, [entityId = entity.Id](const DeletedRecord& record)
			{
				return record.OriginalEntity == entityId;
			}))
		{
			continue;
		}

		std::optional<ScenePersistence::LoadedSceneEntity> snapshot = BuildLoadedSceneEntityFromEntity(entity.Id);
		if (snapshot)
		{
			records->push_back(DeletedRecord{
				.OriginalEntity = entity.Id,
				.CurrentEntity = entity.Id,
				.OriginalIndex = m_Scene.GetEntityIndex(entity.Id),
				.Snapshot = std::move(*snapshot)
			});
		}
	}
	if (records->empty())
	{
		return;
	}
	if (records->size() == 1)
	{
		DeleteEntityFromHierarchyWithUndo(records->front().OriginalEntity);
		return;
	}

	m_EditorCommandStack.Execute(Editor::EditorCommand{
		.Name = std::format("Delete {} entities", records->size()),
		.Execute = [this, records]()
		{
			for (auto recordIt = records->rbegin(); recordIt != records->rend(); ++recordIt)
			{
				if (recordIt->CurrentEntity != InvalidEntityId && m_Scene.ContainsEntity(recordIt->CurrentEntity))
				{
					DeleteEntityFromHierarchy(recordIt->CurrentEntity);
					recordIt->CurrentEntity = InvalidEntityId;
				}
			}
		},
		.Undo = [this, records]()
		{
			std::unordered_map<EntityId, EntityId> originalToRestored;
			for (DeletedRecord& record : *records)
			{
				record.CurrentEntity = CreateEntityFromLoadedSceneEntity(record.Snapshot, {});
				if (record.CurrentEntity != InvalidEntityId)
				{
					originalToRestored[record.OriginalEntity] = record.CurrentEntity;
				}
			}

			for (DeletedRecord& record : *records)
			{
				if (record.CurrentEntity == InvalidEntityId || !m_Scene.ContainsEntity(record.CurrentEntity))
				{
					continue;
				}

				if (record.Snapshot.HasHierarchy)
				{
					EntityId parentEntity = record.Snapshot.ParentEntityId;
					if (const auto remappedParentIt = originalToRestored.find(parentEntity); remappedParentIt != originalToRestored.end())
					{
						parentEntity = remappedParentIt->second;
					}
					if (parentEntity != InvalidEntityId && m_Scene.ContainsEntity(parentEntity))
					{
						static_cast<void>(m_Scene.SetParentEntity(record.CurrentEntity, parentEntity, false));
					}
					m_Scene.EnsureHierarchyComponent(record.CurrentEntity).Expanded = record.Snapshot.HierarchyExpanded;
				}
				static_cast<void>(m_Scene.MoveEntityToIndex(record.CurrentEntity, record.OriginalIndex));
			}

			if (!records->empty() && records->front().CurrentEntity != InvalidEntityId)
			{
				m_Scene.SetSelectedEntity(records->front().CurrentEntity);
			}
			m_Scene.UpdateWorldTransforms();
			MarkSceneDirty();
			AppendAssetLog(std::format("Restored {} deleted entities", records->size()));
		}
	});
}

void Engine::UndoEditorCommand()
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		if (m_RuntimeCommandStack.CanUndo())
		{
			m_RuntimeCommandStack.Undo();
		}
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Undo"))
	{
		return;
	}

	if (!m_EditorCommandStack.CanUndo())
	{
		return;
	}
	m_EditorCommandStack.Undo();
}

void Engine::RedoEditorCommand()
{
	if (m_PlayScene && IsRuntimePlaying())
	{
		if (m_RuntimeCommandStack.CanRedo())
		{
			m_RuntimeCommandStack.Redo();
		}
		return;
	}

	if (BlockEditSceneMutationDuringPlay("Redo"))
	{
		return;
	}

	if (!m_EditorCommandStack.CanRedo())
	{
		return;
	}
	m_EditorCommandStack.Redo();
}

std::string Engine::MakeDuplicateEntityName(EntityId entityId) const
{
	const std::string* sourceName = m_Scene.GetEntityName(entityId);
	const std::string baseName = sourceName && !sourceName->empty() ? *sourceName : "Entity";

	auto nameExists = [this](const std::string& candidate)
	{
		return std::ranges::any_of(m_Scene.GetEntities(), [this, &candidate](const SceneEntity& entity)
			{
				const std::string* entityName = m_Scene.GetEntityName(entity.Id);
				return entityName && *entityName == candidate;
			});
	};

	std::string candidate = baseName + "_Copy";
	if (!nameExists(candidate))
	{
		return candidate;
	}

	for (uint32_t copyIndex = 2; copyIndex < 10000; ++copyIndex)
	{
		candidate = std::format("{}_Copy{}", baseName, copyIndex);
		if (!nameExists(candidate))
		{
			return candidate;
		}
	}

	return std::format("{}_Copy{}", baseName, m_Scene.GetEntities().size());
}

void Engine::RemoveEntityFromRenderState(EntityId entityId)
{
	std::erase(m_RenderState.RenderEntities, entityId);
	m_RenderState.TransparentEntities.erase(entityId);
	m_RenderState.EntityMaterialTransparency.erase(entityId);
	m_MeshRestoreGenerations.erase(entityId);
	m_MeshRestoreRequests.erase(entityId);
	m_MeshRestoreConflictResults.erase(entityId);
}

void Engine::UploadEntityGeometry(EntityId entityId)
{
	const Asset::StaticMeshAsset* meshAsset = GetRuntimeScene().GetMeshAsset(entityId);
	if (!meshAsset || !m_StaticMeshRenderer.VertexBuffer || !m_StaticMeshRenderer.IndexBuffer)
	{
		return;
	}

	if (!EnsureGeometryBufferCapacity(meshAsset->Vertices.size(), meshAsset->Indices.size()))
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
	InitializeJobSystem();
	DragAcceptFiles(m_hMainWnd, TRUE);

	if (m_StartupOptions.RuntimePackageManifestPath)
	{
		Projects::RuntimePackageResult runtimeResult = Projects::ProjectBuildService::LoadRuntimePackage(*m_StartupOptions.RuntimePackageManifestPath);
		if (!runtimeResult.Success)
		{
			const std::wstring message(runtimeResult.ErrorMessage.begin(), runtimeResult.ErrorMessage.end());
			MessageBoxW(m_hMainWnd, message.c_str(), L"Runtime Package Error", MB_OK | MB_ICONERROR);
			return false;
		}

		m_Project = std::move(runtimeResult.Descriptor);
		m_SampleMode = Samples::Benchmark::SampleMode::ProjectScene;
		m_LastSampleMode = m_SampleMode;
		m_AssetFileSystem.SetRootPath(m_Project->RootPath / m_Project->AssetRoot);
		AppendAssetLog(std::format("Runtime package loaded: {}", runtimeResult.ManifestPath.string()));
	}
	else if (m_StartupOptions.ProjectFilePath)
	{
		Projects::ProjectResult projectResult = Projects::ProjectService::LoadProject(*m_StartupOptions.ProjectFilePath);
		if (!projectResult.Success)
		{
			const std::wstring message(projectResult.ErrorMessage.begin(), projectResult.ErrorMessage.end());
			MessageBoxW(m_hMainWnd, message.c_str(), L"Project Error", MB_OK | MB_ICONERROR);
			return false;
		}

		m_Project = std::move(projectResult.Descriptor);
		m_SampleMode = Samples::Benchmark::SampleMode::ProjectScene;
		m_LastSampleMode = m_SampleMode;
		m_AssetFileSystem.SetRootPath(m_Project->RootPath / m_Project->AssetRoot);
		AppendAssetLog(std::format("Project loaded: {}", m_Project->ProjectFilePath.string()));
	}
	else
	{
		m_AssetFileSystem.SetRootPath("Assets");
	}
	ConfigureResourceSystem();

	if (!glslang_initialize_process())
	{
		return false;
	}

	if (!m_PhysicsWorld.Initialize())
	{
		MessageBoxW(m_hMainWnd, L"PhysX 초기화에 실패했습니다.", L"Physics Error", MB_OK | MB_ICONERROR);
		return false;
	}

	if (m_Project)
	{
		InitializeProjectScene();
		SetSceneDirty(false);
		m_CurrentScenePath = GetDefaultScenePath();
		if (std::filesystem::is_regular_file(m_CurrentScenePath) && !OpenSceneFromPath(m_CurrentScenePath, false))
		{
			InitializeProjectScene();
			SetSceneDirty(false);
		}
		if (IsRuntimeMode())
		{
			m_PlayState = Editor::EditorPlayState::Play;
			SetPhysicsSimulationEnabled(true);
			AppendAssetLog("Runtime player mode active.");
		}
	}
	else
	{
		InitializeProjectScene();
		SetSceneDirty(false);
	}

	if (!SwitchGraphicsAPI(m_Graphics.CurrentApi))
	{
		return false;
	}

	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		const EntityId smokeEntity = CreatePrimitiveEntity(Asset::PrimitiveMeshKind::Cube);
		if (smokeEntity != InvalidEntityId)
		{
			AppendAssetLog(std::format("Smoke scene primitive ready: entity {}", smokeEntity));
			FrameEntityCamera(m_SceneCamera, smokeEntity);
			FrameEntityCamera(m_Camera, smokeEntity);
			SetSceneDirty(false);
		}
		else
		{
			AppendAssetLog("Smoke scene primitive creation failed.");
		}
	}

	return true;
}

LRESULT Engine::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_DROPFILES)
	{
		HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
		return 0;
	}

	if (msg == WM_CLOSE && !m_StartupOptions.SmokeTestFrameLimit && !ConfirmSaveDirtyScene())
	{
		return 0;
	}

	if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
	{
		return 1;
	}

	if (msg == WM_COMMAND)
	{
		switch (LOWORD(wParam))
		{
		case IDM_EXIT:
			if (ConfirmSaveDirtyScene())
			{
				DestroyWindow(hWnd);
			}
			return 0;

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
	Memory::BeginFrame();
	m_JobSystem.BeginFrame();
	m_PhaseScheduler.BeginFrame(m_JobSystem.GetFrameIndex());
	m_LastDeltaTime = deltaTime;
	RunFramePhases(deltaTime);
	GetRuntimeScene().UpdateWorldTransforms();
	UpdateSceneReferenceHotReload(deltaTime);
	UpdateAutosave(deltaTime);
}

void Engine::RunFramePhases(float deltaTime)
{
	Scene& runtimeScene = GetRuntimeScene();
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::BeginFrame, [this](Jobs::FramePhaseScheduler&)
		{
			ProcessPendingGraphicsApiSwitch();
		});

	m_PhaseScheduler.RunPhase(Jobs::FramePhase::DrainMainThreadQueues, [this](Jobs::FramePhaseScheduler&)
		{
			for (const auto& hotReloadEvent : m_AssetHotReloadService.ConsumeEvents())
			{
				QueueModelReload(hotReloadEvent.SourcePath, hotReloadEvent.ChangedPath);
			}
			DrainCompletedAssetJobs();
			for (std::string& error : m_JobSystem.ConsumeErrors())
			{
				AppendAssetLog(std::move(error));
			}
		});

	m_PhaseScheduler.RunPhase(Jobs::FramePhase::Start, [this, deltaTime, &runtimeScene](Jobs::FramePhaseScheduler&)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				return;
			}

			if (m_LastSampleMode != m_SampleMode)
			{
				if (m_PhysicsSimulationEnabled && m_SampleMode != Samples::Benchmark::SampleMode::ProjectScene)
				{
					SetPhysicsSimulationEnabled(false);
				}

				if (m_SampleMode == Samples::Benchmark::SampleMode::SpiderSample && m_SpiderEntity == InvalidEntityId)
				{
					if (LoadSpiderStaticMesh())
					{
						if (const Asset::StaticMeshAsset* meshAsset = GetMeshAsset(m_SpiderEntity))
						{
							static_cast<void>(EnsureGeometryBufferCapacity(meshAsset->Vertices.size(), meshAsset->Indices.size()));
						}
						static_cast<void>(CreateTextureResourcesForEntity(m_SpiderEntity));
					}
				}
				FramePrimaryRenderableCamera();
				FramePrimaryRenderableCamera(m_SceneCamera);
				m_LastSampleMode = m_SampleMode;
			}

			if (m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene)
			{
				m_ScriptRuntime.RunStart(
					runtimeScene,
					m_SceneCommandBuffer,
					deltaTime,
					m_JobSystem.GetFrameIndex(),
					IsRuntimePlaying());
			}
		});

	if (m_SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::FixedUpdate, [](Jobs::FramePhaseScheduler&) {});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::Update, [this, deltaTime](Jobs::FramePhaseScheduler&)
			{
				m_BenchmarkRunner.Update(deltaTime, &m_JobSystem);
			});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::LateUpdate, [](Jobs::FramePhaseScheduler&) {});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::Animation, [](Jobs::FramePhaseScheduler&) {});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::Physics, [](Jobs::FramePhaseScheduler&) {});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::RenderPrepare, [](Jobs::FramePhaseScheduler&) {});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::Commit, [this](Jobs::FramePhaseScheduler&)
			{
				m_SceneCommandBuffer.ExecuteAndClear();
			});
		m_PhaseScheduler.RunPhase(Jobs::FramePhase::EndFrame, [this](Jobs::FramePhaseScheduler&)
			{
				for (std::string& error : m_JobSystem.ConsumeErrors())
				{
					AppendAssetLog(std::move(error));
				}
			});
		m_PhaseScheduler.EndFrame();
		return;
	}

	m_PhaseScheduler.RunPhase(Jobs::FramePhase::FixedUpdate, [](Jobs::FramePhaseScheduler&) {});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::Update, [this, deltaTime, &runtimeScene](Jobs::FramePhaseScheduler& scheduler)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				return;
			}

			if (m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene)
			{
				m_ScriptRuntime.ScheduleUpdate(
					runtimeScene,
					scheduler,
					m_SceneCommandBuffer,
					deltaTime,
					m_JobSystem.GetFrameIndex(),
					IsRuntimePlaying());
			}
		});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::LateUpdate, [this, deltaTime, &runtimeScene](Jobs::FramePhaseScheduler& scheduler)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				return;
			}

			if (m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene)
			{
				m_ScriptRuntime.ScheduleLateUpdate(
					runtimeScene,
					scheduler,
					m_SceneCommandBuffer,
					deltaTime,
					m_JobSystem.GetFrameIndex(),
					IsRuntimePlaying());
			}
		});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::Animation, [this, deltaTime](Jobs::FramePhaseScheduler&)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				return;
			}

			UpdateAnimatedMesh(GetRuntimeScene(), deltaTime);
		});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::Physics, [this, deltaTime](Jobs::FramePhaseScheduler&)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				return;
			}

			if (m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene && m_PhysicsSimulationEnabled)
			{
				m_PhysicsWorld.Step(GetRuntimeScene(), deltaTime);
			}
		});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::RenderPrepare, [](Jobs::FramePhaseScheduler&) {});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::Commit, [this](Jobs::FramePhaseScheduler&)
		{
			m_SceneCommandBuffer.ExecuteAndClear();
		});
	m_PhaseScheduler.RunPhase(Jobs::FramePhase::EndFrame, [this, deltaTime, &runtimeScene](Jobs::FramePhaseScheduler& scheduler)
		{
			if (m_PlayState == Editor::EditorPlayState::Paused && !m_PlayStepRequested)
			{
				for (std::string& error : m_JobSystem.ConsumeErrors())
				{
					AppendAssetLog(std::move(error));
				}
				for (std::string& log : m_ScriptRuntime.ConsumeLogs())
				{
					AppendAssetLog(std::move(log));
				}
				return;
			}

			if (m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene)
			{
				m_ScriptRuntime.ScheduleEndFrame(
					runtimeScene,
					scheduler,
					m_SceneCommandBuffer,
					deltaTime,
					m_JobSystem.GetFrameIndex(),
					IsRuntimePlaying());
			}
			for (std::string& error : m_JobSystem.ConsumeErrors())
			{
				AppendAssetLog(std::move(error));
			}
			for (std::string& log : m_ScriptRuntime.ConsumeLogs())
			{
				AppendAssetLog(std::move(log));
			}
		});

	m_PhaseScheduler.EndFrame();
	if (m_PlayState == Editor::EditorPlayState::Paused && m_PlayStepRequested)
	{
		m_PlayStepRequested = false;
		AppendAssetLog("Play mode single-frame step completed.");
	}
}

void Engine::Render()
{
	if (!m_Graphics.Device || !m_Graphics.CommandList)
	{
		return;
	}

	// 커맨드 리스트 리셋 및 기본 설정
	ResetCameraConstantAllocator();
	ResetRenderFrameStats();
	m_RenderGraph.BeginFrame(m_JobSystem.GetFrameIndex(), m_Graphics.CurrentApi, m_RenderMode);
	const size_t resetPass = m_RenderGraph.AddPass("Command List Reset", Rendering::RenderPassKind::Setup, "CommandList");
	MeasureRenderGraphPass(m_RenderGraph, resetPass, [this]()
		{
			m_Graphics.CommandList->Reset();
		});
	m_Graphics.CommandList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
	m_Graphics.CommandList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

	// 백버퍼를 렌더타겟 상태로 전환
	IGpuResource* backBuffer = m_Graphics.Device->GetBackBufferResource();
	const size_t beginBackBufferPass = m_RenderGraph.AddPass("BackBuffer Present->RenderTarget", Rendering::RenderPassKind::Setup, "Swapchain");
	MeasureRenderGraphPass(m_RenderGraph, beginBackBufferPass, [this, backBuffer]()
		{
			m_Graphics.CommandList->ResourceBarrier(backBuffer, ResourceState::Present, ResourceState::RenderTarget);
		});

	// 렌더타겟 설정
	void* rtvHandle = m_Graphics.Device->GetCurrentBackBufferRTV();
	void* dsvHandle = m_Graphics.Device->GetDepthStencilView();

	// 화면 전체는 에디터 배경색으로만 초기화하고, 실제 월드 렌더는 Scene/Game 패널 rect 안에서만 수행합니다.
	const float clearColor[4] = { 0.025f, 0.027f, 0.032f, 1.0f };
	const size_t clearPass = m_RenderGraph.AddPass("Clear Editor BackBuffer", Rendering::RenderPassKind::Clear, "Swapchain");
	MeasureRenderGraphPass(m_RenderGraph, clearPass, [this, rtvHandle, dsvHandle, &clearColor]()
		{
			m_Graphics.CommandList->SetRenderTargets(rtvHandle, dsvHandle);
			m_Graphics.CommandList->ClearRenderTarget(rtvHandle, clearColor);
			m_Graphics.CommandList->ClearDepthStencil(dsvHandle, 1.0f, 0);
		});

	const size_t deferredLightUploadPass = m_RenderGraph.AddPass("Upload Deferred Light List", Rendering::RenderPassKind::Setup, "DeferredLightBuffer", "Only active in Deferred mode", m_RenderMode == RenderMode::Deferred);
	MeasureRenderGraphPass(m_RenderGraph, deferredLightUploadPass, [this]()
		{
			static_cast<void>(UpdateDeferredLightBuffer());
		});
	if (IsRuntimeMode())
	{
		const size_t runtimeFramePass = m_RenderGraph.AddPass("Build Runtime Player Frame", Rendering::RenderPassKind::Setup, "Runtime");
		MeasureRenderGraphPass(m_RenderGraph, runtimeFramePass, [this]()
			{
				Scene& runtimeScene = GetRuntimeScene();
				SyncGameCameraFromSceneEntity();
				const float aspect = static_cast<float>((std::max)(m_ClientWidth, 1)) / static_cast<float>((std::max)(m_ClientHeight, 1));
				if (CameraComponent* camera = runtimeScene.GetCameraComponent(m_GameCameraEntity);
					camera && runtimeScene.IsCameraEnabled(m_GameCameraEntity))
				{
					m_Camera.SetLens(camera->FovY, aspect, camera->NearZ, camera->FarZ);
				}
				else
				{
					m_Camera.SetLens(DirectX::XM_PIDIV4, aspect, 0.1f, 1000.0f);
				}
				m_ShadowFrameData = Rendering::ShadowSystem::BuildDirectionalShadowFrameData(runtimeScene, ResolveKeyLightEntity(), m_Camera, m_ShadowSettings);
			});
		Editor::ViewportPanelState runtimeViewport;
		runtimeViewport.Left = 0.0f;
		runtimeViewport.Top = 0.0f;
		runtimeViewport.Width = static_cast<float>(m_ClientWidth);
		runtimeViewport.Height = static_cast<float>(m_ClientHeight);
		runtimeViewport.IsVisible = true;
		runtimeViewport.IsHovered = true;
		runtimeViewport.IsFocused = true;
		RenderWorldViewport(runtimeViewport, m_Camera, "Runtime Game View", true);
	}
	else
	{
		const size_t editorFramePass = m_RenderGraph.AddPass("Build Editor DockSpace", Rendering::RenderPassKind::Editor, "ImGui");
		MeasureRenderGraphPass(m_RenderGraph, editorFramePass, [this]()
			{
				BeginEditorFrame();
				UpdateViewportCameraLenses();
				m_ShadowFrameData = Rendering::ShadowSystem::BuildDirectionalShadowFrameData(m_Scene, ResolveKeyLightEntity(), m_Camera, m_ShadowSettings);
			});
		RenderWorldViewport(m_EditorLayer.GetSceneViewport(), m_SceneCamera, "Scene View", false);
		RenderWorldViewport(m_EditorLayer.GetGameViewport(), m_Camera, "Game View", true);
		RenderEditorDrawData();
	}

	// 백버퍼를 Present 상태로 전환
	const size_t endBackBufferPass = m_RenderGraph.AddPass("BackBuffer RenderTarget->Present", Rendering::RenderPassKind::Present, "Swapchain");
	MeasureRenderGraphPass(m_RenderGraph, endBackBufferPass, [this, backBuffer]()
		{
			m_Graphics.CommandList->ResourceBarrier(backBuffer, ResourceState::RenderTarget, ResourceState::Present);
		});
	m_Graphics.CommandList->Close();
	const size_t executePass = m_RenderGraph.AddPass("Execute Command List", Rendering::RenderPassKind::Present, "GraphicsQueue");

	// 커맨드 리스트 실행 및 화면 출력
	MeasureRenderGraphPass(m_RenderGraph, executePass, [this]()
		{
			m_Graphics.Device->ExecuteCommandList(m_Graphics.CommandList.get());
		});
	const size_t presentPass = m_RenderGraph.AddPass("Present Swapchain", Rendering::RenderPassKind::Present, "Swapchain");
	MeasureRenderGraphPass(m_RenderGraph, presentPass, [this]()
		{
			m_Graphics.Device->Present();
		});
	m_Graphics.Device->MoveToNextFrame();
	m_LastCompletedRenderFrameStats = m_RenderFrameStats;
	m_RenderGraph.EndFrame();
	LogRendererRoadmapHealthSnapshot();
	if (m_StartupOptions.SmokeTestFrameLimit)
	{
		++m_SmokeRenderedFrameCount;
		if (!m_SmokeShutdownRequested && m_SmokeRenderedFrameCount >= *m_StartupOptions.SmokeTestFrameLimit)
		{
			m_SmokeShutdownRequested = true;
			AppendAssetLog(std::format(
				"Renderer smoke test completed after {} frame(s).",
				m_SmokeRenderedFrameCount));
			PostQuitMessage(0);
		}
	}

	UpdateWindowTitleWithFps();
	Memory::EndFrame();
}

void Engine::OnResize()
{
	ResizeRenderWindow();
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_SceneCamera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
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

		if (!IsRuntimeMode() && !CreateImGuiResources())
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

	if (!CreateDeferredLightBuffer(kInitialDeferredLightBufferCapacity))
	{
		LogEngineTrace("SwitchGraphicsAPI failed during deferred light buffer creation.");
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

	if (!RecreateDynamicTextureResources())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during dynamic texture resource recreation.");
		ShutdownGraphics();
		return false;
	}

	if (!IsRuntimeMode() && !CreateImGuiResources())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during ImGui initialization.");
		ShutdownGraphics();
		return false;
	}

	RebuildWindowTitleBase();
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_SceneCamera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_RenderStartTime = std::chrono::steady_clock::now();
	ResetFpsCounter();
	UpdateRendererMenuState();
	RequestRendererRoadmapHealthLog();
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
	RequestRendererRoadmapHealthLog();

	std::string modeLogMessage = "Render mode switched to ";
	modeLogMessage.append(RenderModeToString(m_RenderMode));
	LogEngineTrace(modeLogMessage);
}

void Engine::ProcessPendingGraphicsApiSwitch()
{
	if (!m_HasPendingGraphicsApiSwitch)
	{
		return;
	}

	const GraphicsAPI requestedApi = m_PendingGraphicsApi;
	m_HasPendingGraphicsApiSwitch = false;
	if (requestedApi == m_Graphics.CurrentApi)
	{
		return;
	}

	const GraphicsAPI previousApi = m_Graphics.CurrentApi;
	if (!SwitchGraphicsAPI(requestedApi) && !SwitchGraphicsAPI(previousApi))
	{
		LogEngineTrace("Editor toolbar failed to restore previous graphics API after switch failure.");
	}
}

void Engine::CreateEditorSceneEntities()
{
	if (m_GameCameraEntity == InvalidEntityId)
	{
		m_GameCameraEntity = CreateEntity("Camera");
		TransformComponent& transform = m_Scene.EnsureTransformComponent(m_GameCameraEntity);
		transform.LocalTransform = m_Camera.GetTransform();
		transform.LocalTransform.Scale = Math::OneVector3();
		transform.UpdateWorld();

		CameraComponent& camera = m_Scene.EnsureCameraComponent(m_GameCameraEntity);
		camera.FovY = m_Camera.GetFovY();
		camera.NearZ = m_Camera.GetNearZ();
		camera.FarZ = m_Camera.GetFarZ();
		camera.IsGameCamera = true;
	}

	if (m_KeyLightEntity == InvalidEntityId)
	{
		m_KeyLightEntity = CreateEntity("Light");
		TransformComponent& transform = m_Scene.EnsureTransformComponent(m_KeyLightEntity);
		transform.LocalTransform = Math::Transform::FromEuler(
			{ -120.0f, 220.0f, -220.0f },
			DirectX::XMConvertToRadians(40.0f),
			DirectX::XMConvertToRadians(35.0f),
			0.0f);
		transform.UpdateWorld();

		LightComponent& light = m_Scene.EnsureLightComponent(m_KeyLightEntity);
		light.Type = LightType::Directional;
		light.Color = { 1.0f, 0.95f, 0.82f };
		light.Intensity = 3.25f;
		light.Range = 450.0f;
		light.SpotAngle = DirectX::XM_PIDIV4;
		light.Enabled = true;
	}
}

void Engine::InitializeProjectScene()
{
	m_RenderState.Reset();
	m_AmbientColor = { 0.62f, 0.68f, 0.78f };
	m_AmbientIntensity = 0.35f;
	m_Exposure = 1.0f;
	m_SkyboxSettings = Rendering::SkyboxSettings{};
	m_MaterialDebugView = MaterialDebugView::Lit;
	m_Camera.LookAt(
		{ 0.0f, 2.5f, -8.0f },
		{ 0.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f });
	m_SceneCamera.LookAt(
		{ 0.0f, 3.5f, -9.0f },
		{ 0.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f });
	CreateEditorSceneEntities();
	SyncRuntimeCameraToGameCameraEntity();
	m_Scene.ResetSelection();
}

void Engine::SyncRuntimeCameraToGameCameraEntity()
{
	if (m_GameCameraEntity == InvalidEntityId)
	{
		return;
	}

	Scene& runtimeScene = GetRuntimeScene();
	TransformComponent& transform = runtimeScene.EnsureTransformComponent(m_GameCameraEntity);
	transform.LocalTransform = m_Camera.GetTransform();
	transform.LocalTransform.Scale = Math::OneVector3();
	transform.UpdateWorld();

	CameraComponent& camera = runtimeScene.EnsureCameraComponent(m_GameCameraEntity);
	camera.FovY = m_Camera.GetFovY();
	camera.NearZ = m_Camera.GetNearZ();
	camera.FarZ = m_Camera.GetFarZ();
	camera.IsGameCamera = true;
}

void Engine::SyncGameCameraFromSceneEntity()
{
	Scene& runtimeScene = GetRuntimeScene();
	if (m_GameCameraEntity == InvalidEntityId
		|| !runtimeScene.ContainsEntity(m_GameCameraEntity)
		|| !runtimeScene.IsCameraEnabled(m_GameCameraEntity)
		|| !runtimeScene.GetCameraComponent(m_GameCameraEntity))
	{
		return;
	}

	const DirectX::XMFLOAT3 previousPosition = m_Camera.GetPosition();
	const DirectX::XMFLOAT3 previousForward = m_Camera.GetForward();
	const float previousFovY = m_Camera.GetFovY();
	const float previousNearZ = m_Camera.GetNearZ();
	const float previousFarZ = m_Camera.GetFarZ();

	if (TransformComponent* transform = runtimeScene.GetTransformComponent(m_GameCameraEntity))
	{
		transform->UpdateWorld();
		m_Camera.SetTransform(transform->WorldTransform);
	}

	if (CameraComponent* camera = runtimeScene.GetCameraComponent(m_GameCameraEntity))
	{
		const float aspect = m_Camera.GetAspect();
		const float fovY = std::clamp(camera->FovY, DirectX::XMConvertToRadians(1.0f), DirectX::XMConvertToRadians(179.0f));
		const float nearZ = (std::max)(0.001f, camera->NearZ);
		const float farZ = (std::max)(nearZ + 0.001f, camera->FarZ);
		camera->FovY = fovY;
		camera->NearZ = nearZ;
		camera->FarZ = farZ;
		camera->IsGameCamera = true;
		m_Camera.SetLens(camera->FovY, aspect, camera->NearZ, camera->FarZ);
	}

	const DirectX::XMFLOAT3 currentPosition = m_Camera.GetPosition();
	const DirectX::XMFLOAT3 currentForward = m_Camera.GetForward();
	const bool cameraChanged =
		std::fabs(currentPosition.x - previousPosition.x) > 0.0001f
		|| std::fabs(currentPosition.y - previousPosition.y) > 0.0001f
		|| std::fabs(currentPosition.z - previousPosition.z) > 0.0001f
		|| std::fabs(currentForward.x - previousForward.x) > 0.0001f
		|| std::fabs(currentForward.y - previousForward.y) > 0.0001f
		|| std::fabs(currentForward.z - previousForward.z) > 0.0001f
		|| std::fabs(m_Camera.GetFovY() - previousFovY) > 0.0001f
		|| std::fabs(m_Camera.GetNearZ() - previousNearZ) > 0.0001f
		|| std::fabs(m_Camera.GetFarZ() - previousFarZ) > 0.0001f;
	if (cameraChanged)
	{
		m_BenchmarkRunner.SetSpawnView(m_Camera);
	}
}

bool Engine::IsRuntimeMode() const noexcept
{
	return m_StartupOptions.RuntimePackageManifestPath.has_value();
}

Scene& Engine::GetRuntimeScene() noexcept
{
	return m_PlayScene ? *m_PlayScene : m_Scene;
}

const Scene& Engine::GetRuntimeScene() const noexcept
{
	return m_PlayScene ? *m_PlayScene : m_Scene;
}

bool Engine::IsRuntimePlaying() const noexcept
{
	return IsRuntimeMode()
		|| m_PlayState == Editor::EditorPlayState::Play
		|| m_PlayState == Editor::EditorPlayState::Paused;
}

bool Engine::BlockEditSceneMutationDuringPlay(std::string_view action)
{
	if (!IsRuntimePlaying())
	{
		return false;
	}

	const std::string_view actionLabel = action.empty() ? std::string_view("Edit-scene mutation") : action;
	AppendAssetLog(std::format(
		"{} skipped: Play mode is editing the runtime scene clone only; edit-scene changes are locked until Stop.",
		actionLabel));
	return true;
}

bool Engine::IsGameCameraEntity(EntityId entityId) const noexcept
{
	return entityId != InvalidEntityId && entityId == m_GameCameraEntity;
}

EntityId Engine::ResolveKeyLightEntity()
{
	auto isUsableLight = [this](EntityId entityId)
	{
		const LightComponent* light = m_Scene.GetLightComponent(entityId);
		return entityId != InvalidEntityId
			&& m_Scene.ContainsEntity(entityId)
			&& light
			&& m_Scene.IsLightEnabled(entityId)
			&& light->Enabled;
	};

	if (isUsableLight(m_KeyLightEntity))
	{
		return m_KeyLightEntity;
	}

	EntityId firstEnabledLight = InvalidEntityId;
	for (const SceneEntity& entity : m_Scene.GetEntities())
	{
		const LightComponent* light = m_Scene.GetLightComponent(entity.Id);
		if (!light || !m_Scene.IsLightEnabled(entity.Id) || !light->Enabled)
		{
			continue;
		}

		if (firstEnabledLight == InvalidEntityId)
		{
			firstEnabledLight = entity.Id;
		}
		if (light->Type == LightType::Directional)
		{
			m_KeyLightEntity = entity.Id;
			return m_KeyLightEntity;
		}
	}

	m_KeyLightEntity = firstEnabledLight;
	return m_KeyLightEntity;
}

void Engine::RebuildWindowTitleBase()
{
	const std::wstring_view apiName = m_Graphics.CurrentApi == GraphicsAPI::DirectX12 ? L"DirectX12" : L"Vulkan";
	m_WindowTitleBase.clear();
	m_WindowTitleBase.reserve(160);
	m_WindowTitleBase.append(L"EnginePlatformer");
	if (m_SceneDirty)
	{
		m_WindowTitleBase.push_back(L'*');
	}
	if (m_Project)
	{
		const std::wstring projectName(m_Project->Name.begin(), m_Project->Name.end());
		m_WindowTitleBase.append(L" - ");
		m_WindowTitleBase.append(projectName);
	}
	if (!m_CurrentScenePath.empty())
	{
		const std::wstring sceneName = m_CurrentScenePath.filename().wstring();
		m_WindowTitleBase.append(L" - ");
		m_WindowTitleBase.append(sceneName);
	}
	m_WindowTitleBase.append(L" - ");
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

	m_StaticMeshRenderer.VertexBuffer.reset();
	m_StaticMeshRenderer.IndexBuffer.reset();
	m_StaticMeshRenderer.CameraBuffer.reset();
	m_StaticMeshRenderer.DeferredLightBuffer.reset();
	m_StaticMeshRenderer.DeferredLightingBuffer.reset();
	m_StaticMeshRenderer.DeferredTileRangeBuffer.reset();
	m_StaticMeshRenderer.DeferredTileLightIndexBuffer.reset();
	m_StaticMeshRenderer.CameraBufferStride = 256;
	m_StaticMeshRenderer.CameraBufferCapacity = 0;
	m_StaticMeshRenderer.CameraBufferCursor = 0;
	m_StaticMeshRenderer.DeferredLightBufferCapacity = 0;
	m_StaticMeshRenderer.DeferredLightCount = 0;
	m_StaticMeshRenderer.DeferredTileRangeCapacity = 0;
	m_StaticMeshRenderer.DeferredTileLightIndexCapacity = 0;
	m_StaticMeshRenderer.DeferredTileCountX = 0;
	m_StaticMeshRenderer.DeferredTileCountY = 0;
	m_StaticMeshRenderer.DeferredTileLightReferenceCount = 0;
	m_StaticMeshRenderer.DeferredMaxTileLightCount = 0;
	m_StaticMeshRenderer.DeferredFullTileLightCount = 0;
	m_StaticMeshRenderer.DeferredCpuLights.clear();

	m_Graphics.CommandList.reset();
	m_Graphics.Device.reset();

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

	DragAcceptFiles(m_hRenderWnd, TRUE);
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
	const std::vector<CpuMaterialTexture> fallbackMaterialTextures = { CpuMaterialTexture{} };
	const auto* materialTextures = GetMaterialTextures(m_Scene.GetPrimaryRenderableEntity());
	if (!materialTextures)
	{
		materialTextures = &fallbackMaterialTextures;
	}

	const size_t materialCount = (std::max)(static_cast<size_t>(1), materialTextures->size());
	const size_t textureCount = materialCount * MaterialSlotCount();

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
		if (!dx12Device)
		{
			return false;
		}

		m_StaticMeshRenderer.Dx12.MaterialTextures.clear();
		m_StaticMeshRenderer.Dx12.MaterialTextures.resize(textureCount);
		m_StaticMeshRenderer.Dx12.MaterialCount = materialCount;
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
			const size_t materialIndex = textureIndex / MaterialSlotCount();
			const size_t slotIndex = textureIndex % MaterialSlotCount();
			const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
			auto& dx12MaterialTexture = m_StaticMeshRenderer.Dx12.MaterialTextures[textureIndex];
			const UINT64 rowPitch = static_cast<UINT64>(materialTexture.Width) * 4;
			const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				DXGI_FORMAT_R8G8B8A8_TYPELESS,
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
			const size_t materialIndex = textureIndex / MaterialSlotCount();
			const size_t slotIndex = textureIndex % MaterialSlotCount();
			const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
			srvDesc.Format = GetDx12TextureSrvFormat(materialTexture);
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

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		return false;
	}

	// Vulkan 경로는 material 수만큼 sampled image를 만들고, 각 material texture를 staging buffer를 통해
	// 개별 VkImage로 업로드합니다. 이후 descriptor set은 CreateVulkanTriangleResources()에서 material별로 생성합니다.
	m_StaticMeshRenderer.Vulkan.MaterialTextures.clear();
	m_StaticMeshRenderer.Vulkan.MaterialTextures.resize(textureCount);
	m_StaticMeshRenderer.Vulkan.MaterialCount = materialCount;
	size_t vulkanDdsCandidateCount = 0;

	for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		const size_t materialIndex = textureIndex / MaterialSlotCount();
		const size_t slotIndex = textureIndex % MaterialSlotCount();
		const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
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
		imageCreateInfo.format = GetVulkanTextureFormat(materialTexture);
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
		imageViewCreateInfo.format = GetVulkanTextureFormat(materialTexture);
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
	m_StaticMeshRenderer.Dx12.MaterialCount = 0;
	m_StaticMeshRenderer.Dx12.EntityMaterials.clear();

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		m_StaticMeshRenderer.Vulkan.MaterialTextures.clear();
		m_StaticMeshRenderer.Vulkan.MaterialCount = 0;
		m_StaticMeshRenderer.Vulkan.EntityMaterials.clear();
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
	m_StaticMeshRenderer.Vulkan.MaterialCount = 0;

	for (auto& [entityId, entityResources] : m_StaticMeshRenderer.Vulkan.EntityMaterials)
	{
		(void)entityId;
		if (entityResources.DescriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), entityResources.DescriptorPool, nullptr);
			entityResources.DescriptorPool = VK_NULL_HANDLE;
		}

		for (auto& materialTexture : entityResources.MaterialTextures)
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
	}
	m_StaticMeshRenderer.Vulkan.EntityMaterials.clear();
}

bool Engine::CreateTextureResourcesForEntity(EntityId entityId)
{
	const auto* materialTextures = GetRuntimeScene().GetMaterialTextures(entityId);
	if (!materialTextures)
	{
		return false;
	}

	const size_t materialCount = (std::max)(static_cast<size_t>(1), materialTextures->size());
	const size_t textureCount = materialCount * MaterialSlotCount();
	DestroyTextureResourcesForEntity(entityId);

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
		if (!dx12Device)
		{
			return false;
		}

		auto& entityResources = m_StaticMeshRenderer.Dx12.EntityMaterials[entityId];
		entityResources.MaterialTextures.resize(textureCount);
		entityResources.MaterialCount = materialCount;

		ComPtr<ID3D12CommandAllocator> commandAllocator;
		ComPtr<ID3D12GraphicsCommandList> commandList;
		if (FAILED(dx12Device->GetD3DDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator))) ||
			FAILED(dx12Device->GetD3DDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
		{
			return false;
		}

		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			const size_t materialIndex = textureIndex / MaterialSlotCount();
			const size_t slotIndex = textureIndex % MaterialSlotCount();
			const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
			auto& dx12MaterialTexture = entityResources.MaterialTextures[textureIndex];
			const UINT64 rowPitch = static_cast<UINT64>(materialTexture.Width) * 4;
			const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
				DXGI_FORMAT_R8G8B8A8_TYPELESS,
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
		if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&entityResources.ShaderResourceHeap))))
		{
			return false;
		}

		const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpuHandle = entityResources.ShaderResourceHeap->GetCPUDescriptorHandleForHeapStart();
		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			const size_t materialIndex = textureIndex / MaterialSlotCount();
			const size_t slotIndex = textureIndex % MaterialSlotCount();
			const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
			srvDesc.Format = GetDx12TextureSrvFormat(materialTexture);
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			dx12Device->GetD3DDevice()->CreateShaderResourceView(
				entityResources.MaterialTextures[textureIndex].Texture.Get(),
				&srvDesc,
				cpuHandle);
			cpuHandle.ptr += descriptorSize;
		}

		return true;
	}

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		return false;
	}

	auto& entityResources = m_StaticMeshRenderer.Vulkan.EntityMaterials[entityId];
	entityResources.MaterialTextures.resize(textureCount);
	entityResources.MaterialCount = materialCount;

	for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		const size_t materialIndex = textureIndex / MaterialSlotCount();
		const size_t slotIndex = textureIndex % MaterialSlotCount();
		const auto& materialTexture = GetCpuMaterialTextureSlot(*materialTextures, materialIndex, slotIndex);
		auto& vulkanMaterialTexture = entityResources.MaterialTextures[textureIndex];

		VkImageCreateInfo imageCreateInfo = {};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.extent.width = static_cast<uint32_t>(materialTexture.Width);
		imageCreateInfo.extent.height = static_cast<uint32_t>(materialTexture.Height);
		imageCreateInfo.extent.depth = 1;
		imageCreateInfo.mipLevels = 1;
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.format = GetVulkanTextureFormat(materialTexture);
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
		imageViewCreateInfo.format = GetVulkanTextureFormat(materialTexture);
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

	return RecreateVulkanEntityDescriptorSets();
}

void Engine::DestroyTextureResourcesForEntity(EntityId entityId)
{
	m_StaticMeshRenderer.Dx12.EntityMaterials.erase(entityId);

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto entityResourcesIt = m_StaticMeshRenderer.Vulkan.EntityMaterials.find(entityId);
	if (entityResourcesIt == m_StaticMeshRenderer.Vulkan.EntityMaterials.end())
	{
		return;
	}

	if (vulkanDevice)
	{
		auto& entityResources = entityResourcesIt->second;
		if (entityResources.DescriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), entityResources.DescriptorPool, nullptr);
			entityResources.DescriptorPool = VK_NULL_HANDLE;
		}

		for (auto& materialTexture : entityResources.MaterialTextures)
		{
			if (materialTexture.Sampler != VK_NULL_HANDLE)
			{
				vkDestroySampler(vulkanDevice->GetVkDevice(), materialTexture.Sampler, nullptr);
			}
			if (materialTexture.ImageView != VK_NULL_HANDLE)
			{
				vkDestroyImageView(vulkanDevice->GetVkDevice(), materialTexture.ImageView, nullptr);
			}
			if (materialTexture.Image != VK_NULL_HANDLE)
			{
				vkDestroyImage(vulkanDevice->GetVkDevice(), materialTexture.Image, nullptr);
			}
			if (materialTexture.ImageMemory != VK_NULL_HANDLE)
			{
				vkFreeMemory(vulkanDevice->GetVkDevice(), materialTexture.ImageMemory, nullptr);
			}
		}
	}

	m_StaticMeshRenderer.Vulkan.EntityMaterials.erase(entityResourcesIt);
}

bool Engine::RecreateTextureResourcesForEntity(EntityId entityId)
{
	DestroyTextureResourcesForEntity(entityId);
	return CreateTextureResourcesForEntity(entityId);
}

bool Engine::RecreateDynamicTextureResources()
{
	bool success = true;
	for (const auto& record : m_RuntimeAssetRegistry.GetRecords())
	{
		for (EntityId entityId : record.Entities)
		{
			if (m_Scene.GetMeshComponent(entityId))
			{
				success = CreateTextureResourcesForEntity(entityId) && success;
			}
		}
	}
	return success;
}

bool Engine::RecreateVulkanEntityDescriptorSets()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto vulkanCameraBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.CameraBuffer.get());
	auto vulkanLightBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightBuffer.get());
	if (!vulkanDevice || !vulkanCameraBuffer || m_StaticMeshRenderer.Vulkan.DescriptorSetLayout == VK_NULL_HANDLE)
	{
		return true;
	}

	const VkDescriptorBufferInfo cameraBufferInfo = {
		.buffer = vulkanCameraBuffer->GetVkBuffer(),
		.offset = 0,
		.range = sizeof(CameraConstants)
	};
	const VkDescriptorBufferInfo lightBufferInfo = {
		.buffer = vulkanLightBuffer ? vulkanLightBuffer->GetVkBuffer() : VK_NULL_HANDLE,
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredLightBuffer ? m_StaticMeshRenderer.DeferredLightBuffer->GetSize() : sizeof(LightGpuData)
	};

	for (auto& [entityId, entityResources] : m_StaticMeshRenderer.Vulkan.EntityMaterials)
	{
		(void)entityId;
		if (entityResources.DescriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), entityResources.DescriptorPool, nullptr);
			entityResources.DescriptorPool = VK_NULL_HANDLE;
		}
		entityResources.DescriptorSets.clear();
		if (entityResources.MaterialTextures.empty())
		{
			continue;
		}

		const uint32_t materialTextureCount = static_cast<uint32_t>(
			entityResources.MaterialCount > 0
				? entityResources.MaterialCount
				: MaterialCountFromFlattenedTextureCount(entityResources.MaterialTextures.size()));
		const VkDescriptorPoolSize descriptorPoolSize = {
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.descriptorCount = materialTextureCount
		};
		const VkDescriptorPoolSize textureDescriptorPoolSize = {
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = materialTextureCount * static_cast<uint32_t>(MaterialSlotCount())
		};
		const VkDescriptorPoolSize lightDescriptorPoolSize = {
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = materialTextureCount
		};
		const VkDescriptorPoolSize descriptorPoolSizes[] = { descriptorPoolSize, textureDescriptorPoolSize, lightDescriptorPoolSize };
		const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = materialTextureCount,
			.poolSizeCount = static_cast<uint32_t>(std::size(descriptorPoolSizes)),
			.pPoolSizes = descriptorPoolSizes
		};

		if (vkCreateDescriptorPool(vulkanDevice->GetVkDevice(), &descriptorPoolCreateInfo, nullptr, &entityResources.DescriptorPool) != VK_SUCCESS)
		{
			return false;
		}

		std::vector<VkDescriptorSetLayout> descriptorSetLayouts(materialTextureCount, m_StaticMeshRenderer.Vulkan.DescriptorSetLayout);
		const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = entityResources.DescriptorPool,
			.descriptorSetCount = materialTextureCount,
			.pSetLayouts = descriptorSetLayouts.data()
		};
		entityResources.DescriptorSets.resize(materialTextureCount);

		if (vkAllocateDescriptorSets(vulkanDevice->GetVkDevice(), &descriptorSetAllocateInfo, entityResources.DescriptorSets.data()) != VK_SUCCESS)
		{
			return false;
		}

		for (uint32_t materialIndex = 0; materialIndex < materialTextureCount; ++materialIndex)
		{
			std::array<VkDescriptorImageInfo, Asset::kMaterialTextureSlotCount> textureImageInfos = {};
			for (size_t slotIndex = 0; slotIndex < MaterialSlotCount(); ++slotIndex)
			{
				const size_t flattenedIndex = FlattenMaterialTextureIndex(materialIndex, slotIndex);
				const auto& materialTexture = entityResources.MaterialTextures[(std::min)(flattenedIndex, entityResources.MaterialTextures.size() - 1)];
				textureImageInfos[slotIndex] = {
					.sampler = materialTexture.Sampler,
					.imageView = materialTexture.ImageView,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				};
			}

			const VkWriteDescriptorSet writeDescriptorSet = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = entityResources.DescriptorSets[materialIndex],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo = &cameraBufferInfo
			};
			std::array<VkWriteDescriptorSet, Asset::kMaterialTextureSlotCount + 2> writeDescriptorSets = {};
			writeDescriptorSets[0] = writeDescriptorSet;
			for (size_t slotIndex = 0; slotIndex < MaterialSlotCount(); ++slotIndex)
			{
				writeDescriptorSets[slotIndex + 1] = {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = entityResources.DescriptorSets[materialIndex],
					.dstBinding = static_cast<uint32_t>(1 + slotIndex),
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &textureImageInfos[slotIndex]
				};
			}
			writeDescriptorSets[MaterialSlotCount() + 1] = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = entityResources.DescriptorSets[materialIndex],
				.dstBinding = static_cast<uint32_t>(MaterialSlotCount() + 1),
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &lightBufferInfo
			};

			vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
		}
	}

	return true;
}

bool Engine::RefreshVulkanDeferredLightBufferDescriptors()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto vulkanLightBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightBuffer.get());
	auto vulkanTileRangeBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredTileRangeBuffer.get());
	auto vulkanTileIndexBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredTileLightIndexBuffer.get());
	if (!vulkanDevice || !vulkanLightBuffer || m_StaticMeshRenderer.Vulkan.DescriptorSetLayout == VK_NULL_HANDLE)
	{
		return true;
	}

	const VkDescriptorBufferInfo lightBufferInfo = {
		.buffer = vulkanLightBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredLightBuffer->GetSize()
	};
	const VkDescriptorBufferInfo tileRangeBufferInfo = {
		.buffer = vulkanTileRangeBuffer ? vulkanTileRangeBuffer->GetVkBuffer() : vulkanLightBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredTileRangeBuffer ? m_StaticMeshRenderer.DeferredTileRangeBuffer->GetSize() : m_StaticMeshRenderer.DeferredLightBuffer->GetSize()
	};
	const VkDescriptorBufferInfo tileIndexBufferInfo = {
		.buffer = vulkanTileIndexBuffer ? vulkanTileIndexBuffer->GetVkBuffer() : vulkanLightBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredTileLightIndexBuffer ? m_StaticMeshRenderer.DeferredTileLightIndexBuffer->GetSize() : m_StaticMeshRenderer.DeferredLightBuffer->GetSize()
	};
	const uint32_t lightBufferBinding = static_cast<uint32_t>(MaterialSlotCount() + 1);

	auto writeLightBuffer = [&](VkDescriptorSet descriptorSet)
	{
		if (descriptorSet == VK_NULL_HANDLE)
		{
			return;
		}

		const VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptorSet,
			.dstBinding = lightBufferBinding,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &lightBufferInfo
		};
		vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), 1, &writeDescriptorSet, 0, nullptr);
	};

	for (VkDescriptorSet descriptorSet : m_StaticMeshRenderer.Vulkan.DescriptorSets)
	{
		writeLightBuffer(descriptorSet);
	}

	for (const auto& [entityId, entityResources] : m_StaticMeshRenderer.Vulkan.EntityMaterials)
	{
		(void)entityId;
		for (VkDescriptorSet descriptorSet : entityResources.DescriptorSets)
		{
			writeLightBuffer(descriptorSet);
		}
	}

	if (m_StaticMeshRenderer.Vulkan.Deferred.LightingDescriptorSet != VK_NULL_HANDLE)
	{
		const VkWriteDescriptorSet writeDescriptorSets[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_StaticMeshRenderer.Vulkan.Deferred.LightingDescriptorSet,
				.dstBinding = 10,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &lightBufferInfo
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_StaticMeshRenderer.Vulkan.Deferred.LightingDescriptorSet,
				.dstBinding = 11,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &tileRangeBufferInfo
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_StaticMeshRenderer.Vulkan.Deferred.LightingDescriptorSet,
				.dstBinding = 12,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &tileIndexBufferInfo
			}
		};
		vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), static_cast<uint32_t>(std::size(writeDescriptorSets)), writeDescriptorSets, 0, nullptr);
	}

	return true;
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
		auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
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
		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
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

		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
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

void Engine::BeginEditorFrame()
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
	UpdateEditorCameraFromInput(m_LastDeltaTime);

	const EntityId keyLightEntity = ResolveKeyLightEntity();
	float keyLightIntensity = 3.25f;
	Scene& runtimeScene = GetRuntimeScene();
	if (const LightComponent* keyLight = runtimeScene.GetLightComponent(keyLightEntity))
	{
		keyLightIntensity = keyLight->Intensity;
	}
	m_PostProcessSettings.Exposure = m_Exposure;
	const bool hdrTargetAvailable =
		(m_Graphics.CurrentApi == GraphicsAPI::DirectX12 && m_StaticMeshRenderer.Dx12.Deferred.HdrColorTexture != nullptr) ||
		(m_Graphics.CurrentApi == GraphicsAPI::Vulkan && m_StaticMeshRenderer.Vulkan.Deferred.HdrColorImage != VK_NULL_HANDLE);
	const uint32_t sceneLightCount = RenderSystem::CountSceneRenderableLights(runtimeScene);
	const bool usesFallbackLight = sceneLightCount == 0;
	const uint32_t effectiveForwardLightCount = usesFallbackLight
		? 1u
		: (std::min)(sceneLightCount, kMaxForwardGpuLights);
	const uint32_t truncatedForwardLightCount = sceneLightCount > kMaxForwardGpuLights
		? sceneLightCount - kMaxForwardGpuLights
		: 0u;
	const Rendering::RenderFrameStats& lastRenderStats = m_LastCompletedRenderFrameStats;

	const bool canControlProjectPlayMode = m_Project.has_value()
		&& m_SampleMode == Samples::Benchmark::SampleMode::ProjectScene
		&& !IsRuntimeMode();
	Editor::EditorContext editorContext{
		.CurrentApi = m_Graphics.CurrentApi,
		.CurrentRenderMode = m_RenderMode,
		.SceneCamera = m_SceneCamera,
		.GameCamera = m_Camera,
		.ActiveScene = runtimeScene,
		.SampleMode = m_SampleMode,
		.BenchmarkRunner = m_BenchmarkRunner,
		.ShowDemoWindow = m_ShowImGuiDemoWindow,
		.ViewportWidth = m_ClientWidth,
		.ViewportHeight = m_ClientHeight,
		.ProjectName = m_Project ? m_Project->Name : "Development",
		.ProjectRootPath = m_Project ? m_Project->RootPath : std::filesystem::current_path(),
		.CurrentScenePath = m_CurrentScenePath,
		.ProjectSnapshot = m_AssetFileSystem.GetSnapshot(),
		.AssetLogLines = &m_AssetLogLines,
		.MemoryStats = Memory::GetStats(),
		.JobStats = m_JobSystem.GetStats(),
		.ScriptStats = m_ScriptRuntime.GetStats(),
		.ResourceStats = m_ResourceManager.GetStats(),
		.MaterialStats = BuildSceneMaterialResourceStats(),
		.ShaderVariantStats = m_ShaderVariantCache.GetStats(),
		.RenderGraphStats = m_RenderGraph.GetStats(),
		.RenderGraphPasses = &m_RenderGraph.GetPasses(),
		.RenderFrameStats = m_LastCompletedRenderFrameStats,
		.ShadowSettings = m_ShadowSettings,
		.ShadowStats = Rendering::ShadowSystem::BuildStats(m_ShadowFrameData, m_ShadowSettings),
		.PostProcessStats = Rendering::PostProcessSystem::BuildStats(m_PostProcessSettings, m_Graphics.CurrentApi, m_RenderMode, hdrTargetAvailable),
		.ForwardLightLimit = kMaxForwardGpuLights,
		.SceneLightCount = sceneLightCount,
		.ForwardLightUsedCount = effectiveForwardLightCount,
		.ForwardLightTruncatedCount = truncatedForwardLightCount,
		.UsesFallbackLight = usesFallbackLight,
		.DeferredLightCount = m_StaticMeshRenderer.DeferredLightCount,
		.DeferredLightBufferCapacity = m_StaticMeshRenderer.DeferredLightBufferCapacity,
		.DeferredTileCountX = m_StaticMeshRenderer.DeferredTileCountX,
		.DeferredTileCountY = m_StaticMeshRenderer.DeferredTileCountY,
		.DeferredTileViewportCount = lastRenderStats.DeferredTileViewportCount,
		.DeferredTileCountTotal = lastRenderStats.DeferredTileCountTotal,
		.DeferredTileLightReferenceCount = lastRenderStats.DeferredTileLightReferenceCount,
		.DeferredMaxTileLightCount = lastRenderStats.DeferredMaxTileLightCount,
		.DeferredFullTileLightCount = lastRenderStats.DeferredFullTileLightCount,
		.ProjectRefreshInProgress = m_AssetFileSystem.IsRefreshInProgress(),
		.IsSceneDirty = m_SceneDirty,
		.CanEditProjectScene = canControlProjectPlayMode && !IsRuntimePlaying(),
		.CanControlPlayMode = canControlProjectPlayMode,
		.ActiveSceneIsRuntimeClone = m_PlayScene.has_value(),
		.PhysicsSimulationEnabled = m_PhysicsSimulationEnabled,
		.AutosaveEnabled = m_AutosaveEnabled,
		.AutosaveLastSucceeded = m_LastAutosaveSucceeded,
		.AutosaveIntervalSeconds = m_AutosaveIntervalSeconds,
		.AutosaveElapsedSeconds = m_AutosaveElapsedSeconds,
		.AutosavePath = m_LastAutosavePath,
		.AutosaveStatusMessage = m_AutosaveStatusMessage,
		.PlayState = m_PlayState,
		.CanUndo = m_PlayScene && IsRuntimePlaying() ? m_RuntimeCommandStack.CanUndo() : m_EditorCommandStack.CanUndo(),
		.CanRedo = m_PlayScene && IsRuntimePlaying() ? m_RuntimeCommandStack.CanRedo() : m_EditorCommandStack.CanRedo(),
		.UndoLabel = m_PlayScene && IsRuntimePlaying() ? m_RuntimeCommandStack.GetUndoLabel() : m_EditorCommandStack.GetUndoLabel(),
		.RedoLabel = m_PlayScene && IsRuntimePlaying() ? m_RuntimeCommandStack.GetRedoLabel() : m_EditorCommandStack.GetRedoLabel(),
		.AmbientColor = m_AmbientColor,
		.AmbientIntensity = m_AmbientIntensity,
		.Exposure = m_Exposure,
		.Skybox = m_SkyboxSettings,
		.KeyLightIntensity = keyLightIntensity,
		.DebugView = m_MaterialDebugView,
		.ViewFrustumCullingEnabled = m_ViewFrustumCullingEnabled,
		.OnGraphicsApiChanged = [this](GraphicsAPI requestedApi)
		{
			if (requestedApi == m_Graphics.CurrentApi && !m_HasPendingGraphicsApiSwitch)
			{
				return;
			}

			m_PendingGraphicsApi = requestedApi;
			m_HasPendingGraphicsApiSwitch = true;
		},
		.OnRenderModeChanged = [this](RenderMode renderMode)
		{
			SwitchRenderMode(renderMode);
		},
		.OnSaveScene = [this]()
		{
			static_cast<void>(SaveCurrentScene());
		},
		.OnSaveSceneAs = [this]()
		{
			static_cast<void>(SaveCurrentSceneAs());
		},
		.OnOpenSceneDialog = [this]()
		{
			static_cast<void>(OpenSceneFromDialog());
		},
		.OnOpenScene = [this](const std::filesystem::path& path)
		{
			static_cast<void>(OpenSceneFromPath(ResolveProjectScenePath(path), true));
		},
		.OnSaveSelectedPrefab = [this]()
		{
			static_cast<void>(SaveSelectedEntityAsPrefab());
		},
		.OnExportProject = [this]()
		{
			static_cast<void>(ExportProjectPackage());
		},
		.OnExportProjectProfile = [this](const Editor::ExportProfileSettings& profile) -> bool
		{
			return ExportProjectPackage(profile);
		},
		.OnRevealProject = [this]()
		{
			if (m_Project)
			{
				RevealAssetPath(m_Project->RootPath);
			}
		},
		.OnExit = [this]()
		{
			SendMessageW(m_hMainWnd, WM_CLOSE, 0, 0);
		},
		.OnUndo = [this]()
		{
			UndoEditorCommand();
		},
		.OnRedo = [this]()
		{
			RedoEditorCommand();
		},
		.OnPlayModeChanged = [this](bool enabled)
		{
			SetPlayModeEnabled(enabled);
		},
		.OnPlayPausedChanged = [this](bool paused)
		{
			SetPlayPaused(paused);
		},
		.OnPlayStep = [this]()
		{
			StepPlayMode();
		},
		.OnResetPlayRuntimeScene = [this]()
		{
			ResetPlayRuntimeScene();
		},
		.OnAutosaveEnabledChanged = [this](bool enabled)
		{
			SetAutosaveEnabled(enabled);
		},
		.OnAutosaveIntervalChanged = [this](float intervalSeconds)
		{
			SetAutosaveInterval(intervalSeconds);
		},
		.OnFrameSelected = [this]()
		{
			FrameSelectedEntityCamera(m_SceneCamera);
		},
		.OnAlignGameCameraToScene = [this]()
		{
			AlignGameCameraToSceneCamera();
		},
		.OnAlignSceneCameraToGame = [this]()
		{
			AlignSceneCameraToGameCamera();
		},
		.OnScenePick = [this](float mouseX, float mouseY, float viewportWidth, float viewportHeight)
		{
			m_Scene.SetSelectedEntity(TryPickEntity(mouseX, mouseY, m_SceneCamera, viewportWidth, viewportHeight));
		},
		.OnAssetOpen = [this](const std::filesystem::path& path)
		{
			OpenAssetPath(path);
		},
		.OnAssetReveal = [this](const std::filesystem::path& path)
		{
			RevealAssetPath(path);
		},
		.OnAssetImportSettingsRequested = [this](const std::filesystem::path& path)
		{
			static_cast<void>(CreateOrUpdateImportSettingsForAsset(path));
		},
		.OnAssetReimportRequested = [this](const std::filesystem::path& path)
		{
			if (!Asset::IsModelAssetPath(path))
			{
				AppendAssetLog(std::format("Reimport ignored: not a model asset {}", path.string()));
				return;
			}
			if (m_RuntimeAssetRegistry.GetEntities(path).empty())
			{
				AppendAssetLog(std::format("Reimport skipped: model is not loaded in the current scene {}", path.string()));
				return;
			}
			QueueModelReload(path, path);
		},
		.OnModelDrop = [this](const std::filesystem::path& path, Editor::AssetDropTarget target)
		{
			QueueModelImportFromDrop(path, target);
		},
		.OnProjectRefresh = [this]()
		{
			m_AssetFileSystem.RequestRefresh();
			ConfigureResourceSystem();
		},
		.OnLoadPrefabForInspection = [this](
			const std::filesystem::path& prefabPath,
			ScenePersistence::LoadedSceneEntity& root,
			std::string& errorMessage) -> bool
		{
			if (!m_Project)
			{
				errorMessage = "Prefab inspection requires a loaded project.";
				return false;
			}
			ScenePersistence::LoadPrefabResult result = ScenePersistence::PrefabService::LoadPrefab(prefabPath, *m_Project);
			if (!result.Success)
			{
				errorMessage = result.ErrorMessage;
				return false;
			}
			root = std::move(result.Root);
			return true;
		},
		.OnApplyEntityToPrefab = [this](EntityId entityId) -> bool
		{
			return ApplyEntityToPrefabSource(entityId);
		},
		.OnSavePrefabInspectionRoot = [this](
			const std::filesystem::path& prefabPath,
			const ScenePersistence::LoadedSceneEntity& root) -> bool
		{
			return SavePrefabInspectionRoot(prefabPath, root);
		},
		.OnRevertMeshToPrefabSource = [this](
			EntityId entityId,
			const ScenePersistence::LoadedSceneEntity& prefabRoot,
			const std::filesystem::path& prefabPath) -> bool
		{
			return RevertEntityMeshToPrefabSource(entityId, prefabRoot, prefabPath);
		},
		.OnGetMeshRestoreStatus = [this](EntityId entityId) -> Editor::MeshRestoreRuntimeStatus
		{
			return GetMeshRestoreRuntimeStatus(entityId);
		},
		.OnCancelMeshRestore = [this](EntityId entityId) -> bool
		{
			return CancelMeshRestore(entityId);
		},
		.OnApplyConflictedMeshRestore = [this](EntityId entityId) -> bool
		{
			return ApplyConflictedMeshRestore(entityId);
		},
		.OnReloadMeshRestoreFromPrefabSource = [this](EntityId entityId) -> bool
		{
			return ReloadMeshRestoreFromPrefabSource(entityId);
		},
		.OnLoadSceneReference = [this](EntityId entityId) -> bool
		{
			return LoadSceneReference(entityId);
		},
		.OnUnloadSceneReference = [this](EntityId entityId) -> bool
		{
			return UnloadSceneReference(entityId);
		},
		.OnGetSceneReferenceStatus = [this](EntityId entityId) -> Editor::SceneReferenceRuntimeStatus
		{
			return GetSceneReferenceRuntimeStatus(entityId);
		},
		.OnGetNestedSceneChildStatus = [this](EntityId entityId) -> Editor::NestedSceneChildStatus
		{
			return GetNestedSceneChildStatus(entityId);
		},
		.OnMakeNestedSceneChildLocal = [this](EntityId entityId) -> bool
		{
			return MakeNestedSceneChildLocalWithUndo(entityId);
		},
		.OnCreateProjectAsset = [this](Editor::ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory)
		{
			static_cast<void>(CreateProjectAsset(kind, targetDirectory));
		},
		.OnCreateNamedProjectAsset = [this](Editor::ProjectCreateAssetKind kind, const std::filesystem::path& targetDirectory, std::string_view requestedName)
		{
			static_cast<void>(CreateProjectAsset(kind, targetDirectory, requestedName));
		},
		.OnRenameEntity = [this](EntityId entityId, std::string_view name)
		{
			RenameEntityFromHierarchy(entityId, name);
		},
		.OnDuplicateEntity = [this](EntityId entityId)
		{
			DuplicateEntityFromHierarchyWithUndo(entityId);
		},
		.OnDuplicateEntities = [this](std::vector<EntityId> entityIds)
		{
			DuplicateEntitiesFromHierarchyWithUndo(std::move(entityIds));
		},
		.OnDeleteEntity = [this](EntityId entityId)
		{
			DeleteEntityFromHierarchyWithUndo(entityId);
		},
		.OnDeleteEntities = [this](std::vector<EntityId> entityIds)
		{
			DeleteEntitiesFromHierarchyWithUndo(std::move(entityIds));
		},
		.OnEntitySceneVisibilityChanged = [this](EntityId entityId, bool visible)
		{
			SetEntitySceneVisibilityWithUndo(entityId, visible);
		},
		.OnEntityScenePickabilityChanged = [this](EntityId entityId, bool pickable)
		{
			SetEntityScenePickabilityWithUndo(entityId, pickable);
		},
		.OnCreateEmptyEntity = [this](EntityId parentEntity)
		{
			CreateEmptySceneEntityWithUndo("Entity", parentEntity);
		},
		.OnCreateCameraEntity = [this](EntityId parentEntity)
		{
			CreateCameraSceneEntityWithUndo(parentEntity);
		},
		.OnCreateLightEntity = [this](EntityId parentEntity)
		{
			CreateLightSceneEntityWithUndo(parentEntity);
		},
		.OnCreatePrimitive = [this](Asset::PrimitiveMeshKind kind, EntityId parentEntity)
		{
			CreatePrimitiveEntityWithUndo(kind, parentEntity);
		},
		.OnCreateEmptyParentForEntity = [this](EntityId childEntity)
		{
			CreateEmptyParentForEntityWithUndo(childEntity);
		},
		.OnMoveEntity = [this](EntityId movedEntity, EntityId targetEntity, Editor::EntityDropPlacement placement)
		{
			MoveEntitiesInHierarchyWithUndo({ movedEntity }, targetEntity, placement);
		},
		.OnMoveEntities = [this](std::vector<EntityId> movedEntities, EntityId targetEntity, Editor::EntityDropPlacement placement)
		{
			MoveEntitiesInHierarchyWithUndo(std::move(movedEntities), targetEntity, placement);
		},
		.OnComponentAdded = [this](EntityId entityId, SceneComponentKind kind)
		{
			AddComponentToEntityWithUndo(entityId, kind);
		},
		.OnComponentRemoved = [this](EntityId entityId, SceneComponentKind kind)
		{
			RemoveComponentFromEntityWithUndo(entityId, kind);
		},
		.OnComponentEnabledChanged = [this](EntityId entityId, SceneComponentKind kind, bool enabled)
		{
			SetComponentEnabledForEntityWithUndo(entityId, kind, enabled);
		},
		.OnComponentReset = [this](EntityId entityId, SceneComponentKind kind)
		{
			ResetComponentForEntityWithUndo(entityId, kind);
		},
		.OnComponentPaste = [this](EntityId entityId, SceneComponentKind kind, const ScenePersistence::LoadedSceneEntity& snapshot)
		{
			PasteComponentValuesToEntityWithUndo(entityId, kind, snapshot);
		},
		.OnTransformEditCommitted = [this](EntityId entityId, const Math::Transform& beforeTransform, const Math::Transform& afterTransform)
		{
			CommitTransformEdit(entityId, beforeTransform, afterTransform);
		},
		.OnTransformBatchEditCommitted = [this](std::vector<Editor::TransformEditRecord> records)
		{
			CommitTransformBatchEdit(std::move(records));
		},
		.OnComponentBatchEditCommitted = [this](std::vector<Editor::ComponentEditRecord> records)
		{
			CommitComponentBatchEdit(std::move(records));
		},
		.OnMaterialEditCommitted = [this](EntityId entityId, size_t materialIndex, const Asset::StaticMeshMaterial& beforeMaterial, const Asset::StaticMeshMaterial& afterMaterial)
		{
			CommitMaterialEdit(entityId, materialIndex, beforeMaterial, afterMaterial);
		},
		.OnMaterialBatchEditCommitted = [this](std::vector<Editor::MaterialEditRecord> records)
		{
			CommitMaterialBatchEdit(std::move(records));
		},
		.OnMaterialShadingModelChanged = [this](EntityId entityId, size_t materialIndex, Asset::MaterialShadingModel model)
		{
			SetMaterialShadingModelWithUndo(entityId, materialIndex, model);
		},
		.OnMaterialTextureAssigned = [this](EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot, const std::filesystem::path& path)
		{
			AssignMaterialTextureWithUndo(entityId, materialIndex, slot, path);
		},
		.OnMaterialTexturesAssigned = [this](EntityId entityId, size_t materialIndex, const std::vector<Editor::MaterialTextureAssignment>& assignments)
		{
			AssignMaterialTexturesWithUndo(entityId, materialIndex, assignments);
		},
		.OnMaterialTextureBatchAssigned = [this](EntityId entityId, const std::vector<Editor::MaterialTextureBatchAssignment>& batchAssignments)
		{
			AssignMaterialTextureBatchWithUndo(entityId, batchAssignments);
		},
		.OnMaterialTextureCleared = [this](EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot)
		{
			ClearMaterialTextureWithUndo(entityId, materialIndex, slot);
		},
		.OnMaterialTextureBrowseRequested = [this](EntityId entityId, size_t materialIndex, Asset::MaterialTextureSlot slot)
		{
			BrowseMaterialTexture(entityId, materialIndex, slot);
		},
		.OnMaterialEdited = [this](EntityId entityId, size_t materialIndex)
		{
			MarkMaterialEdited(entityId, materialIndex);
		},
		.OnAmbientColorChanged = [this](const DirectX::XMFLOAT3& ambientColor)
		{
			m_AmbientColor = {
				std::clamp(ambientColor.x, 0.0f, 4.0f),
				std::clamp(ambientColor.y, 0.0f, 4.0f),
				std::clamp(ambientColor.z, 0.0f, 4.0f)
			};
			MarkSceneDirty();
		},
		.OnAmbientIntensityChanged = [this](float ambientIntensity)
		{
			m_AmbientIntensity = std::clamp(ambientIntensity, 0.0f, 2.0f);
			MarkSceneDirty();
		},
		.OnExposureChanged = [this](float exposure)
		{
			m_Exposure = std::clamp(exposure, 0.05f, 8.0f);
			MarkSceneDirty();
		},
		.OnSkyboxSettingsChanged = [this](const Rendering::SkyboxSettings& skybox)
		{
			m_SkyboxSettings = Rendering::ClampSkyboxSettings(skybox);
			MarkSceneDirty();
		},
		.OnKeyLightIntensityChanged = [this](float intensity)
		{
			if (LightComponent* light = m_Scene.GetLightComponent(ResolveKeyLightEntity()))
			{
				light->Intensity = std::clamp(intensity, 0.0f, 100.0f);
				MarkSceneDirty();
			}
		},
		.OnMaterialDebugViewChanged = [this](MaterialDebugView debugView)
		{
			m_MaterialDebugView = debugView;
		},
		.OnViewFrustumCullingChanged = [this](bool enabled)
		{
			m_ViewFrustumCullingEnabled = enabled;
		},
		.OnShadowSettingsChanged = [this](const Rendering::ShadowSettings& settings)
		{
			m_ShadowSettings = settings;
			MarkSceneDirty();
		},
		.OnSceneEdited = [this]()
		{
			if (!IsRuntimePlaying())
			{
				MarkSceneDirty();
			}
		},
		.OnPhysicsSimulationChanged = [this](bool enabled)
		{
			SetPhysicsSimulationEnabled(enabled);
		},
		.OnPhysicsActorDirty = [this](EntityId entityId)
		{
			MarkPhysicsActorDirty(entityId);
		}
	};
	m_EditorLayer.Draw(editorContext);
	SyncGameCameraFromSceneEntity();
}

void Engine::RenderEditorDrawData()
{
	if (!m_ImGuiInitialized)
	{
		return;
	}

	if (m_SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		const Editor::ViewportPanelState& gameViewport = m_EditorLayer.GetGameViewport();
		if (gameViewport.CanRender())
		{
			m_BenchmarkRunner.DrawViewportOverlay(
				m_Camera,
				gameViewport.Left,
				gameViewport.Top,
				gameViewport.Width,
				gameViewport.Height);
		}
	}

	const size_t imguiPass = m_RenderGraph.AddPass("Render ImGui Draw Data", Rendering::RenderPassKind::Editor, "Swapchain");
	MeasureRenderGraphPass(m_RenderGraph, imguiPass, [this]()
		{
			ImGui::Render();
			m_Graphics.CommandList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
			m_Graphics.CommandList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

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
		});
}

void Engine::UpdateEditorCameraFromInput(float deltaTime)
{
	if (ImGui::GetCurrentContext() == nullptr)
	{
		return;
	}

	const Editor::ViewportPanelState& sceneViewport = m_EditorLayer.GetSceneViewport();
	if (!sceneViewport.IsVisible)
	{
		m_SceneCameraControlActive = false;
		return;
	}

	if (sceneViewport.IsHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		m_SceneCameraControlActive = true;
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		m_SceneCameraControlActive = false;
	}

	const bool keyboardActive = sceneViewport.IsHovered || sceneViewport.IsFocused || m_SceneCameraControlActive;
	if (!keyboardActive)
	{
		return;
	}

	const ImGuiIO& io = ImGui::GetIO();
	const float mouseDeltaX = std::clamp(io.MouseDelta.x, -120.0f, 120.0f);
	const float mouseDeltaY = std::clamp(io.MouseDelta.y, -120.0f, 120.0f);
	m_SceneCamera.UpdateFromInputState(
		deltaTime,
		ImGui::IsKeyDown(ImGuiKey_W),
		ImGui::IsKeyDown(ImGuiKey_S),
		ImGui::IsKeyDown(ImGuiKey_A),
		ImGui::IsKeyDown(ImGuiKey_D),
		ImGui::IsKeyDown(ImGuiKey_Q),
		ImGui::IsKeyDown(ImGuiKey_E),
		m_SceneCameraControlActive,
		mouseDeltaX,
		mouseDeltaY);

	if (m_SceneCameraControlActive)
	{
		ImGui::SetMouseCursor(ImGuiMouseCursor_None);
	}
}

void Engine::UpdateViewportCameraLenses()
{
	const Editor::ViewportPanelState& sceneViewport = m_EditorLayer.GetSceneViewport();
	if (sceneViewport.CanRender())
	{
		m_SceneCamera.SetLens(DirectX::XM_PIDIV4, sceneViewport.AspectRatio(), 0.1f, 1000.0f);
	}

	const Editor::ViewportPanelState& gameViewport = m_EditorLayer.GetGameViewport();
	if (gameViewport.CanRender())
	{
		if (CameraComponent* camera = m_Scene.GetCameraComponent(m_GameCameraEntity);
			camera && m_Scene.IsCameraEnabled(m_GameCameraEntity))
		{
			m_Camera.SetLens(camera->FovY, gameViewport.AspectRatio(), camera->NearZ, camera->FarZ);
		}
		else
		{
			m_Camera.SetLens(DirectX::XM_PIDIV4, gameViewport.AspectRatio(), 0.1f, 1000.0f);
		}
	}
}

void Engine::RenderWorldViewport(const Editor::ViewportPanelState& viewport, const Camera& camera, std::string_view label, bool isGameView)
{
	if (!viewport.CanRender())
	{
		m_RenderGraph.AddPass(label, Rendering::RenderPassKind::World, "Viewport", "Skipped: hidden or too small", false);
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	if (right <= left || bottom <= top)
	{
		m_RenderGraph.AddPass(label, Rendering::RenderPassKind::World, "Viewport", "Skipped: invalid viewport rect", false);
		return;
	}

	const float width = static_cast<float>(right - left);
	const float height = static_cast<float>(bottom - top);
	const size_t worldPass = m_RenderGraph.AddPass(label, Rendering::RenderPassKind::World, "Viewport", RenderModeToString(m_RenderMode));
	const auto worldBegin = std::chrono::steady_clock::now();
	ResetViewCullingCache();
	BuildViewportVisibleRenderList(camera, isGameView);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());

	const std::array<float, 4> skyClearColor = BuildSkyClearColor(m_SkyboxSettings);
	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
		if (native)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
			backBufferRtv.ptr = reinterpret_cast<SIZE_T>(m_Graphics.Device->GetCurrentBackBufferRTV());
			const D3D12_RECT clearRect = {
				.left = left,
				.top = top,
				.right = right,
				.bottom = bottom
			};
			native->ClearRenderTargetView(backBufferRtv, skyClearColor.data(), 1, &clearRect);
		}
	}
	else if (auto commandList = dynamic_cast<VulkanCommandList*>(m_Graphics.CommandList.get()))
	{
		commandList->BeginSwapchainRenderPassForExternalCommands();
		auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
		if (commandBuffer != VK_NULL_HANDLE)
		{
			VkClearAttachment clearAttachment = {};
			clearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			clearAttachment.colorAttachment = 0;
			clearAttachment.clearValue.color.float32[0] = skyClearColor[0];
			clearAttachment.clearValue.color.float32[1] = skyClearColor[1];
			clearAttachment.clearValue.color.float32[2] = skyClearColor[2];
			clearAttachment.clearValue.color.float32[3] = skyClearColor[3];

			const VkClearRect clearRect = {
				.rect = {
					.offset = { static_cast<int32_t>(left), static_cast<int32_t>(top) },
					.extent = {
						static_cast<uint32_t>(right - left),
						static_cast<uint32_t>(bottom - top)
					}
				},
				.baseArrayLayer = 0,
				.layerCount = 1
			};
			vkCmdClearAttachments(commandBuffer, 1, &clearAttachment, 1, &clearRect);
		}
	}

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		if (m_RenderMode == RenderMode::Deferred)
		{
			m_ShadowFrameData = Rendering::ShadowSystem::BuildDirectionalShadowFrameData(m_Scene, ResolveKeyLightEntity(), camera, m_ShadowSettings);
			const size_t shadowPass = m_RenderGraph.AddPass("DX12 Shadow Map Depth", Rendering::RenderPassKind::Shadow, "ShadowMap", label, m_ShadowFrameData.Enabled && m_ShadowFrameData.HasDirectionalCaster);
			MeasureRenderGraphPass(m_RenderGraph, shadowPass, [this, &camera]()
				{
					DrawDx12ShadowDepthPass(camera);
				});
			m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
			m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
			m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
			m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());
			const DeferredPassTimingIndices timings{
				.Geometry = m_RenderGraph.AddPass("DX12 GBuffer Geometry", Rendering::RenderPassKind::Geometry, label),
				.TileCulling = m_RenderGraph.AddPass("DX12 Deferred Tile Culling", Rendering::RenderPassKind::Debug, label, "CPU builds per-tile light index list"),
				.Lighting = m_RenderGraph.AddPass("DX12 Deferred Lighting", Rendering::RenderPassKind::Lighting, label),
				.PostProcess = m_RenderGraph.AddPass("DX12 Tone Map", Rendering::RenderPassKind::PostProcess, "HDRColor -> Swapchain", label),
				.Transparency = m_RenderGraph.AddPass("DX12 Forward Transparency", Rendering::RenderPassKind::Transparency, label)
			};
			DrawDx12DeferredTriangle(viewport, camera, timings);
		}
		else
		{
			const size_t skyboxPass = m_RenderGraph.AddPass("DX12 Procedural Skybox", Rendering::RenderPassKind::Clear, label, "Fullscreen procedural sky", m_SkyboxSettings.Enabled);
			MeasureRenderGraphPass(m_RenderGraph, skyboxPass, [this, &viewport, &camera]()
				{
					DrawDx12Skybox(viewport, camera);
				});
			const size_t forwardPass = m_RenderGraph.AddPass("DX12 Forward Mesh", Rendering::RenderPassKind::Geometry, label);
			MeasureRenderGraphPass(m_RenderGraph, forwardPass, [this, &camera]()
				{
					DrawDx12Triangle(camera);
				});
		}
	}
	else
	{
		if (m_RenderMode == RenderMode::Deferred)
		{
			m_ShadowFrameData = Rendering::ShadowSystem::BuildDirectionalShadowFrameData(m_Scene, ResolveKeyLightEntity(), camera, m_ShadowSettings);
			const size_t shadowPass = m_RenderGraph.AddPass("Vulkan Shadow Map Depth", Rendering::RenderPassKind::Shadow, "ShadowMap", label, m_ShadowFrameData.Enabled && m_ShadowFrameData.HasDirectionalCaster);
			MeasureRenderGraphPass(m_RenderGraph, shadowPass, [this, &camera]()
				{
					DrawVulkanShadowDepthPass(camera);
				});
			m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
			m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
			m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
			m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());
			const DeferredPassTimingIndices timings{
				.Geometry = m_RenderGraph.AddPass("Vulkan GBuffer Geometry", Rendering::RenderPassKind::Geometry, label),
				.TileCulling = m_RenderGraph.AddPass("Vulkan Deferred Tile Culling", Rendering::RenderPassKind::Debug, label, "CPU builds per-tile light index list"),
				.Lighting = m_RenderGraph.AddPass("Vulkan Deferred Lighting", Rendering::RenderPassKind::Lighting, label),
				.PostProcess = m_RenderGraph.AddPass("Vulkan Tone Map", Rendering::RenderPassKind::PostProcess, "HDRColor -> Swapchain", label),
				.Transparency = m_RenderGraph.AddPass("Vulkan Forward Transparency", Rendering::RenderPassKind::Transparency, label)
			};
			DrawVulkanDeferredTriangle(viewport, camera, timings);
		}
		else
		{
			const size_t skyboxPass = m_RenderGraph.AddPass("Vulkan Procedural Skybox", Rendering::RenderPassKind::Clear, label, "Fullscreen procedural sky", m_SkyboxSettings.Enabled);
			MeasureRenderGraphPass(m_RenderGraph, skyboxPass, [this, &viewport, &camera]()
				{
					DrawVulkanSkybox(viewport, camera);
				});
			const size_t forwardPass = m_RenderGraph.AddPass("Vulkan Forward Mesh", Rendering::RenderPassKind::Geometry, label);
			MeasureRenderGraphPass(m_RenderGraph, forwardPass, [this, &camera]()
				{
					DrawVulkanTriangle(camera);
				});
		}
	}

	const size_t benchmarkPass = m_RenderGraph.AddPass("Benchmark Overlay Instances", Rendering::RenderPassKind::Debug, label, "Only draws in ECS Benchmark mode", m_SampleMode == Samples::Benchmark::SampleMode::EcsBenchmark);
	MeasureRenderGraphPass(m_RenderGraph, benchmarkPass, [this, &camera]()
		{
			DrawBenchmarkInstances(camera);
		});

	const auto worldEnd = std::chrono::steady_clock::now();
	const std::chrono::duration<double, std::milli> worldElapsed = worldEnd - worldBegin;
	m_RenderGraph.SetPassCpuTime(worldPass, worldElapsed.count(), false);
	ResetViewCullingCache();
}

void Engine::ResetRenderFrameStats()
{
	const Scene& runtimeScene = GetRuntimeScene();
	m_RenderFrameStats = {};
	m_RenderFrameStats.FrameIndex = m_JobSystem.GetFrameIndex();
	m_RenderFrameStats.RenderEntityCount = static_cast<uint32_t>(m_RenderState.RenderEntities.size());

	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		++m_RenderFrameStats.EnabledMeshEntityCount;
		bool hasTransparentMaterial = false;
		if (meshAsset->Submeshes.empty())
		{
			hasTransparentMaterial = IsMaterialTransparent(entityId, 0);
		}
		else
		{
			for (const Asset::StaticMeshSubmesh& submesh : meshAsset->Submeshes)
			{
				if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					hasTransparentMaterial = true;
					break;
				}
			}
		}

		if (hasTransparentMaterial)
		{
			++m_RenderFrameStats.TransparentEntityCount;
		}
	}
}

void Engine::ResetViewCullingCache()
{
	m_ViewCullingCache.clear();
	m_ViewportVisibleRenderEntities.clear();
}

void Engine::BuildViewportVisibleRenderList(const Camera& camera, bool isGameView)
{
	const Scene& runtimeScene = GetRuntimeScene();
	m_ViewportVisibleRenderEntities.clear();
	m_ViewportVisibleRenderEntities.reserve(m_RenderState.RenderEntities.size());
	const uint32_t requestCountBefore = m_RenderFrameStats.ViewCullingRequestCount;
	const uint32_t culledCountBefore = m_RenderFrameStats.ViewCulledEntityCount;
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (entityId == InvalidEntityId || !runtimeScene.IsMeshEnabled(entityId) || !runtimeScene.GetMeshAsset(entityId))
		{
			continue;
		}
		if (!isGameView && !runtimeScene.IsEntityVisibleInScene(entityId))
		{
			continue;
		}
		if (!m_ViewFrustumCullingEnabled || ShouldSubmitEntityForCamera(entityId, camera))
		{
			m_ViewportVisibleRenderEntities.push_back(entityId);
		}
	}
	const uint32_t visibleListCount = static_cast<uint32_t>(m_ViewportVisibleRenderEntities.size());
	const uint32_t requestCount = m_RenderFrameStats.ViewCullingRequestCount - requestCountBefore;
	const uint32_t culledCount = m_RenderFrameStats.ViewCulledEntityCount - culledCountBefore;
	m_RenderFrameStats.ViewVisibleListEntityCount += visibleListCount;
	if (isGameView)
	{
		m_RenderFrameStats.GameViewCullingRequestCount += requestCount;
		m_RenderFrameStats.GameViewVisibleListEntityCount += visibleListCount;
		m_RenderFrameStats.GameViewCulledEntityCount += culledCount;
	}
	else
	{
		m_RenderFrameStats.SceneViewCullingRequestCount += requestCount;
		m_RenderFrameStats.SceneViewVisibleListEntityCount += visibleListCount;
		m_RenderFrameStats.SceneViewCulledEntityCount += culledCount;
	}
}

bool Engine::ShouldSubmitEntityForCamera(EntityId entityId, const Camera& camera)
{
	++m_RenderFrameStats.ViewCullingRequestCount;
	if (const auto cachedVisibilityIt = m_ViewCullingCache.find(entityId);
		cachedVisibilityIt != m_ViewCullingCache.end())
	{
		++m_RenderFrameStats.ViewCullingCacheHitCount;
		if (cachedVisibilityIt->second)
		{
			++m_RenderFrameStats.ViewVisibleEntityCount;
		}
		else
		{
			++m_RenderFrameStats.ViewCulledEntityCount;
		}
		return cachedVisibilityIt->second;
	}

	++m_RenderFrameStats.ViewCullingCacheMissCount;
	++m_RenderFrameStats.ViewCullingTestCount;

	const Scene& runtimeScene = GetRuntimeScene();
	const TransformComponent* transform = runtimeScene.GetTransformComponent(entityId);
	const BoundsComponent* bounds = runtimeScene.GetBoundsComponent(entityId);
	if (!transform || !bounds)
	{
		m_ViewCullingCache[entityId] = true;
		++m_RenderFrameStats.ViewVisibleEntityCount;
		return true;
	}

	const DirectX::XMVECTOR localMin = DirectX::XMLoadFloat3(&bounds->LocalMin);
	const DirectX::XMVECTOR localMax = DirectX::XMLoadFloat3(&bounds->LocalMax);
	const DirectX::XMVECTOR localCenter = DirectX::XMVectorScale(DirectX::XMVectorAdd(localMin, localMax), 0.5f);
	const DirectX::XMVECTOR localExtent = DirectX::XMVectorScale(DirectX::XMVectorSubtract(localMax, localMin), 0.5f);
	const DirectX::XMVECTOR worldCenter = DirectX::XMVector3TransformCoord(localCenter, transform->GetWorldXmMatrix());

	const DirectX::XMFLOAT3& scale = transform->WorldTransform.Scale;
	const float maxScale = (std::max)(0.0001f, (std::max)(std::fabs(scale.x), (std::max)(std::fabs(scale.y), std::fabs(scale.z))));
	const float localRadius = DirectX::XMVectorGetX(DirectX::XMVector3Length(localExtent));
	const float radius = (std::max)(localRadius * maxScale, 0.001f);

	const DirectX::XMFLOAT3 cameraPositionValue = camera.GetPosition();
	const DirectX::XMFLOAT3 cameraForwardValue = camera.GetForward();
	const DirectX::XMFLOAT3 cameraRightValue = camera.GetRight();
	const DirectX::XMFLOAT3 cameraUpValue = camera.GetUp();
	const DirectX::XMVECTOR cameraPosition = DirectX::XMLoadFloat3(&cameraPositionValue);
	const DirectX::XMVECTOR cameraForward = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraForwardValue));
	const DirectX::XMVECTOR cameraRight = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraRightValue));
	const DirectX::XMVECTOR cameraUp = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraUpValue));
	const DirectX::XMVECTOR toCenter = DirectX::XMVectorSubtract(worldCenter, cameraPosition);

	const float z = DirectX::XMVectorGetX(DirectX::XMVector3Dot(toCenter, cameraForward));
	const float x = DirectX::XMVectorGetX(DirectX::XMVector3Dot(toCenter, cameraRight));
	const float y = DirectX::XMVectorGetX(DirectX::XMVector3Dot(toCenter, cameraUp));
	const float nearZ = (std::max)(camera.GetNearZ(), 0.001f);
	const float farZ = (std::max)(camera.GetFarZ(), nearZ + 0.001f);
	const float tanY = std::tan((std::max)(camera.GetFovY(), 0.001f) * 0.5f);
	const float tanX = tanY * (std::max)(camera.GetAspect(), 0.001f);
	const float invSideLengthX = 1.0f / std::sqrt(tanX * tanX + 1.0f);
	const float invSideLengthY = 1.0f / std::sqrt(tanY * tanY + 1.0f);
	const float sideCosX = invSideLengthX;
	const float sideSinX = tanX * invSideLengthX;
	const float sideCosY = invSideLengthY;
	const float sideSinY = tanY * invSideLengthY;

	const bool visible =
		z + radius >= nearZ &&
		z - radius <= farZ &&
		(x * sideCosX + z * sideSinX) >= -radius &&
		(-x * sideCosX + z * sideSinX) >= -radius &&
		(y * sideCosY + z * sideSinY) >= -radius &&
		(-y * sideCosY + z * sideSinY) >= -radius;

	if (visible)
	{
		++m_RenderFrameStats.ViewVisibleEntityCount;
	}
	else
	{
		++m_RenderFrameStats.ViewCulledEntityCount;
	}
	m_ViewCullingCache[entityId] = visible;
	return visible;
}

void Engine::RecordIndexedDraw(Rendering::DrawSubmissionKind kind, uint32_t indexCount, uint32_t instanceCount) noexcept
{
	const uint32_t safeInstanceCount = (std::max)(instanceCount, 1u);
	++m_RenderFrameStats.DrawCallCount;
	++m_RenderFrameStats.IndexedDrawCallCount;
	if (safeInstanceCount > 1)
	{
		++m_RenderFrameStats.InstancedDrawCallCount;
	}
	m_RenderFrameStats.SubmittedIndexCount += static_cast<uint64_t>(indexCount) * safeInstanceCount;
	m_RenderFrameStats.SubmittedTriangleCount += static_cast<uint64_t>(indexCount / 3u) * safeInstanceCount;
	m_RenderFrameStats.SubmittedInstanceCount += safeInstanceCount;

	switch (kind)
	{
	case Rendering::DrawSubmissionKind::Transparent:
		++m_RenderFrameStats.TransparentDrawCallCount;
		break;
	case Rendering::DrawSubmissionKind::Shadow:
		++m_RenderFrameStats.ShadowDrawCallCount;
		break;
	case Rendering::DrawSubmissionKind::DeferredGeometry:
		++m_RenderFrameStats.DeferredGeometryDrawCallCount;
		break;
	case Rendering::DrawSubmissionKind::Benchmark:
		++m_RenderFrameStats.BenchmarkDrawCallCount;
		break;
	case Rendering::DrawSubmissionKind::Fullscreen:
		++m_RenderFrameStats.FullscreenDrawCallCount;
		break;
	case Rendering::DrawSubmissionKind::Opaque:
	default:
		++m_RenderFrameStats.OpaqueDrawCallCount;
		break;
	}
}

void Engine::RecordFullscreenDraw(Rendering::DrawSubmissionKind kind, uint32_t vertexCount, uint32_t instanceCount) noexcept
{
	const uint32_t safeInstanceCount = (std::max)(instanceCount, 1u);
	++m_RenderFrameStats.DrawCallCount;
	++m_RenderFrameStats.FullscreenDrawCallCount;
	if (safeInstanceCount > 1)
	{
		++m_RenderFrameStats.InstancedDrawCallCount;
	}
	m_RenderFrameStats.SubmittedTriangleCount += static_cast<uint64_t>(vertexCount / 3u) * safeInstanceCount;
	m_RenderFrameStats.SubmittedInstanceCount += safeInstanceCount;

	if (kind == Rendering::DrawSubmissionKind::Benchmark)
	{
		++m_RenderFrameStats.BenchmarkDrawCallCount;
	}
}

void Engine::FramePrimaryRenderableCamera()
{
	EntityId focusEntity = m_Scene.GetPrimaryRenderableEntity();
	if (focusEntity == InvalidEntityId && !m_RenderState.RenderEntities.empty())
	{
		focusEntity = m_RenderState.RenderEntities.front();
	}

	FrameEntityCamera(m_Camera, focusEntity);
	m_BenchmarkRunner.SetSpawnView(m_Camera);
	SyncRuntimeCameraToGameCameraEntity();
}

void Engine::FrameSelectedEntityCamera()
{
	FrameSelectedEntityCamera(m_Camera);
}

void Engine::FramePrimaryRenderableCamera(Camera& camera)
{
	EntityId focusEntity = m_Scene.GetPrimaryRenderableEntity();
	if (focusEntity == InvalidEntityId && !m_RenderState.RenderEntities.empty())
	{
		focusEntity = m_RenderState.RenderEntities.front();
	}

	FrameEntityCamera(camera, focusEntity);
}

void Engine::FrameSelectedEntityCamera(Camera& camera)
{
	EntityId focusEntity = m_Scene.GetSelectedEntity();
	if (focusEntity == InvalidEntityId)
	{
		focusEntity = m_Scene.GetPrimaryRenderableEntity();
	}
	if (focusEntity == InvalidEntityId && !m_RenderState.RenderEntities.empty())
	{
		focusEntity = m_RenderState.RenderEntities.front();
	}

	FrameEntityCamera(camera, focusEntity);
}

void Engine::FrameEntityCamera(EntityId entityId)
{
	FrameEntityCamera(m_Camera, entityId);
	m_BenchmarkRunner.SetSpawnView(m_Camera);
	SyncRuntimeCameraToGameCameraEntity();
}

void Engine::FrameEntityCamera(Camera& camera, EntityId entityId)
{
	const TransformComponent* transform = GetTransformComponent(entityId);
	if (!transform)
	{
		return;
	}

	DirectX::XMFLOAT3 localCenter = { 0.0f, 0.0f, 0.0f };
	float maxExtent = 3.0f;
	if (const BoundsComponent* bounds = m_Scene.GetBoundsComponent(entityId))
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
	camera.LookAt(
		{ worldCenter.x, worldCenter.y + maxExtent * 0.35f, worldCenter.z - cameraDistance },
		worldCenter,
		{ 0.0f, 1.0f, 0.0f });
}

void Engine::DrawBenchmarkInstances(const Camera& camera)
{
	if (m_SampleMode != Samples::Benchmark::SampleMode::EcsBenchmark)
	{
		return;
	}

	if (m_Graphics.CurrentApi == GraphicsAPI::DirectX12)
	{
		DrawDx12BenchmarkInstances(camera);
	}
	else
	{
		DrawVulkanBenchmarkInstances(camera);
	}
}

void Engine::DrawDx12BenchmarkInstances(const Camera& camera)
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto lightResource = m_StaticMeshRenderer.DeferredLightBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredLightBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!native || !cameraResource || !lightResource || !dx12Device || !m_StaticMeshRenderer.Dx12.PipelineState || !m_StaticMeshRenderer.Dx12.RootSignature)
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

	const uint64_t cameraOffset = UpdateBenchmarkCameraBuffer(camera, instanceCount, localScale);
	if (cameraOffset == InvalidCameraConstantOffset())
	{
		return;
	}
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + cameraOffset);
	native->SetGraphicsRootShaderResourceView(2, lightResource->GetGPUVirtualAddress());
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
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, static_cast<uint32_t>(meshAsset->Indices.size()), instanceCount);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), instanceCount, 0, 0, 0);
			return;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			const size_t materialIndex = submesh.MaterialIndex < m_StaticMeshRenderer.Dx12.MaterialCount ? submesh.MaterialIndex : 0;
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex * MaterialSlotCount();
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, submesh.IndexCount, instanceCount);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, instanceCount, submesh.IndexOffset, 0, 0);
		}
		return;
	}

	native->SetGraphicsRootDescriptorTable(1, baseHandle);
	const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		? GetBenchmarkSpiderGlyphGeometry()
		: GetBenchmarkPrimitiveGeometry();
	RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, static_cast<uint32_t>(geometry.Indices.size()), instanceCount);
	m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(geometry.Indices.size()), instanceCount, 0, 0, 0);
}

void Engine::DrawVulkanBenchmarkInstances(const Camera& camera)
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

	const uint64_t cameraOffset = UpdateBenchmarkCameraBuffer(camera, instanceCount, localScale);
	if (cameraOffset == InvalidCameraConstantOffset())
	{
		return;
	}
	const uint32_t cameraDynamicOffset = static_cast<uint32_t>(cameraOffset);
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());

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
				1,
				&cameraDynamicOffset);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, static_cast<uint32_t>(meshAsset->Indices.size()), instanceCount);
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
				1,
				&cameraDynamicOffset);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, submesh.IndexCount, instanceCount);
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
		1,
		&cameraDynamicOffset);
	const BenchmarkGeometry& geometry = config.ObjectType == Samples::Benchmark::BenchmarkObjectType::Spider
		? GetBenchmarkSpiderGlyphGeometry()
		: GetBenchmarkPrimitiveGeometry();
	RecordIndexedDraw(Rendering::DrawSubmissionKind::Benchmark, static_cast<uint32_t>(geometry.Indices.size()), instanceCount);
	m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(geometry.Indices.size()), instanceCount, 0, 0, 0);
}

void Engine::UploadBenchmarkGeometry(std::span<const Asset::StaticMeshVertex> vertices, std::span<const uint32_t> indices)
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

uint64_t Engine::UpdateBenchmarkCameraBuffer(const Camera& camera, uint32_t instanceCount, float localScale)
{
	if (!m_StaticMeshRenderer.CameraBuffer)
	{
		return InvalidCameraConstantOffset();
	}

	CameraConstants cameraConstants = {};
	DirectX::XMStoreFloat4x4(&cameraConstants.WorldViewProjection, camera.GetProjectionMatrix());
	DirectX::XMStoreFloat4x4(&cameraConstants.ViewProjection, camera.GetViewProjectionMatrix());
	DirectX::XMStoreFloat4x4(&cameraConstants.World, DirectX::XMMatrixIdentity());
	DirectX::XMStoreFloat4x4(&cameraConstants.WorldInverseTranspose, DirectX::XMMatrixIdentity());
	const auto position = camera.GetPosition();
	cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
	cameraConstants.BenchmarkParams = {
		static_cast<float>(instanceCount),
		localScale,
		camera.GetFovY(),
		camera.GetAspect()
	};
	cameraConstants.AmbientSpecular = { 1.0f, 0.0f, 1.0f, 0.0f };
	cameraConstants.AmbientColorIntensity = { 1.0f, 1.0f, 1.0f, 1.0f };
	cameraConstants.ExposureDebug = { 1.0f, 0.0f, 0.0f, 0.0f };
	cameraConstants.LightCountParams = { 0.0f, 0.0f, 0.0f, 0.0f };
	cameraConstants.MaterialTextureFlags2.w = 1.0f;

	return WriteCameraConstants(cameraConstants);
}

bool Engine::CreateDx12TriangleResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
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
	descriptorRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(MaterialSlotCount()), 0);

	CD3DX12_ROOT_PARAMETER rootParameters[3] = {};
	rootParameters[0].InitAsConstantBufferView(0);
	rootParameters[1].InitAsDescriptorTable(1, &descriptorRange, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParameters[2].InitAsShaderResourceView(10, 0, D3D12_SHADER_VISIBILITY_PIXEL);

	CD3DX12_STATIC_SAMPLER_DESC samplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(static_cast<UINT>(std::size(rootParameters)), rootParameters, 1, &samplerDesc, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Normal)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TexCoord)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Color)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Tangent)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENTSIGN", 0, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TangentSign)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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

	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.TransparentPipelineState))))
	{
		return false;
	}

	if (!CreateDx12ShadowResources())
	{
		return false;
	}

	if (!CreateDx12SkyboxResources())
	{
		return false;
	}

	return CreateDx12DeferredResources();
}

bool Engine::CreateDx12SkyboxResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!dx12Device)
	{
		return false;
	}

	const std::string skyboxShaderSource = ShaderUtils::LoadShaderSource(GetDx12SkyboxShaderPath());
	if (skyboxShaderSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"DirectX12 Skybox 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3DCompile(skyboxShaderSource.c_str(), skyboxShaderSource.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &errors)) ||
		FAILED(D3DCompile(skyboxShaderSource.c_str(), skyboxShaderSource.size(), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pixelShader, &errors)))
	{
		return false;
	}

	CD3DX12_ROOT_PARAMETER rootParameters[1] = {};
	rootParameters[0].InitAsConstants(
		static_cast<UINT>(sizeof(Rendering::SkyboxGpuConstants) / sizeof(uint32_t)),
		0,
		0,
		D3D12_SHADER_VISIBILITY_PIXEL);

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(static_cast<UINT>(std::size(rootParameters)), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> signature;
	if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
		FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.SkyboxRootSignature))))
	{
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.SkyboxRootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.InputLayout = {};
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	psoDesc.SampleDesc.Count = 1;

	return SUCCEEDED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.SkyboxPipelineState)));
}

bool Engine::CreateDx12DeferredResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!dx12Device || !m_StaticMeshRenderer.Dx12.RootSignature)
	{
		return false;
	}

	const std::string geometryShaderSource = ShaderUtils::LoadShaderSource(GetDx12DeferredGeometryShaderPath());
	const std::string lightingShaderSource = ShaderUtils::LoadShaderSource(GetDx12DeferredLightingShaderPath());
	const std::string toneMapShaderSource = ShaderUtils::LoadShaderSource(GetDx12ToneMapShaderPath());
	if (geometryShaderSource.empty() || lightingShaderSource.empty() || toneMapShaderSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"DirectX12 Deferred 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ComPtr<ID3DBlob> geometryVertexShader;
	ComPtr<ID3DBlob> geometryPixelShader;
	ComPtr<ID3DBlob> lightingVertexShader;
	ComPtr<ID3DBlob> lightingPixelShader;
	ComPtr<ID3DBlob> toneMapVertexShader;
	ComPtr<ID3DBlob> toneMapPixelShader;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3DCompile(geometryShaderSource.c_str(), geometryShaderSource.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &geometryVertexShader, &errors)) ||
		FAILED(D3DCompile(geometryShaderSource.c_str(), geometryShaderSource.size(), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &geometryPixelShader, &errors)) ||
		FAILED(D3DCompile(lightingShaderSource.c_str(), lightingShaderSource.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &lightingVertexShader, &errors)) ||
		FAILED(D3DCompile(lightingShaderSource.c_str(), lightingShaderSource.size(), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &lightingPixelShader, &errors)) ||
		FAILED(D3DCompile(toneMapShaderSource.c_str(), toneMapShaderSource.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &toneMapVertexShader, &errors)) ||
		FAILED(D3DCompile(toneMapShaderSource.c_str(), toneMapShaderSource.size(), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &toneMapPixelShader, &errors)))
	{
		return false;
	}

	static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Normal)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TexCoord)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Color)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Tangent)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENTSIGN", 0, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TangentSign)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPsoDesc = {};
	geometryPsoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.RootSignature.Get();
	geometryPsoDesc.VS = { geometryVertexShader->GetBufferPointer(), geometryVertexShader->GetBufferSize() };
	geometryPsoDesc.PS = { geometryPixelShader->GetBufferPointer(), geometryPixelShader->GetBufferSize() };
	geometryPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	geometryPsoDesc.SampleMask = UINT_MAX;
	geometryPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	geometryPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	geometryPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	geometryPsoDesc.DepthStencilState.DepthEnable = TRUE;
	geometryPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	geometryPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	geometryPsoDesc.DepthStencilState.StencilEnable = FALSE;
	geometryPsoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	geometryPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	geometryPsoDesc.NumRenderTargets = static_cast<UINT>(Rendering::Dx12StaticMeshResources::DeferredResources::GBufferCount);
	for (UINT targetIndex = 0; targetIndex < geometryPsoDesc.NumRenderTargets; ++targetIndex)
	{
		geometryPsoDesc.RTVFormats[targetIndex] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
	geometryPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	geometryPsoDesc.SampleDesc.Count = 1;
	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&geometryPsoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Deferred.GeometryPipelineState))))
	{
		return false;
	}

	CD3DX12_DESCRIPTOR_RANGE gbufferRange = {};
	gbufferRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, static_cast<UINT>(Rendering::Dx12StaticMeshResources::DeferredResources::LightingSrvCount), 0);
	CD3DX12_ROOT_PARAMETER lightingRootParameters[5] = {};
	lightingRootParameters[0].InitAsConstantBufferView(0);
	lightingRootParameters[1].InitAsDescriptorTable(1, &gbufferRange, D3D12_SHADER_VISIBILITY_PIXEL);
	lightingRootParameters[2].InitAsShaderResourceView(10, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	lightingRootParameters[3].InitAsShaderResourceView(11, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	lightingRootParameters[4].InitAsShaderResourceView(12, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	CD3DX12_STATIC_SAMPLER_DESC lightingSamplers[2] = {
		CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
		CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)
	};
	lightingSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	lightingSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	lightingSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	lightingSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	lightingSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	lightingSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	lightingSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	lightingSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	CD3DX12_ROOT_SIGNATURE_DESC lightingRootSignatureDesc;
	lightingRootSignatureDesc.Init(static_cast<UINT>(std::size(lightingRootParameters)), lightingRootParameters, static_cast<UINT>(std::size(lightingSamplers)), lightingSamplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> lightingSignature;
	if (FAILED(D3D12SerializeRootSignature(&lightingRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &lightingSignature, &errors)) ||
		FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(0, lightingSignature->GetBufferPointer(), lightingSignature->GetBufferSize(), IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Deferred.LightingRootSignature))))
	{
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPsoDesc = {};
	lightingPsoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.Deferred.LightingRootSignature.Get();
	lightingPsoDesc.VS = { lightingVertexShader->GetBufferPointer(), lightingVertexShader->GetBufferSize() };
	lightingPsoDesc.PS = { lightingPixelShader->GetBufferPointer(), lightingPixelShader->GetBufferSize() };
	lightingPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	lightingPsoDesc.SampleMask = UINT_MAX;
	lightingPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	lightingPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	lightingPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	lightingPsoDesc.DepthStencilState.DepthEnable = FALSE;
	lightingPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	lightingPsoDesc.InputLayout = {};
	lightingPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	lightingPsoDesc.NumRenderTargets = 1;
	lightingPsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	lightingPsoDesc.SampleDesc.Count = 1;
	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&lightingPsoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Deferred.LightingPipelineState))))
	{
		return false;
	}

	CD3DX12_DESCRIPTOR_RANGE hdrRange = {};
	hdrRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
	CD3DX12_ROOT_PARAMETER toneMapRootParameters[2] = {};
	toneMapRootParameters[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	toneMapRootParameters[1].InitAsDescriptorTable(1, &hdrRange, D3D12_SHADER_VISIBILITY_PIXEL);
	CD3DX12_STATIC_SAMPLER_DESC toneMapSamplerDesc(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
	toneMapSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	toneMapSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	toneMapSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	CD3DX12_ROOT_SIGNATURE_DESC toneMapRootSignatureDesc;
	toneMapRootSignatureDesc.Init(static_cast<UINT>(std::size(toneMapRootParameters)), toneMapRootParameters, 1, &toneMapSamplerDesc, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> toneMapSignature;
	if (FAILED(D3D12SerializeRootSignature(&toneMapRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &toneMapSignature, &errors)) ||
		FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(0, toneMapSignature->GetBufferPointer(), toneMapSignature->GetBufferSize(), IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Deferred.ToneMapRootSignature))))
	{
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC toneMapPsoDesc = {};
	toneMapPsoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.Deferred.ToneMapRootSignature.Get();
	toneMapPsoDesc.VS = { toneMapVertexShader->GetBufferPointer(), toneMapVertexShader->GetBufferSize() };
	toneMapPsoDesc.PS = { toneMapPixelShader->GetBufferPointer(), toneMapPixelShader->GetBufferSize() };
	toneMapPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	toneMapPsoDesc.SampleMask = UINT_MAX;
	toneMapPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	toneMapPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	toneMapPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	toneMapPsoDesc.DepthStencilState.DepthEnable = FALSE;
	toneMapPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	toneMapPsoDesc.InputLayout = {};
	toneMapPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	toneMapPsoDesc.NumRenderTargets = 1;
	toneMapPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	toneMapPsoDesc.SampleDesc.Count = 1;
	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&toneMapPsoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Deferred.ToneMapPipelineState))))
	{
		return false;
	}

	return EnsureDx12DeferredResources();
}

bool Engine::CreateDx12ShadowResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!dx12Device)
	{
		return false;
	}

	const std::string shadowShaderSource = ShaderUtils::LoadShaderSource(GetDx12ShadowDepthShaderPath());
	if (shadowShaderSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"DirectX12 Shadow 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3DCompile(shadowShaderSource.c_str(), shadowShaderSource.size(), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &errors)))
	{
		return false;
	}

	CD3DX12_ROOT_PARAMETER rootParameters[1] = {};
	rootParameters[0].InitAsConstantBufferView(0);

	CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
	rootSignatureDesc.Init(static_cast<UINT>(std::size(rootParameters)), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature;
	if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
		FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Shadow.RootSignature))))
	{
		return false;
	}

	static constexpr D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Position)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Normal)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TexCoord)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Color)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, Tangent)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENTSIGN", 0, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(Asset::StaticMeshVertex, TangentSign)), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_StaticMeshRenderer.Dx12.Shadow.RootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.DepthBias = 1000;
	psoDesc.RasterizerState.SlopeScaledDepthBias = 2.0f;
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 0;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	if (FAILED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_StaticMeshRenderer.Dx12.Shadow.PipelineState))))
	{
		return false;
	}

	return EnsureDx12ShadowResources();
}

bool Engine::EnsureDx12ShadowResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!dx12Device)
	{
		return false;
	}

	auto& shadow = m_StaticMeshRenderer.Dx12.Shadow;
	if (!m_ShadowSettings.Enabled)
	{
		shadow.DepthTexture.Reset();
		shadow.DsvHeap.Reset();
		shadow.Size = 0;
		shadow.IsValid = false;
		WriteDx12DeferredShadowSrv();
		return true;
	}

	const uint32_t mapSize = std::clamp(m_ShadowSettings.MapSize, 256u, 8192u);
	if (shadow.IsValid && shadow.Size == mapSize)
	{
		WriteDx12DeferredShadowSrv();
		return true;
	}

	m_Graphics.Device->WaitForGPU();
	shadow.DepthTexture.Reset();
	shadow.DsvHeap.Reset();
	shadow.Size = mapSize;

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&shadow.DsvHeap))))
	{
		shadow.IsValid = false;
		return false;
	}

	const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_CLEAR_VALUE clearValue = {
		.Format = DXGI_FORMAT_D32_FLOAT,
		.DepthStencil = { 1.0f, 0 }
	};
	D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R32_TYPELESS,
		mapSize,
		mapSize,
		1,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	if (FAILED(dx12Device->GetD3DDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&textureDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&shadow.DepthTexture))))
	{
		shadow.IsValid = false;
		return false;
	}

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dx12Device->GetD3DDevice()->CreateDepthStencilView(shadow.DepthTexture.Get(), &dsvDesc, shadow.DsvHeap->GetCPUDescriptorHandleForHeapStart());

	shadow.IsValid = true;
	WriteDx12DeferredShadowSrv();
	return true;
}

void Engine::WriteDx12DeferredShadowSrv()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	if (!dx12Device || !deferred.GBufferSrvHeap)
	{
		return;
	}

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
		deferred.GBufferSrvHeap->GetCPUDescriptorHandleForHeapStart(),
		static_cast<INT>(Rendering::Dx12StaticMeshResources::DeferredResources::ShadowSrvIndex),
		descriptorSize);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	dx12Device->GetD3DDevice()->CreateShaderResourceView(m_StaticMeshRenderer.Dx12.Shadow.DepthTexture.Get(), &srvDesc, srvHandle);
}

void Engine::WriteDx12DeferredHdrSrv()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	if (!dx12Device || !deferred.GBufferSrvHeap)
	{
		return;
	}

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
		deferred.GBufferSrvHeap->GetCPUDescriptorHandleForHeapStart(),
		static_cast<INT>(Rendering::Dx12StaticMeshResources::DeferredResources::HdrSrvIndex),
		descriptorSize);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	dx12Device->GetD3DDevice()->CreateShaderResourceView(deferred.HdrColorTexture.Get(), &srvDesc, srvHandle);
}

bool Engine::EnsureDx12DeferredResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!dx12Device)
	{
		return false;
	}

	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	if (!EnsureDx12ShadowResources())
	{
		return false;
	}

	const uint32_t width = static_cast<uint32_t>((std::max)(m_ClientWidth, 1));
	const uint32_t height = static_cast<uint32_t>((std::max)(m_ClientHeight, 1));
	bool gBuffersValid = true;
	for (const auto& texture : deferred.GBufferTextures)
	{
		gBuffersValid = gBuffersValid && texture != nullptr;
	}

	if (deferred.IsValid &&
		deferred.Width == width &&
		deferred.Height == height &&
		gBuffersValid &&
		deferred.GBufferRtvHeap &&
		deferred.GBufferSrvHeap &&
		deferred.HdrColorTexture &&
		deferred.HdrRtvHeap)
	{
		WriteDx12DeferredShadowSrv();
		WriteDx12DeferredHdrSrv();
		return true;
	}

	m_Graphics.Device->WaitForGPU();
	for (auto& texture : deferred.GBufferTextures)
	{
		texture.Reset();
	}
	deferred.HdrColorTexture.Reset();
	deferred.GBufferRtvHeap.Reset();
	deferred.HdrRtvHeap.Reset();
	deferred.GBufferSrvHeap.Reset();
	deferred.Width = width;
	deferred.Height = height;

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = static_cast<UINT>(Rendering::Dx12StaticMeshResources::DeferredResources::GBufferCount);
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&deferred.GBufferRtvHeap))))
	{
		deferred.IsValid = false;
		return false;
	}
	deferred.RtvDescriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_DESCRIPTOR_HEAP_DESC hdrRtvHeapDesc = {};
	hdrRtvHeapDesc.NumDescriptors = 1;
	hdrRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hdrRtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&hdrRtvHeapDesc, IID_PPV_ARGS(&deferred.HdrRtvHeap))))
	{
		deferred.IsValid = false;
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = static_cast<UINT>(Rendering::Dx12StaticMeshResources::DeferredResources::PostProcessSrvCount);
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&deferred.GBufferSrvHeap))))
	{
		deferred.IsValid = false;
		return false;
	}

	const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_CLEAR_VALUE clearValue = {
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.Color = { 0.0f, 0.0f, 0.0f, 0.0f }
	};
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(deferred.GBufferRtvHeap->GetCPUDescriptorHandleForHeapStart());
	CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(deferred.GBufferSrvHeap->GetCPUDescriptorHandleForHeapStart());
	const UINT srvDescriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (size_t targetIndex = 0; targetIndex < Rendering::Dx12StaticMeshResources::DeferredResources::GBufferCount; ++targetIndex)
	{
		D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			width,
			height,
			1,
			1,
			1,
			0,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		if (FAILED(dx12Device->GetD3DDevice()->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			&clearValue,
			IID_PPV_ARGS(&deferred.GBufferTextures[targetIndex]))))
		{
			deferred.IsValid = false;
			return false;
		}

		dx12Device->GetD3DDevice()->CreateRenderTargetView(deferred.GBufferTextures[targetIndex].Get(), nullptr, rtvHandle);
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		dx12Device->GetD3DDevice()->CreateShaderResourceView(deferred.GBufferTextures[targetIndex].Get(), &srvDesc, srvHandle);

		rtvHandle.Offset(1, deferred.RtvDescriptorSize);
		srvHandle.Offset(1, srvDescriptorSize);
	}

	D3D12_RESOURCE_DESC hdrTextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		width,
		height,
		1,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	if (FAILED(dx12Device->GetD3DDevice()->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&hdrTextureDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(&deferred.HdrColorTexture))))
	{
		deferred.IsValid = false;
		return false;
	}
	dx12Device->GetD3DDevice()->CreateRenderTargetView(deferred.HdrColorTexture.Get(), nullptr, deferred.HdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

	WriteDx12DeferredShadowSrv();
	WriteDx12DeferredHdrSrv();
	deferred.IsValid = true;
	return true;
}

void Engine::DestroyDx12TriangleResources()
{
	DestroyDx12DeferredResources();
	DestroyDx12SkyboxResources();
	DestroyDx12ShadowResources();
	m_StaticMeshRenderer.Dx12.TransparentPipelineState.Reset();
	m_StaticMeshRenderer.Dx12.PipelineState.Reset();
	m_StaticMeshRenderer.Dx12.RootSignature.Reset();
}

void Engine::DestroyDx12SkyboxResources()
{
	m_StaticMeshRenderer.Dx12.SkyboxPipelineState.Reset();
	m_StaticMeshRenderer.Dx12.SkyboxRootSignature.Reset();
}

void Engine::DestroyDx12DeferredResources()
{
	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	for (auto& texture : deferred.GBufferTextures)
	{
		texture.Reset();
	}
	deferred.GBufferRtvHeap.Reset();
	deferred.GBufferSrvHeap.Reset();
	deferred.HdrColorTexture.Reset();
	deferred.HdrRtvHeap.Reset();
	deferred.GeometryPipelineState.Reset();
	deferred.LightingRootSignature.Reset();
	deferred.LightingPipelineState.Reset();
	deferred.ToneMapRootSignature.Reset();
	deferred.ToneMapPipelineState.Reset();
	deferred.Width = 0;
	deferred.Height = 0;
	deferred.RtvDescriptorSize = 0;
	deferred.IsValid = false;
}

void Engine::DestroyDx12ShadowResources()
{
	auto& shadow = m_StaticMeshRenderer.Dx12.Shadow;
	shadow.DepthTexture.Reset();
	shadow.DsvHeap.Reset();
	shadow.RootSignature.Reset();
	shadow.PipelineState.Reset();
	shadow.Size = 0;
	shadow.IsValid = false;
}

void Engine::DrawDx12Skybox(const Editor::ViewportPanelState& viewport, const Camera& camera)
{
	if (!m_SkyboxSettings.Enabled)
	{
		return;
	}

	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	if (!native || !m_StaticMeshRenderer.Dx12.SkyboxRootSignature || !m_StaticMeshRenderer.Dx12.SkyboxPipelineState)
	{
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	if (right <= left || bottom <= top)
	{
		return;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
	backBufferRtv.ptr = reinterpret_cast<SIZE_T>(m_Graphics.Device->GetCurrentBackBufferRTV());
	native->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left), static_cast<float>(bottom - top));
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.SkyboxRootSignature.Get());
	native->SetPipelineState(m_StaticMeshRenderer.Dx12.SkyboxPipelineState.Get());
	const Rendering::SkyboxGpuConstants skyboxConstants = Rendering::BuildSkyboxGpuConstants(m_SkyboxSettings, camera);
	native->SetGraphicsRoot32BitConstants(
		0,
		static_cast<UINT>(sizeof(Rendering::SkyboxGpuConstants) / sizeof(uint32_t)),
		&skyboxConstants,
		0);
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);
}

void Engine::DrawDx12ShadowDepthPass(const Camera& camera)
{
	(void)camera;
	if (!m_ShadowFrameData.Enabled || !m_ShadowFrameData.HasDirectionalCaster || !EnsureDx12ShadowResources())
	{
		return;
	}

	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto& shadow = m_StaticMeshRenderer.Dx12.Shadow;
	if (!native || !cameraResource || !shadow.DepthTexture || !shadow.DsvHeap || !shadow.RootSignature || !shadow.PipelineState)
	{
		return;
	}

	m_Graphics.CommandList->SetViewport(0.0f, 0.0f, static_cast<float>(shadow.Size), static_cast<float>(shadow.Size));
	m_Graphics.CommandList->SetScissorRect(0, 0, static_cast<long>(shadow.Size), static_cast<long>(shadow.Size));
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());

	D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		shadow.DepthTexture.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_DEPTH_WRITE);
	native->ResourceBarrier(1, &barrier);

	const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = shadow.DsvHeap->GetCPUDescriptorHandleForHeapStart();
	native->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);
	native->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	native->SetGraphicsRootSignature(shadow.RootSignature.Get());
	native->SetPipelineState(shadow.PipelineState.Get());
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		const uint64_t cameraOffset = UpdateShadowCameraBuffer(entityId);
		if (cameraOffset == InvalidCameraConstantOffset())
		{
			continue;
		}
		native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + cameraOffset);

		if (meshAsset->Submeshes.empty())
		{
			if (!IsMaterialTransparent(entityId, 0))
			{
				RecordIndexedDraw(Rendering::DrawSubmissionKind::Shadow, static_cast<uint32_t>(meshAsset->Indices.size()), 1);
				m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			}
			continue;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				RecordIndexedDraw(Rendering::DrawSubmissionKind::Shadow, submesh.IndexCount, 1);
				m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
			}
		}
	}

	barrier = CD3DX12_RESOURCE_BARRIER::Transition(
		shadow.DepthTexture.Get(),
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	native->ResourceBarrier(1, &barrier);
}

void Engine::DrawDx12ToneMapPass(const Editor::ViewportPanelState& viewport)
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	if (!native || !dx12Device || !deferred.HdrColorTexture || !deferred.GBufferSrvHeap || !deferred.ToneMapRootSignature || !deferred.ToneMapPipelineState)
	{
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	if (right <= left || bottom <= top)
	{
		return;
	}
	const float width = static_cast<float>(right - left);
	const float height = static_cast<float>(bottom - top);

	D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
	backBufferRtv.ptr = reinterpret_cast<SIZE_T>(m_Graphics.Device->GetCurrentBackBufferRTV());
	native->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	native->SetGraphicsRootSignature(deferred.ToneMapRootSignature.Get());
	native->SetPipelineState(deferred.ToneMapPipelineState.Get());
	ID3D12DescriptorHeap* descriptorHeaps[] = { deferred.GBufferSrvHeap.Get() };
	native->SetDescriptorHeaps(1, descriptorHeaps);
	const DirectX::XMFLOAT4 postProcessConstants = {
		std::clamp(m_Exposure, 0.05f, 8.0f),
		static_cast<float>(static_cast<uint32_t>(m_MaterialDebugView)),
		0.0f,
		0.0f
	};
	native->SetGraphicsRoot32BitConstants(0, 4, &postProcessConstants, 0);
	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_GPU_DESCRIPTOR_HANDLE hdrHandle(
		deferred.GBufferSrvHeap->GetGPUDescriptorHandleForHeapStart(),
		static_cast<INT>(Rendering::Dx12StaticMeshResources::DeferredResources::HdrSrvIndex),
		descriptorSize);
	native->SetGraphicsRootDescriptorTable(1, hdrHandle);
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);
}

void Engine::DrawDx12DeferredTriangle(const Editor::ViewportPanelState& viewport, const Camera& camera, const DeferredPassTimingIndices& timings)
{
	if (!EnsureDx12DeferredResources())
	{
		DrawDx12Triangle(camera);
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	if (right <= left || bottom <= top)
	{
		return;
	}
	const float width = static_cast<float>(right - left);
	const float height = static_cast<float>(bottom - top);

	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto lightResource = m_StaticMeshRenderer.DeferredLightBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredLightBuffer->GetNativeResource()) : nullptr;
	auto lightingResource = m_StaticMeshRenderer.DeferredLightingBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredLightingBuffer->GetNativeResource()) : nullptr;
	auto tileRangeResource = m_StaticMeshRenderer.DeferredTileRangeBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredTileRangeBuffer->GetNativeResource()) : nullptr;
	auto tileIndexResource = m_StaticMeshRenderer.DeferredTileLightIndexBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredTileLightIndexBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	auto& deferred = m_StaticMeshRenderer.Dx12.Deferred;
	if (!native || !cameraResource || !lightResource || !lightingResource || !dx12Device ||
		!deferred.GeometryPipelineState || !deferred.LightingRootSignature || !deferred.LightingPipelineState ||
		!deferred.ToneMapRootSignature || !deferred.ToneMapPipelineState ||
		!deferred.GBufferRtvHeap || !deferred.HdrRtvHeap || !deferred.GBufferSrvHeap || !deferred.HdrColorTexture)
	{
		DrawDx12Triangle(camera);
		return;
	}

	const auto geometryBegin = std::chrono::steady_clock::now();
	std::array<D3D12_RESOURCE_BARRIER, Rendering::Dx12StaticMeshResources::DeferredResources::GBufferCount> barriers = {};
	for (size_t targetIndex = 0; targetIndex < barriers.size(); ++targetIndex)
	{
		barriers[targetIndex] = CD3DX12_RESOURCE_BARRIER::Transition(
			deferred.GBufferTextures[targetIndex].Get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	native->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, Rendering::Dx12StaticMeshResources::DeferredResources::GBufferCount> rtvHandles = {};
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(deferred.GBufferRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (size_t targetIndex = 0; targetIndex < rtvHandles.size(); ++targetIndex)
	{
		rtvHandles[targetIndex] = rtvHandle;
		rtvHandle.Offset(1, deferred.RtvDescriptorSize);
	}
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
	dsvHandle.ptr = reinterpret_cast<SIZE_T>(m_Graphics.Device->GetDepthStencilView());
	native->OMSetRenderTargets(static_cast<UINT>(rtvHandles.size()), rtvHandles.data(), FALSE, &dsvHandle);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	const float gbufferClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (D3D12_CPU_DESCRIPTOR_HANDLE handle : rtvHandles)
	{
		native->ClearRenderTargetView(handle, gbufferClear, 0, nullptr);
	}
	native->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	native->SetGraphicsRootShaderResourceView(2, lightResource->GetGPUVirtualAddress());
	native->SetPipelineState(deferred.GeometryPipelineState.Get());
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		ID3D12DescriptorHeap* selectedHeap = m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Get();
		size_t selectedMaterialCount = m_StaticMeshRenderer.Dx12.MaterialCount;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Dx12.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Dx12.EntityMaterials.end()
			&& entityMaterialIt->second.ShaderResourceHeap
			&& !entityMaterialIt->second.MaterialTextures.empty())
		{
			selectedHeap = entityMaterialIt->second.ShaderResourceHeap.Get();
			selectedMaterialCount = entityMaterialIt->second.MaterialCount;
		}
		if (!selectedHeap || selectedMaterialCount == 0)
		{
			continue;
		}

		ID3D12DescriptorHeap* descriptorHeaps[] = { selectedHeap };
		native->SetDescriptorHeaps(1, descriptorHeaps);
		const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = selectedHeap->GetGPUDescriptorHandleForHeapStart();

		auto drawOpaqueMaterial = [&](uint32_t indexCount, uint32_t indexOffset, size_t materialIndex)
		{
			const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, false);
			if (cameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + cameraOffset);
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex * MaterialSlotCount();
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::DeferredGeometry, indexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(indexCount, 1, indexOffset, 0, 0);
		};

		if (meshAsset->Submeshes.empty())
		{
			if (!IsMaterialTransparent(entityId, 0))
			{
				drawOpaqueMaterial(static_cast<uint32_t>(meshAsset->Indices.size()), 0, 0);
			}
			continue;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				const size_t materialIndex = submesh.MaterialIndex < selectedMaterialCount ? submesh.MaterialIndex : 0;
				drawOpaqueMaterial(submesh.IndexCount, submesh.IndexOffset, materialIndex);
			}
		}
	}

	for (size_t targetIndex = 0; targetIndex < barriers.size(); ++targetIndex)
	{
		barriers[targetIndex] = CD3DX12_RESOURCE_BARRIER::Transition(
			deferred.GBufferTextures[targetIndex].Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	native->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

	const auto geometryEnd = std::chrono::steady_clock::now();
	m_RenderGraph.SetPassCpuTime(timings.Geometry, std::chrono::duration<double, std::milli>(geometryEnd - geometryBegin).count());

	MeasureRenderGraphPass(m_RenderGraph, timings.TileCulling, [this, &camera, left, top, right, bottom]()
		{
			static_cast<void>(UpdateDeferredLightTiles(
				camera,
				static_cast<uint32_t>((std::max)(left, 0L)),
				static_cast<uint32_t>((std::max)(top, 0L)),
				static_cast<uint32_t>((std::max)(right - left, 1L)),
				static_cast<uint32_t>((std::max)(bottom - top, 1L)),
				static_cast<uint32_t>((std::max)(m_ClientWidth, 1)),
				static_cast<uint32_t>((std::max)(m_ClientHeight, 1))));
		});
	tileRangeResource = m_StaticMeshRenderer.DeferredTileRangeBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredTileRangeBuffer->GetNativeResource()) : nullptr;
	tileIndexResource = m_StaticMeshRenderer.DeferredTileLightIndexBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredTileLightIndexBuffer->GetNativeResource()) : nullptr;
	if (!tileRangeResource || !tileIndexResource)
	{
		return;
	}

	const auto lightingBegin = std::chrono::steady_clock::now();
	const auto cameraPosition = camera.GetPosition();
	DeferredLightingConstants lightingConstants = {};
	lightingConstants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f };
	lightingConstants.AmbientColorIntensity = {
		std::clamp(m_AmbientColor.x, 0.0f, 4.0f),
		std::clamp(m_AmbientColor.y, 0.0f, 4.0f),
		std::clamp(m_AmbientColor.z, 0.0f, 4.0f),
		std::clamp(m_AmbientIntensity, 0.0f, 2.0f)
	};
	lightingConstants.ExposureDebug = {
		std::clamp(m_Exposure, 0.05f, 8.0f),
		static_cast<float>(static_cast<uint32_t>(m_MaterialDebugView)),
		0.0f,
		0.0f
	};
	lightingConstants.LightCountParams = {
		static_cast<float>(m_StaticMeshRenderer.DeferredLightCount),
		static_cast<float>(m_StaticMeshRenderer.DeferredTileCountX),
		static_cast<float>(m_StaticMeshRenderer.DeferredTileCountY),
		m_StaticMeshRenderer.DeferredTileCountX > 0 && m_StaticMeshRenderer.DeferredTileCountY > 0 ? 1.0f : 0.0f
	};
	lightingConstants.ScreenSize = {
		static_cast<float>((std::max)(m_ClientWidth, 1)),
		static_cast<float>((std::max)(m_ClientHeight, 1)),
		1.0f / static_cast<float>((std::max)(m_ClientWidth, 1)),
		1.0f / static_cast<float>((std::max)(m_ClientHeight, 1))
	};
	Rendering::ShadowFrameData shadowData = m_ShadowFrameData;
	if (!m_StaticMeshRenderer.Dx12.Shadow.IsValid)
	{
		shadowData.Params.x = 0.0f;
	}
	lightingConstants.ShadowViewProjection = shadowData.LightViewProjection;
	lightingConstants.ShadowParams = shadowData.Params;
	lightingConstants.ShadowDirection = shadowData.DirectionToLight;
	lightingConstants.Skybox = Rendering::BuildSkyboxGpuConstants(m_SkyboxSettings, camera);
	if (WriteDeferredLightingConstants(lightingConstants) == InvalidCameraConstantOffset())
	{
		return;
	}

	D3D12_RESOURCE_BARRIER hdrBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		deferred.HdrColorTexture.Get(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	native->ResourceBarrier(1, &hdrBarrier);

	const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = deferred.HdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
	native->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	const std::array<float, 4> hdrClear = BuildSkyClearColor(m_SkyboxSettings);
	native->ClearRenderTargetView(hdrRtv, hdrClear.data(), 0, nullptr);
	native->SetGraphicsRootSignature(deferred.LightingRootSignature.Get());
	native->SetPipelineState(deferred.LightingPipelineState.Get());
	ID3D12DescriptorHeap* gbufferHeap[] = { deferred.GBufferSrvHeap.Get() };
	native->SetDescriptorHeaps(1, gbufferHeap);
	native->SetGraphicsRootConstantBufferView(0, lightingResource->GetGPUVirtualAddress());
	native->SetGraphicsRootDescriptorTable(1, deferred.GBufferSrvHeap->GetGPUDescriptorHandleForHeapStart());
	native->SetGraphicsRootShaderResourceView(2, lightResource->GetGPUVirtualAddress());
	native->SetGraphicsRootShaderResourceView(3, tileRangeResource->GetGPUVirtualAddress());
	native->SetGraphicsRootShaderResourceView(4, tileIndexResource->GetGPUVirtualAddress());
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);

	hdrBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
		deferred.HdrColorTexture.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	native->ResourceBarrier(1, &hdrBarrier);

	const auto lightingEnd = std::chrono::steady_clock::now();
	m_RenderGraph.SetPassCpuTime(timings.Lighting, std::chrono::duration<double, std::milli>(lightingEnd - lightingBegin).count());

	MeasureRenderGraphPass(m_RenderGraph, timings.PostProcess, [this, &viewport]()
		{
			DrawDx12ToneMapPass(viewport);
		});

	MeasureRenderGraphPass(m_RenderGraph, timings.Transparency, [this, native, dsvHandle, &camera, left, top, right, bottom, width, height]()
		{
			D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = {};
			backBufferRtv.ptr = reinterpret_cast<SIZE_T>(m_Graphics.Device->GetCurrentBackBufferRTV());
			native->OMSetRenderTargets(1, &backBufferRtv, FALSE, &dsvHandle);
			m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
			m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
			DrawDx12ForwardTransparentPass(camera);
		});
}

void Engine::DrawDx12ForwardTransparentPass(const Camera& camera)
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto lightResource = m_StaticMeshRenderer.DeferredLightBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredLightBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!native || !cameraResource || !lightResource || !dx12Device || !m_StaticMeshRenderer.Dx12.TransparentPipelineState || !m_StaticMeshRenderer.Dx12.RootSignature)
	{
		return;
	}

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	native->SetGraphicsRootShaderResourceView(2, lightResource->GetGPUVirtualAddress());
	native->SetPipelineState(m_StaticMeshRenderer.Dx12.TransparentPipelineState.Get());
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		ID3D12DescriptorHeap* selectedHeap = m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Get();
		size_t selectedMaterialCount = m_StaticMeshRenderer.Dx12.MaterialCount;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Dx12.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Dx12.EntityMaterials.end()
			&& entityMaterialIt->second.ShaderResourceHeap
			&& !entityMaterialIt->second.MaterialTextures.empty())
		{
			selectedHeap = entityMaterialIt->second.ShaderResourceHeap.Get();
			selectedMaterialCount = entityMaterialIt->second.MaterialCount;
		}
		if (!selectedHeap || selectedMaterialCount == 0)
		{
			continue;
		}

		ID3D12DescriptorHeap* descriptorHeaps[] = { selectedHeap };
		native->SetDescriptorHeaps(1, descriptorHeaps);
		const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = selectedHeap->GetGPUDescriptorHandleForHeapStart();
		UploadEntityGeometry(entityId);

		auto drawTransparentMaterial = [&](uint32_t indexCount, uint32_t indexOffset, size_t materialIndex)
		{
			const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, false);
			if (cameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + cameraOffset);
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex * MaterialSlotCount();
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Transparent, indexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(indexCount, 1, indexOffset, 0, 0);
		};

		if (meshAsset->Submeshes.empty())
		{
			if (IsMaterialTransparent(entityId, 0))
			{
				drawTransparentMaterial(static_cast<uint32_t>(meshAsset->Indices.size()), 0, 0);
			}
			continue;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				const size_t materialIndex = submesh.MaterialIndex < selectedMaterialCount ? submesh.MaterialIndex : 0;
				drawTransparentMaterial(submesh.IndexCount, submesh.IndexOffset, materialIndex);
			}
		}
	}
}

void Engine::DrawDx12Triangle(const Camera& camera)
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_Graphics.CommandList->GetNativeResource());
	auto cameraResource = m_StaticMeshRenderer.CameraBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.CameraBuffer->GetNativeResource()) : nullptr;
	auto lightResource = m_StaticMeshRenderer.DeferredLightBuffer ? static_cast<ID3D12Resource*>(m_StaticMeshRenderer.DeferredLightBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Graphics.Device.get());
	if (!native || !cameraResource || !lightResource || !dx12Device || !m_StaticMeshRenderer.Dx12.PipelineState || !m_StaticMeshRenderer.Dx12.RootSignature)
	{
		return;
	}

	native->SetGraphicsRootSignature(m_StaticMeshRenderer.Dx12.RootSignature.Get());
	if (!m_StaticMeshRenderer.Dx12.ShaderResourceHeap || m_StaticMeshRenderer.Dx12.MaterialTextures.empty())
	{
		return;
	}

	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	native->SetGraphicsRootShaderResourceView(2, lightResource->GetGPUVirtualAddress());

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const bool drawTransparentInSecondPass = m_StaticMeshRenderer.Dx12.TransparentPipelineState != nullptr;
	const bool drawOpaquePass = true;
	if (drawOpaquePass)
	{
		native->SetPipelineState(m_StaticMeshRenderer.Dx12.PipelineState.Get());
	}

	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		// 공용 upload buffer를 재사용하므로,
		// entity마다 geometry와 camera 상수를 갱신한 뒤 draw를 이어서 기록합니다.
		UploadEntityGeometry(entityId);
		const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera);
		if (cameraOffset == InvalidCameraConstantOffset())
		{
			continue;
		}
		native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + cameraOffset);

		ID3D12DescriptorHeap* selectedHeap = m_StaticMeshRenderer.Dx12.ShaderResourceHeap.Get();
		size_t selectedMaterialCount = m_StaticMeshRenderer.Dx12.MaterialCount;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Dx12.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Dx12.EntityMaterials.end()
			&& entityMaterialIt->second.ShaderResourceHeap
			&& !entityMaterialIt->second.MaterialTextures.empty())
		{
			selectedHeap = entityMaterialIt->second.ShaderResourceHeap.Get();
			selectedMaterialCount = entityMaterialIt->second.MaterialCount;
		}
		if (!selectedHeap || selectedMaterialCount == 0)
		{
			continue;
		}

		ID3D12DescriptorHeap* descriptorHeaps[] = { selectedHeap };
		native->SetDescriptorHeaps(1, descriptorHeaps);
		const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = selectedHeap->GetGPUDescriptorHandleForHeapStart();

		if (meshAsset->Submeshes.empty())
		{
			const bool entityIsTransparent = IsMaterialTransparent(entityId, 0);
			if ((entityIsTransparent && !drawTransparentInSecondPass) || (!entityIsTransparent && !drawOpaquePass))
			{
				continue;
			}

			if (entityIsTransparent)
			{
				const uint64_t transparentCameraOffset = UpdateCameraBuffer(entityId, camera, 0, false);
				if (transparentCameraOffset == InvalidCameraConstantOffset())
				{
					continue;
				}
				native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + transparentCameraOffset);
			}

			native->SetPipelineState(entityIsTransparent ? m_StaticMeshRenderer.Dx12.TransparentPipelineState.Get() : m_StaticMeshRenderer.Dx12.PipelineState.Get());
			native->SetGraphicsRootDescriptorTable(1, baseHandle);
			RecordIndexedDraw(entityIsTransparent ? Rendering::DrawSubmissionKind::Transparent : Rendering::DrawSubmissionKind::Opaque, static_cast<uint32_t>(meshAsset->Indices.size()), 1);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			continue;
		}

		auto drawSubmesh = [&](const Asset::StaticMeshSubmesh& submesh, bool useDeferredLighting)
		{
			const size_t materialIndex = submesh.MaterialIndex < selectedMaterialCount ? submesh.MaterialIndex : 0;
			const uint64_t materialCameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, useDeferredLighting);
			if (materialCameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress() + materialCameraOffset);
			D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
			materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex * MaterialSlotCount();
			native->SetGraphicsRootDescriptorTable(1, materialHandle);
			RecordIndexedDraw(IsMaterialTransparent(entityId, materialIndex) ? Rendering::DrawSubmissionKind::Transparent : Rendering::DrawSubmissionKind::Opaque, submesh.IndexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
		};

		if (drawOpaquePass)
		{
			native->SetPipelineState(m_StaticMeshRenderer.Dx12.PipelineState.Get());
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh, m_RenderMode == RenderMode::Deferred);
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
					drawSubmesh(submesh, false);
				}
			}
		}
	}
}

bool Engine::CreateCameraBuffer()
{
	uint64_t alignment = 256;
	if (auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get()))
	{
		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(vulkanDevice->GetVkPhysicalDevice(), &properties);
		alignment = (std::max)(alignment, static_cast<uint64_t>(properties.limits.minUniformBufferOffsetAlignment));
	}

	m_StaticMeshRenderer.CameraBufferStride = AlignUp(static_cast<uint64_t>(sizeof(CameraConstants)), alignment);
	m_StaticMeshRenderer.CameraBufferCapacity = CameraConstantSlotCount();
	m_StaticMeshRenderer.CameraBufferCursor = 0;
	const uint64_t cameraBufferSize = m_StaticMeshRenderer.CameraBufferStride * m_StaticMeshRenderer.CameraBufferCapacity;

	const BufferDesc bufferDesc = {
		.Size = cameraBufferSize,
		.Stride = static_cast<uint32_t>(sizeof(CameraConstants)),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_StaticMeshRenderer.CameraBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
	return m_StaticMeshRenderer.CameraBuffer != nullptr;
}

bool Engine::CreateDeferredLightBuffer(uint32_t capacity)
{
	if (!m_Graphics.Device)
	{
		return false;
	}

	if (!m_StaticMeshRenderer.DeferredLightingBuffer)
	{
		const BufferDesc lightingBufferDesc = {
			.Size = sizeof(DeferredLightingConstants),
			.Stride = static_cast<uint32_t>(sizeof(DeferredLightingConstants)),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.DeferredLightingBuffer = m_Graphics.Device->CreateBuffer(lightingBufferDesc);
		if (!m_StaticMeshRenderer.DeferredLightingBuffer)
		{
			return false;
		}
	}

	const uint32_t clampedCapacity = (std::max)(capacity, 1u);
	const BufferDesc bufferDesc = {
		.Size = static_cast<uint64_t>(clampedCapacity) * sizeof(LightGpuData),
		.Stride = static_cast<uint32_t>(sizeof(LightGpuData)),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_StaticMeshRenderer.DeferredLightBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
	if (!m_StaticMeshRenderer.DeferredLightBuffer)
	{
		m_StaticMeshRenderer.DeferredLightBufferCapacity = 0;
		m_StaticMeshRenderer.DeferredLightCount = 0;
		return false;
	}

	m_StaticMeshRenderer.DeferredLightBufferCapacity = clampedCapacity;
	m_StaticMeshRenderer.DeferredLightCount = 0;
	if (!EnsureDeferredTileLightBufferCapacity(1, 1))
	{
		return false;
	}
	return RefreshVulkanDeferredLightBufferDescriptors();
}

uint64_t Engine::WriteDeferredLightingConstants(const DeferredLightingConstants& lightingConstants)
{
	if (!m_StaticMeshRenderer.DeferredLightingBuffer)
	{
		return InvalidCameraConstantOffset();
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.DeferredLightingBuffer->Map(&mappedData);
	if (!mappedData)
	{
		return InvalidCameraConstantOffset();
	}

	std::memcpy(mappedData, &lightingConstants, sizeof(lightingConstants));
	m_StaticMeshRenderer.DeferredLightingBuffer->Unmap();
	return 0;
}

bool Engine::EnsureDeferredLightBufferCapacity(uint32_t lightCount)
{
	const uint32_t requiredCount = (std::max)(lightCount, 1u);
	if (m_StaticMeshRenderer.DeferredLightBuffer && m_StaticMeshRenderer.DeferredLightBufferCapacity >= requiredCount)
	{
		return true;
	}

	uint32_t newCapacity = (std::max)(m_StaticMeshRenderer.DeferredLightBufferCapacity, kInitialDeferredLightBufferCapacity);
	while (newCapacity < requiredCount)
	{
		newCapacity *= 2;
	}
	if (m_Graphics.Device)
	{
		m_Graphics.Device->WaitForGPU();
	}
	return CreateDeferredLightBuffer(newCapacity);
}

bool Engine::EnsureDeferredTileLightBufferCapacity(uint32_t tileCount, uint32_t lightReferenceCount)
{
	if (!m_Graphics.Device)
	{
		return false;
	}

	const uint32_t requiredTileCount = (std::max)(tileCount, 1u);
	const uint32_t requiredReferenceCount = (std::max)(lightReferenceCount, 1u);
	const bool hasEnoughRangeCapacity =
		m_StaticMeshRenderer.DeferredTileRangeBuffer &&
		m_StaticMeshRenderer.DeferredTileRangeCapacity >= requiredTileCount;
	const bool hasEnoughIndexCapacity =
		m_StaticMeshRenderer.DeferredTileLightIndexBuffer &&
		m_StaticMeshRenderer.DeferredTileLightIndexCapacity >= requiredReferenceCount;
	if (hasEnoughRangeCapacity && hasEnoughIndexCapacity)
	{
		return true;
	}

	if (m_Graphics.Device)
	{
		m_Graphics.Device->WaitForGPU();
	}

	if (!hasEnoughRangeCapacity)
	{
		uint32_t newRangeCapacity = (std::max)(m_StaticMeshRenderer.DeferredTileRangeCapacity, 64u);
		while (newRangeCapacity < requiredTileCount)
		{
			newRangeCapacity *= 2;
		}
		const BufferDesc rangeBufferDesc = {
			.Size = static_cast<uint64_t>(newRangeCapacity) * sizeof(DeferredTileLightRange),
			.Stride = static_cast<uint32_t>(sizeof(DeferredTileLightRange)),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.DeferredTileRangeBuffer = m_Graphics.Device->CreateBuffer(rangeBufferDesc);
		if (!m_StaticMeshRenderer.DeferredTileRangeBuffer)
		{
			m_StaticMeshRenderer.DeferredTileRangeCapacity = 0;
			return false;
		}
		m_StaticMeshRenderer.DeferredTileRangeCapacity = newRangeCapacity;
	}

	if (!hasEnoughIndexCapacity)
	{
		uint32_t newIndexCapacity = (std::max)(m_StaticMeshRenderer.DeferredTileLightIndexCapacity, 256u);
		while (newIndexCapacity < requiredReferenceCount)
		{
			newIndexCapacity *= 2;
		}
		const BufferDesc indexBufferDesc = {
			.Size = static_cast<uint64_t>(newIndexCapacity) * sizeof(uint32_t),
			.Stride = static_cast<uint32_t>(sizeof(uint32_t)),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.DeferredTileLightIndexBuffer = m_Graphics.Device->CreateBuffer(indexBufferDesc);
		if (!m_StaticMeshRenderer.DeferredTileLightIndexBuffer)
		{
			m_StaticMeshRenderer.DeferredTileLightIndexCapacity = 0;
			return false;
		}
		m_StaticMeshRenderer.DeferredTileLightIndexCapacity = newIndexCapacity;
	}

	return RefreshVulkanDeferredLightBufferDescriptors();
}

uint32_t Engine::UpdateDeferredLightBuffer()
{
	if (m_RenderMode != RenderMode::Deferred)
	{
		m_StaticMeshRenderer.DeferredLightCount = 0;
		m_StaticMeshRenderer.DeferredCpuLights.clear();
		m_StaticMeshRenderer.DeferredTileCountX = 0;
		m_StaticMeshRenderer.DeferredTileCountY = 0;
		m_StaticMeshRenderer.DeferredTileLightReferenceCount = 0;
		m_StaticMeshRenderer.DeferredMaxTileLightCount = 0;
		m_StaticMeshRenderer.DeferredFullTileLightCount = 0;
		return 0;
	}

	const std::vector<LightGpuData> lights = RenderSystem::CollectSceneLights(GetRuntimeScene(), ResolveKeyLightEntity(), kUnlimitedDeferredGpuLights);
	const uint32_t lightCount = static_cast<uint32_t>((std::min)(lights.size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
	if (!EnsureDeferredLightBufferCapacity(lightCount))
	{
		m_StaticMeshRenderer.DeferredLightCount = 0;
		m_StaticMeshRenderer.DeferredTileCountX = 0;
		m_StaticMeshRenderer.DeferredTileCountY = 0;
		m_StaticMeshRenderer.DeferredTileLightReferenceCount = 0;
		m_StaticMeshRenderer.DeferredMaxTileLightCount = 0;
		m_StaticMeshRenderer.DeferredFullTileLightCount = 0;
		return 0;
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.DeferredLightBuffer->Map(&mappedData);
	if (!mappedData)
	{
		m_StaticMeshRenderer.DeferredLightCount = 0;
		m_StaticMeshRenderer.DeferredTileCountX = 0;
		m_StaticMeshRenderer.DeferredTileCountY = 0;
		m_StaticMeshRenderer.DeferredTileLightReferenceCount = 0;
		m_StaticMeshRenderer.DeferredMaxTileLightCount = 0;
		m_StaticMeshRenderer.DeferredFullTileLightCount = 0;
		return 0;
	}

	std::memcpy(mappedData, lights.data(), static_cast<size_t>(lightCount) * sizeof(LightGpuData));
	m_StaticMeshRenderer.DeferredLightBuffer->Unmap();
	m_StaticMeshRenderer.DeferredLightCount = lightCount;
	m_StaticMeshRenderer.DeferredCpuLights = lights;
	return lightCount;
}

bool Engine::UpdateDeferredLightTiles(
	const Camera& camera,
	uint32_t viewportLeft,
	uint32_t viewportTop,
	uint32_t viewportWidth,
	uint32_t viewportHeight,
	uint32_t screenWidth,
	uint32_t screenHeight)
{
	auto resetDeferredTileStats = [this]()
	{
		m_StaticMeshRenderer.DeferredTileCountX = 0;
		m_StaticMeshRenderer.DeferredTileCountY = 0;
		m_StaticMeshRenderer.DeferredTileLightReferenceCount = 0;
		m_StaticMeshRenderer.DeferredMaxTileLightCount = 0;
		m_StaticMeshRenderer.DeferredFullTileLightCount = 0;
	};

	if (m_RenderMode != RenderMode::Deferred || m_StaticMeshRenderer.DeferredCpuLights.empty())
	{
		resetDeferredTileStats();
		return false;
	}

	viewportWidth = (std::max)(viewportWidth, 1u);
	viewportHeight = (std::max)(viewportHeight, 1u);
	const uint32_t tileCountX = (std::max)(1u, (screenWidth + kDeferredLightTileSize - 1u) / kDeferredLightTileSize);
	const uint32_t tileCountY = (std::max)(1u, (screenHeight + kDeferredLightTileSize - 1u) / kDeferredLightTileSize);
	const uint32_t tileCount = tileCountX * tileCountY;
	std::vector<std::vector<uint32_t>> perTileLightIndices(tileCount);

	auto appendLightToTile = [&](uint32_t tileIndex, uint32_t lightIndex)
	{
		if (tileIndex < perTileLightIndices.size())
		{
			perTileLightIndices[tileIndex].push_back(lightIndex);
		}
	};

	auto appendLightRect = [&](uint32_t lightIndex, int minTileX, int minTileY, int maxTileX, int maxTileY)
	{
		const int clampedMinX = std::clamp(minTileX, 0, static_cast<int>(tileCountX) - 1);
		const int clampedMaxX = std::clamp(maxTileX, 0, static_cast<int>(tileCountX) - 1);
		const int clampedMinY = std::clamp(minTileY, 0, static_cast<int>(tileCountY) - 1);
		const int clampedMaxY = std::clamp(maxTileY, 0, static_cast<int>(tileCountY) - 1);
		if (clampedMaxX < clampedMinX || clampedMaxY < clampedMinY)
		{
			return;
		}
		for (int tileY = clampedMinY; tileY <= clampedMaxY; ++tileY)
		{
			for (int tileX = clampedMinX; tileX <= clampedMaxX; ++tileX)
			{
				appendLightToTile(static_cast<uint32_t>(tileY) * tileCountX + static_cast<uint32_t>(tileX), lightIndex);
			}
		}
	};

	uint32_t fullTileLightCount = 0;
	auto appendLightToAllTiles = [&](uint32_t lightIndex)
	{
		appendLightRect(lightIndex, 0, 0, static_cast<int>(tileCountX) - 1, static_cast<int>(tileCountY) - 1);
		++fullTileLightCount;
	};

	const DirectX::XMMATRIX viewProjection = camera.GetViewProjectionMatrix();
	const DirectX::XMFLOAT3 cameraPosition = camera.GetPosition();
	const DirectX::XMFLOAT3 cameraForward = camera.GetForward();
	const DirectX::XMVECTOR cameraPositionVector = DirectX::XMLoadFloat3(&cameraPosition);
	const DirectX::XMVECTOR cameraForwardVector = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&cameraForward));
	const float nearZ = (std::max)(camera.GetNearZ(), 0.001f);
	const float projectionScale = static_cast<float>(viewportHeight) / (2.0f * std::tan((std::max)(camera.GetFovY(), 0.001f) * 0.5f));

	for (uint32_t lightIndex = 0; lightIndex < m_StaticMeshRenderer.DeferredCpuLights.size(); ++lightIndex)
	{
		const LightGpuData& light = m_StaticMeshRenderer.DeferredCpuLights[lightIndex];
		if (light.PositionType.w <= 0.5f)
		{
			appendLightToAllTiles(lightIndex);
			continue;
		}

		const DirectX::XMVECTOR lightPosition = DirectX::XMVectorSet(light.PositionType.x, light.PositionType.y, light.PositionType.z, 1.0f);
		const float range = (std::max)(light.DirectionRange.w, 0.001f);
		const DirectX::XMVECTOR cameraToLight = DirectX::XMVectorSubtract(lightPosition, cameraPositionVector);
		const float centerDepth = DirectX::XMVectorGetX(DirectX::XMVector3Dot(cameraToLight, cameraForwardVector));
		const DirectX::XMVECTOR clipPosition = DirectX::XMVector4Transform(lightPosition, viewProjection);
		const float clipW = DirectX::XMVectorGetW(clipPosition);
		if (clipW <= 0.001f)
		{
			if (centerDepth + range >= nearZ)
			{
				appendLightToAllTiles(lightIndex);
			}
			continue;
		}

		const float ndcX = DirectX::XMVectorGetX(clipPosition) / clipW;
		const float ndcY = DirectX::XMVectorGetY(clipPosition) / clipW;
		const float screenX = static_cast<float>(viewportLeft) + (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
		const float screenY = static_cast<float>(viewportTop) + (-ndcY * 0.5f + 0.5f) * static_cast<float>(viewportHeight);
		const float distanceToCamera = (std::max)(
			DirectX::XMVectorGetX(DirectX::XMVector3Length(cameraToLight)),
			0.001f);
		const float radiusPixels = std::clamp(range * projectionScale / distanceToCamera, 1.0f, static_cast<float>((std::max)(viewportWidth, viewportHeight)));
		if (screenX + radiusPixels < 0.0f ||
			screenY + radiusPixels < 0.0f ||
			screenX - radiusPixels > static_cast<float>(screenWidth) ||
			screenY - radiusPixels > static_cast<float>(screenHeight))
		{
			continue;
		}

		appendLightRect(
			lightIndex,
			static_cast<int>(std::floor((screenX - radiusPixels) / static_cast<float>(kDeferredLightTileSize))),
			static_cast<int>(std::floor((screenY - radiusPixels) / static_cast<float>(kDeferredLightTileSize))),
			static_cast<int>(std::floor((screenX + radiusPixels) / static_cast<float>(kDeferredLightTileSize))),
			static_cast<int>(std::floor((screenY + radiusPixels) / static_cast<float>(kDeferredLightTileSize))));
	}

	std::vector<DeferredTileLightRange> ranges(tileCount);
	std::vector<uint32_t> lightIndices;
	uint64_t actualLightReferenceCount = 0;
	uint32_t maxTileLightCount = 0;
	for (uint32_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
	{
		ranges[tileIndex].Offset = static_cast<uint32_t>(lightIndices.size());
		ranges[tileIndex].Count = static_cast<uint32_t>((std::min)(perTileLightIndices[tileIndex].size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
		actualLightReferenceCount = (std::min)(
			actualLightReferenceCount + static_cast<uint64_t>(ranges[tileIndex].Count),
			static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()));
		maxTileLightCount = (std::max)(maxTileLightCount, ranges[tileIndex].Count);
		lightIndices.insert(lightIndices.end(), perTileLightIndices[tileIndex].begin(), perTileLightIndices[tileIndex].end());
	}
	if (lightIndices.empty())
	{
		lightIndices.push_back(0);
	}

	if (!EnsureDeferredTileLightBufferCapacity(static_cast<uint32_t>(ranges.size()), static_cast<uint32_t>(lightIndices.size())))
	{
		resetDeferredTileStats();
		return false;
	}

	void* mappedRanges = nullptr;
	m_StaticMeshRenderer.DeferredTileRangeBuffer->Map(&mappedRanges);
	if (!mappedRanges)
	{
		resetDeferredTileStats();
		return false;
	}
	std::memcpy(mappedRanges, ranges.data(), ranges.size() * sizeof(DeferredTileLightRange));
	m_StaticMeshRenderer.DeferredTileRangeBuffer->Unmap();

	void* mappedIndices = nullptr;
	m_StaticMeshRenderer.DeferredTileLightIndexBuffer->Map(&mappedIndices);
	if (!mappedIndices)
	{
		resetDeferredTileStats();
		return false;
	}
	std::memcpy(mappedIndices, lightIndices.data(), lightIndices.size() * sizeof(uint32_t));
	m_StaticMeshRenderer.DeferredTileLightIndexBuffer->Unmap();

	m_StaticMeshRenderer.DeferredTileCountX = tileCountX;
	m_StaticMeshRenderer.DeferredTileCountY = tileCountY;
	m_StaticMeshRenderer.DeferredTileLightReferenceCount = static_cast<uint32_t>(actualLightReferenceCount);
	m_StaticMeshRenderer.DeferredMaxTileLightCount = maxTileLightCount;
	m_StaticMeshRenderer.DeferredFullTileLightCount = fullTileLightCount;
	++m_RenderFrameStats.DeferredTileViewportCount;
	m_RenderFrameStats.DeferredTileCountTotal = static_cast<uint32_t>((std::min)(
		static_cast<uint64_t>(m_RenderFrameStats.DeferredTileCountTotal) + static_cast<uint64_t>(tileCount),
		static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
	m_RenderFrameStats.DeferredTileLightReferenceCount = static_cast<uint32_t>((std::min)(
		static_cast<uint64_t>(m_RenderFrameStats.DeferredTileLightReferenceCount) + actualLightReferenceCount,
		static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
	m_RenderFrameStats.DeferredMaxTileLightCount = (std::max)(m_RenderFrameStats.DeferredMaxTileLightCount, maxTileLightCount);
	m_RenderFrameStats.DeferredFullTileLightCount = static_cast<uint32_t>((std::min)(
		static_cast<uint64_t>(m_RenderFrameStats.DeferredFullTileLightCount) + static_cast<uint64_t>(fullTileLightCount),
		static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
	return true;
}

void Engine::ResetCameraConstantAllocator() noexcept
{
	m_StaticMeshRenderer.CameraBufferCursor = 0;
}

uint64_t Engine::AllocateCameraConstantOffset() noexcept
{
	if (!m_StaticMeshRenderer.CameraBuffer || m_StaticMeshRenderer.CameraBufferCapacity == 0)
	{
		return InvalidCameraConstantOffset();
	}

	if (m_StaticMeshRenderer.CameraBufferCursor >= m_StaticMeshRenderer.CameraBufferCapacity)
	{
		return InvalidCameraConstantOffset();
	}

	const uint64_t offset = m_StaticMeshRenderer.CameraBufferStride * m_StaticMeshRenderer.CameraBufferCursor;
	++m_StaticMeshRenderer.CameraBufferCursor;
	if (offset + sizeof(CameraConstants) > m_StaticMeshRenderer.CameraBuffer->GetSize())
	{
		return InvalidCameraConstantOffset();
	}

	return offset;
}

uint64_t Engine::WriteCameraConstants(const CameraConstants& cameraConstants)
{
	const uint64_t offset = AllocateCameraConstantOffset();
	if (offset == InvalidCameraConstantOffset())
	{
		return InvalidCameraConstantOffset();
	}

	void* mappedData = nullptr;
	m_StaticMeshRenderer.CameraBuffer->Map(&mappedData);
	if (!mappedData)
	{
		return InvalidCameraConstantOffset();
	}

	std::memcpy(static_cast<uint8_t*>(mappedData) + offset, &cameraConstants, sizeof(cameraConstants));
	m_StaticMeshRenderer.CameraBuffer->Unmap();
	return offset;
}

uint64_t Engine::UpdateCameraBuffer()
{
	return UpdateCameraBuffer(m_Camera);
}

uint64_t Engine::UpdateCameraBuffer(EntityId entityId)
{
	return UpdateCameraBuffer(entityId, m_Camera);
}

uint64_t Engine::UpdateCameraBuffer(const Camera& camera)
{
	return UpdateCameraBuffer(GetRuntimeScene().GetPrimaryRenderableEntity(), camera);
}

uint64_t Engine::UpdateCameraBuffer(EntityId entityId, const Camera& camera)
{
	return UpdateCameraBuffer(entityId, camera, 0);
}

uint64_t Engine::UpdateCameraBuffer(EntityId entityId, const Camera& camera, size_t materialIndex)
{
	return UpdateCameraBuffer(entityId, camera, materialIndex, m_RenderMode == RenderMode::Deferred);
}

uint64_t Engine::UpdateCameraBuffer(EntityId entityId, const Camera& camera, size_t materialIndex, bool useDeferredLighting)
{
	if (!m_StaticMeshRenderer.CameraBuffer)
	{
		return InvalidCameraConstantOffset();
	}
	TouchShaderVariant(entityId, materialIndex, useDeferredLighting);

	CameraConstants cameraConstants = {};
	if (!RenderSystem::BuildCameraConstants(
		GetRuntimeScene(),
		camera,
		entityId,
		materialIndex,
		cameraConstants,
		m_AmbientColor,
		m_AmbientIntensity,
		m_Exposure,
		m_MaterialDebugView,
		ResolveKeyLightEntity(),
		useDeferredLighting,
		useDeferredLighting ? m_StaticMeshRenderer.DeferredLightCount : 0,
		m_ShadowSettings))
	{
		return InvalidCameraConstantOffset();
	}

	return WriteCameraConstants(cameraConstants);
}

uint64_t Engine::UpdateShadowCameraBuffer(EntityId entityId)
{
	if (!m_StaticMeshRenderer.CameraBuffer)
	{
		return InvalidCameraConstantOffset();
	}

	const TransformComponent* transform = GetRuntimeScene().GetTransformComponent(entityId);
	if (!transform)
	{
		return InvalidCameraConstantOffset();
	}

	CameraConstants cameraConstants = {};
	const DirectX::XMMATRIX worldMatrix = transform->GetWorldXmMatrix();
	const DirectX::XMMATRIX lightViewProjection = DirectX::XMLoadFloat4x4(&m_ShadowFrameData.LightViewProjection);
	Math::Store(cameraConstants.WorldViewProjection, worldMatrix * lightViewProjection);
	Math::Store(cameraConstants.ViewProjection, lightViewProjection);
	Math::Store(cameraConstants.World, worldMatrix);
	Math::Store(cameraConstants.WorldInverseTranspose, DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, worldMatrix)));
	cameraConstants.ShadowViewProjection = m_ShadowFrameData.LightViewProjection;
	cameraConstants.ShadowParams = m_ShadowFrameData.Params;
	cameraConstants.ShadowDirection = m_ShadowFrameData.DirectionToLight;
	return WriteCameraConstants(cameraConstants);
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

	GetRuntimeScene().SetSelectedEntity(TryPickEntity(static_cast<float>(mouseX), static_cast<float>(mouseY)));
}

bool Engine::TryPickSpider(float mouseX, float mouseY) const
{
	return TryPickEntity(mouseX, mouseY) == m_SpiderEntity;
}

EntityId Engine::TryPickEntity(float mouseX, float mouseY) const
{
	return TryPickEntity(mouseX, mouseY, m_Camera, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
}

EntityId Engine::TryPickEntity(float mouseX, float mouseY, const Camera& camera, float viewportWidth, float viewportHeight) const
{
	const Scene& runtimeScene = GetRuntimeScene();
	EntityId closestEntity = InvalidEntityId;
	float closestDistance = (std::numeric_limits<float>::max)();
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (entityId == InvalidEntityId
			|| !runtimeScene.IsMeshEnabled(entityId)
			|| !runtimeScene.IsEntityVisibleInScene(entityId)
			|| !runtimeScene.IsEntityPickableInScene(entityId))
		{
			continue;
		}

		float hitDistance = 0.0f;
		if (PickingSystem::TryPickEntityAabb(
			runtimeScene,
			entityId,
			camera,
			mouseX,
			mouseY,
			viewportWidth,
			viewportHeight,
			hitDistance)
			&& hitDistance < closestDistance)
		{
			closestEntity = entityId;
			closestDistance = hitDistance;
		}
	}

	return closestEntity;
}


























void Engine::UpdateAnimatedMesh(Scene& runtimeScene, float deltaTime)
{
	struct AnimationTask
	{
		EntityId Entity = InvalidEntityId;
		Asset::StaticMeshAsset* Mesh = nullptr;
		uint32_t ClipIndex = 0;
		double AnimationTimeTicks = 0.0;
		std::vector<Asset::StaticMeshVertex> SkinnedVertices;
		bool Success = false;
	};

	std::vector<AnimationTask> tasks;
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId) || !runtimeScene.IsAnimatorEnabled(entityId))
		{
			continue;
		}

		if (AnimatorComponent* animator = runtimeScene.GetAnimatorComponent(entityId))
		{
			Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
			if (!meshAsset)
			{
				continue;
			}

			AnimationTask task;
			task.Entity = entityId;
			task.Mesh = meshAsset;
			if (AnimationSystem::AdvanceAnimator(*meshAsset, deltaTime, *animator, task.ClipIndex, task.AnimationTimeTicks))
			{
				tasks.push_back(std::move(task));
			}
		}
	}

	if (tasks.empty())
	{
		return;
	}

	if (m_JobSystem.IsInitialized() && tasks.size() > 1)
	{
		m_JobSystem.RunParallelFor(
			tasks.size(),
			1,
			"Animation Skinning",
			[&tasks](size_t begin, size_t end, Jobs::JobContext&)
			{
				for (size_t taskIndex = begin; taskIndex < end; ++taskIndex)
				{
					AnimationTask& task = tasks[taskIndex];
					task.Success = task.Mesh
						&& AnimationSystem::BuildSkinnedVertices(*task.Mesh, task.ClipIndex, task.AnimationTimeTicks, task.SkinnedVertices);
				}
			});
	}
	else
	{
		for (AnimationTask& task : tasks)
		{
			task.Success = task.Mesh
				&& AnimationSystem::BuildSkinnedVertices(*task.Mesh, task.ClipIndex, task.AnimationTimeTicks, task.SkinnedVertices);
		}
	}

	for (AnimationTask& task : tasks)
	{
		if (task.Success && task.Mesh)
		{
			task.Mesh->Vertices.assign(task.SkinnedVertices.begin(), task.SkinnedVertices.end());
		}
	}
}

bool Engine::CreateVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto vulkanCameraBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.CameraBuffer.get());
	auto vulkanLightBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightBuffer.get());
	if (!vulkanDevice || !vulkanCameraBuffer || !vulkanLightBuffer)
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
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
	};
	std::array<VkDescriptorSetLayoutBinding, Asset::kMaterialTextureSlotCount + 2> bindings = {};
	bindings[0] = cameraBinding;
	for (size_t slotIndex = 0; slotIndex < MaterialSlotCount(); ++slotIndex)
	{
		bindings[slotIndex + 1] = {
			.binding = static_cast<uint32_t>(1 + slotIndex),
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
		};
	}
	bindings[MaterialSlotCount() + 1] = {
		.binding = static_cast<uint32_t>(MaterialSlotCount() + 1),
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
	};

	const VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(bindings.size()),
		.pBindings = bindings.data()
	};

	if (vkCreateDescriptorSetLayout(vulkanDevice->GetVkDevice(), &descriptorSetLayoutCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.DescriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}
	if (m_StaticMeshRenderer.Vulkan.MaterialTextures.empty())
	{
		return false;
	}

	const uint32_t materialTextureCount = static_cast<uint32_t>(
		m_StaticMeshRenderer.Vulkan.MaterialCount > 0
			? m_StaticMeshRenderer.Vulkan.MaterialCount
			: MaterialCountFromFlattenedTextureCount(m_StaticMeshRenderer.Vulkan.MaterialTextures.size()));

	const VkDescriptorPoolSize descriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		.descriptorCount = materialTextureCount
	};
	const VkDescriptorPoolSize textureDescriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = materialTextureCount * static_cast<uint32_t>(MaterialSlotCount())
	};
	const VkDescriptorPoolSize lightDescriptorPoolSize = {
		.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = materialTextureCount
	};
	const VkDescriptorPoolSize descriptorPoolSizes[] = { descriptorPoolSize, textureDescriptorPoolSize, lightDescriptorPoolSize };

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
	const VkDescriptorBufferInfo lightBufferInfo = {
		.buffer = vulkanLightBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredLightBuffer->GetSize()
	};
	// Vulkan 경로는 material 수만큼 descriptor set을 만들고, 각 set에 동일한 camera buffer와 material별 texture를 기록합니다.
	// 이렇게 해 두면 draw 시 submesh.MaterialIndex에 맞는 descriptor set 하나만 다시 바인딩하면 됩니다.
	for (uint32_t materialIndex = 0; materialIndex < materialTextureCount; ++materialIndex)
	{
		std::array<VkDescriptorImageInfo, Asset::kMaterialTextureSlotCount> textureImageInfos = {};
		for (size_t slotIndex = 0; slotIndex < MaterialSlotCount(); ++slotIndex)
		{
			const size_t flattenedIndex = FlattenMaterialTextureIndex(materialIndex, slotIndex);
			const auto& materialTexture = m_StaticMeshRenderer.Vulkan.MaterialTextures[(std::min)(flattenedIndex, m_StaticMeshRenderer.Vulkan.MaterialTextures.size() - 1)];
			textureImageInfos[slotIndex] = {
				.sampler = materialTexture.Sampler,
				.imageView = materialTexture.ImageView,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			};
		}

		const VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			.pBufferInfo = &cameraBufferInfo
		};
		std::array<VkWriteDescriptorSet, Asset::kMaterialTextureSlotCount + 2> writeDescriptorSets = {};
		writeDescriptorSets[0] = writeDescriptorSet;
		for (size_t slotIndex = 0; slotIndex < MaterialSlotCount(); ++slotIndex)
		{
			writeDescriptorSets[slotIndex + 1] = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex],
				.dstBinding = static_cast<uint32_t>(1 + slotIndex),
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &textureImageInfos[slotIndex]
			};
		}
		writeDescriptorSets[MaterialSlotCount() + 1] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_StaticMeshRenderer.Vulkan.DescriptorSets[materialIndex],
			.dstBinding = static_cast<uint32_t>(MaterialSlotCount() + 1),
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &lightBufferInfo
		};

		vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
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
		},
		{
			.location = 4,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, Tangent)
		},
		{
			.location = 5,
			.binding = 0,
			.format = VK_FORMAT_R32_SFLOAT,
			.offset = offsetof(Asset::StaticMeshVertex, TangentSign)
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

	if (!RecreateVulkanEntityDescriptorSets())
	{
		return false;
	}

	if (!CreateVulkanShadowResources())
	{
		return false;
	}

	if (!CreateVulkanSkyboxResources())
	{
		return false;
	}

	if (!CreateVulkanDeferredResources())
	{
		return false;
	}

	m_StaticMeshRenderer.Vulkan.IsValid = true;
	return true;
}

bool Engine::CreateVulkanSkyboxResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		return false;
	}

	const std::string vertexSource = ShaderUtils::LoadShaderSource(GetVulkanSkyboxVertexShaderPath());
	const std::string fragmentSource = ShaderUtils::LoadShaderSource(GetVulkanSkyboxFragmentShaderPath());
	if (vertexSource.empty() || fragmentSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"Vulkan Skybox 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	auto createShaderModule = [vulkanDevice](const std::vector<uint32_t>& code, VkShaderModule& shaderModule)
	{
		if (code.empty())
		{
			return false;
		}
		const VkShaderModuleCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = code.size() * sizeof(uint32_t),
			.pCode = code.data()
		};
		return vkCreateShaderModule(vulkanDevice->GetVkDevice(), &createInfo, nullptr, &shaderModule) == VK_SUCCESS;
	};

	if (!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_VERTEX, vertexSource), m_StaticMeshRenderer.Vulkan.SkyboxVertexShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_FRAGMENT, fragmentSource), m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader))
	{
		return false;
	}

	const VkPushConstantRange pushConstantRange = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = static_cast<uint32_t>(sizeof(Rendering::SkyboxGpuConstants))
	};
	const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushConstantRange
	};
	if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &pipelineLayoutCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout) != VK_SUCCESS)
	{
		return false;
	}

	const VkPipelineShaderStageCreateInfo shaderStages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = m_StaticMeshRenderer.Vulkan.SkyboxVertexShader,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader,
			.pName = "main"
		}
	};
	const VkPipelineVertexInputStateCreateInfo vertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
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
		.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
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
	const VkPipelineDepthStencilStateCreateInfo depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_FALSE,
		.depthWriteEnable = VK_FALSE
	};
	const VkPipelineColorBlendAttachmentState colorBlendAttachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};
	const VkPipelineColorBlendStateCreateInfo colorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorBlendAttachment
	};
	const VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = static_cast<uint32_t>(std::size(shaderStages)),
		.pStages = shaderStages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depthStencil,
		.pColorBlendState = &colorBlending,
		.pDynamicState = &dynamicState,
		.layout = m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout,
		.renderPass = vulkanDevice->GetVkRenderPass(),
		.subpass = 0
	};

	return vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_StaticMeshRenderer.Vulkan.SkyboxPipeline) == VK_SUCCESS;
}

bool Engine::CreateVulkanDeferredResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice || m_StaticMeshRenderer.Vulkan.PipelineLayout == VK_NULL_HANDLE)
	{
		return false;
	}

	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	const std::string geometryVertexSource = ShaderUtils::LoadShaderSource(GetVulkanDeferredGeometryVertexShaderPath());
	const std::string geometryFragmentSource = ShaderUtils::LoadShaderSource(GetVulkanDeferredGeometryFragmentShaderPath());
	const std::string lightingVertexSource = ShaderUtils::LoadShaderSource(GetVulkanDeferredLightingVertexShaderPath());
	const std::string lightingFragmentSource = ShaderUtils::LoadShaderSource(GetVulkanDeferredLightingFragmentShaderPath());
	const std::string toneMapVertexSource = ShaderUtils::LoadShaderSource(GetVulkanToneMapVertexShaderPath());
	const std::string toneMapFragmentSource = ShaderUtils::LoadShaderSource(GetVulkanToneMapFragmentShaderPath());
	if (geometryVertexSource.empty() ||
		geometryFragmentSource.empty() ||
		lightingVertexSource.empty() ||
		lightingFragmentSource.empty() ||
		toneMapVertexSource.empty() ||
		toneMapFragmentSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"Vulkan Deferred 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	auto createShaderModule = [&](const std::vector<uint32_t>& code, VkShaderModule& shaderModule)
	{
		if (code.empty())
		{
			return false;
		}
		const VkShaderModuleCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = code.size() * sizeof(uint32_t),
			.pCode = code.data()
		};
		return vkCreateShaderModule(vulkanDevice->GetVkDevice(), &createInfo, nullptr, &shaderModule) == VK_SUCCESS;
	};

	if (!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_VERTEX, geometryVertexSource), deferred.GeometryVertexShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_FRAGMENT, geometryFragmentSource), deferred.GeometryFragmentShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_VERTEX, lightingVertexSource), deferred.LightingVertexShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_FRAGMENT, lightingFragmentSource), deferred.LightingFragmentShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_VERTEX, toneMapVertexSource), deferred.ToneMapVertexShader) ||
		!createShaderModule(ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_FRAGMENT, toneMapFragmentSource), deferred.ToneMapFragmentShader))
	{
		return false;
	}

	std::array<VkAttachmentDescription, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 1> attachments = {};
	for (size_t attachmentIndex = 0; attachmentIndex < Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount; ++attachmentIndex)
	{
		attachments[attachmentIndex] = {
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};
	}
	attachments.back() = {
		.format = VK_FORMAT_D32_SFLOAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};

	std::array<VkAttachmentReference, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount> colorAttachmentRefs = {};
	for (uint32_t attachmentIndex = 0; attachmentIndex < colorAttachmentRefs.size(); ++attachmentIndex)
	{
		colorAttachmentRefs[attachmentIndex] = {
			.attachment = attachmentIndex,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
		};
	}
	const VkAttachmentReference depthAttachmentRef = {
		.attachment = static_cast<uint32_t>(Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount),
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentRefs.size()),
		.pColorAttachments = colorAttachmentRefs.data(),
		.pDepthStencilAttachment = &depthAttachmentRef
	};
	const VkRenderPassCreateInfo renderPassCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = static_cast<uint32_t>(attachments.size()),
		.pAttachments = attachments.data(),
		.subpassCount = 1,
		.pSubpasses = &subpass
	};
	if (vkCreateRenderPass(vulkanDevice->GetVkDevice(), &renderPassCreateInfo, nullptr, &deferred.GeometryRenderPass) != VK_SUCCESS)
	{
		return false;
	}

	const VkAttachmentDescription hdrAttachment = {
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};
	const VkAttachmentReference hdrAttachmentRef = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	};
	const VkSubpassDescription hdrSubpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &hdrAttachmentRef
	};
	const VkSubpassDependency hdrDependencies[] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		}
	};
	const VkRenderPassCreateInfo hdrRenderPassCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &hdrAttachment,
		.subpassCount = 1,
		.pSubpasses = &hdrSubpass,
		.dependencyCount = static_cast<uint32_t>(std::size(hdrDependencies)),
		.pDependencies = hdrDependencies
	};
	if (vkCreateRenderPass(vulkanDevice->GetVkDevice(), &hdrRenderPassCreateInfo, nullptr, &deferred.LightingRenderPass) != VK_SUCCESS)
	{
		return false;
	}

	const VkVertexInputBindingDescription vertexBindingDescription = {
		.binding = 0,
		.stride = sizeof(Asset::StaticMeshVertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX
	};
	const VkVertexInputAttributeDescription vertexAttributeDescriptions[] = {
		{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, Position) },
		{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, Normal) },
		{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, TexCoord) },
		{ .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, Color) },
		{ .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, Tangent) },
		{ .location = 5, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, TangentSign) }
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
	std::array<VkPipelineColorBlendAttachmentState, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount> colorBlendAttachments = {};
	for (auto& attachment : colorBlendAttachments)
	{
		attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}
	const VkPipelineColorBlendStateCreateInfo colorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size()),
		.pAttachments = colorBlendAttachments.data()
	};
	const VkPipelineDepthStencilStateCreateInfo depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS
	};
	const VkPipelineShaderStageCreateInfo geometryShaderStages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = deferred.GeometryVertexShader, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = deferred.GeometryFragmentShader, .pName = "main" }
	};
	const VkGraphicsPipelineCreateInfo geometryPipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = static_cast<uint32_t>(std::size(geometryShaderStages)),
		.pStages = geometryShaderStages,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depthStencil,
		.pColorBlendState = &colorBlending,
		.pDynamicState = &dynamicState,
		.layout = m_StaticMeshRenderer.Vulkan.PipelineLayout,
		.renderPass = deferred.GeometryRenderPass,
		.subpass = 0
	};
	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &geometryPipelineCreateInfo, nullptr, &deferred.GeometryPipeline) != VK_SUCCESS)
	{
		return false;
	}

	const VkDescriptorSetLayoutBinding lightingBindings[] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 10, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 11, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 12, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT }
	};
	const VkDescriptorSetLayoutCreateInfo lightingDescriptorSetLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(std::size(lightingBindings)),
		.pBindings = lightingBindings
	};
	if (vkCreateDescriptorSetLayout(vulkanDevice->GetVkDevice(), &lightingDescriptorSetLayoutInfo, nullptr, &deferred.LightingDescriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}
	const VkPipelineLayoutCreateInfo lightingPipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &deferred.LightingDescriptorSetLayout
	};
	if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &lightingPipelineLayoutInfo, nullptr, &deferred.LightingPipelineLayout) != VK_SUCCESS)
	{
		return false;
	}

	const VkDescriptorSetLayoutBinding toneMapBindings[] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT }
	};
	const VkDescriptorSetLayoutCreateInfo toneMapDescriptorSetLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = static_cast<uint32_t>(std::size(toneMapBindings)),
		.pBindings = toneMapBindings
	};
	if (vkCreateDescriptorSetLayout(vulkanDevice->GetVkDevice(), &toneMapDescriptorSetLayoutInfo, nullptr, &deferred.ToneMapDescriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}
	const VkPipelineLayoutCreateInfo toneMapPipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &deferred.ToneMapDescriptorSetLayout
	};
	if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &toneMapPipelineLayoutInfo, nullptr, &deferred.ToneMapPipelineLayout) != VK_SUCCESS)
	{
		return false;
	}

	const VkPipelineVertexInputStateCreateInfo lightingVertexInput = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
	};
	const VkPipelineDepthStencilStateCreateInfo lightingDepthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_FALSE,
		.depthWriteEnable = VK_FALSE
	};
	const VkPipelineColorBlendAttachmentState lightingBlendAttachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
	};
	const VkPipelineColorBlendStateCreateInfo lightingBlendState = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &lightingBlendAttachment
	};
	const VkPipelineShaderStageCreateInfo lightingShaderStages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = deferred.LightingVertexShader, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = deferred.LightingFragmentShader, .pName = "main" }
	};
	const VkGraphicsPipelineCreateInfo lightingPipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = static_cast<uint32_t>(std::size(lightingShaderStages)),
		.pStages = lightingShaderStages,
		.pVertexInputState = &lightingVertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &lightingDepthStencil,
		.pColorBlendState = &lightingBlendState,
		.pDynamicState = &dynamicState,
		.layout = deferred.LightingPipelineLayout,
		.renderPass = deferred.LightingRenderPass,
		.subpass = 0
	};
	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &lightingPipelineCreateInfo, nullptr, &deferred.LightingPipeline) != VK_SUCCESS)
	{
		return false;
	}

	const VkPipelineShaderStageCreateInfo toneMapShaderStages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = deferred.ToneMapVertexShader, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = deferred.ToneMapFragmentShader, .pName = "main" }
	};
	VkGraphicsPipelineCreateInfo toneMapPipelineCreateInfo = lightingPipelineCreateInfo;
	toneMapPipelineCreateInfo.stageCount = static_cast<uint32_t>(std::size(toneMapShaderStages));
	toneMapPipelineCreateInfo.pStages = toneMapShaderStages;
	toneMapPipelineCreateInfo.layout = deferred.ToneMapPipelineLayout;
	toneMapPipelineCreateInfo.renderPass = vulkanDevice->GetVkLoadRenderPass() != VK_NULL_HANDLE ? vulkanDevice->GetVkLoadRenderPass() : vulkanDevice->GetVkRenderPass();
	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &toneMapPipelineCreateInfo, nullptr, &deferred.ToneMapPipeline) != VK_SUCCESS)
	{
		return false;
	}

	return EnsureVulkanDeferredResources();
}

bool Engine::CreateVulkanShadowResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice || m_StaticMeshRenderer.Vulkan.PipelineLayout == VK_NULL_HANDLE)
	{
		return false;
	}

	auto& shadow = m_StaticMeshRenderer.Vulkan.Shadow;
	const std::string shadowVertexSource = ShaderUtils::LoadShaderSource(GetVulkanShadowDepthVertexShaderPath());
	if (shadowVertexSource.empty())
	{
		MessageBoxW(m_hMainWnd, L"Vulkan Shadow 셰이더 파일을 읽을 수 없습니다.", L"Shader Error", MB_OK | MB_ICONERROR);
		return false;
	}

	const std::vector<uint32_t> shaderCode = ShaderUtils::CompileGlslToSpirv(GLSLANG_STAGE_VERTEX, shadowVertexSource);
	if (shaderCode.empty())
	{
		return false;
	}

	const VkShaderModuleCreateInfo shaderModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shaderCode.size() * sizeof(uint32_t),
		.pCode = shaderCode.data()
	};
	if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &shaderModuleCreateInfo, nullptr, &shadow.VertexShader) != VK_SUCCESS)
	{
		return false;
	}

	const VkAttachmentDescription depthAttachment = {
		.format = VK_FORMAT_D32_SFLOAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	};
	const VkAttachmentReference depthAttachmentRef = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};
	const VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.pDepthStencilAttachment = &depthAttachmentRef
	};
	const VkSubpassDependency dependencies[] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
		}
	};
	const VkRenderPassCreateInfo renderPassCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &depthAttachment,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = static_cast<uint32_t>(std::size(dependencies)),
		.pDependencies = dependencies
	};
	if (vkCreateRenderPass(vulkanDevice->GetVkDevice(), &renderPassCreateInfo, nullptr, &shadow.RenderPass) != VK_SUCCESS)
	{
		return false;
	}

	const VkVertexInputBindingDescription vertexBindingDescription = {
		.binding = 0,
		.stride = sizeof(Asset::StaticMeshVertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX
	};
	const VkVertexInputAttributeDescription vertexAttributeDescriptions[] = {
		{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Asset::StaticMeshVertex, Position) }
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
		.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates)),
		.pDynamicStates = dynamicStates
	};
	const VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.depthBiasEnable = VK_TRUE,
		.depthBiasConstantFactor = 1.25f,
		.depthBiasSlopeFactor = 1.75f,
		.lineWidth = 1.0f
	};
	const VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};
	const VkPipelineDepthStencilStateCreateInfo depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL
	};
	const VkPipelineColorBlendStateCreateInfo colorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
	};
	const VkPipelineShaderStageCreateInfo shaderStage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
		.module = shadow.VertexShader,
		.pName = "main"
	};
	const VkGraphicsPipelineCreateInfo pipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 1,
		.pStages = &shaderStage,
		.pVertexInputState = &vertexInput,
		.pInputAssemblyState = &inputAssembly,
		.pViewportState = &viewportState,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depthStencil,
		.pColorBlendState = &colorBlending,
		.pDynamicState = &dynamicState,
		.layout = m_StaticMeshRenderer.Vulkan.PipelineLayout,
		.renderPass = shadow.RenderPass,
		.subpass = 0
	};
	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &shadow.Pipeline) != VK_SUCCESS)
	{
		return false;
	}

	return EnsureVulkanShadowResources();
}

bool Engine::EnsureVulkanShadowResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto& shadow = m_StaticMeshRenderer.Vulkan.Shadow;
	if (!vulkanDevice || shadow.RenderPass == VK_NULL_HANDLE)
	{
		return false;
	}

	const uint32_t mapSize = m_ShadowSettings.Enabled
		? std::clamp(m_ShadowSettings.MapSize, 256u, 8192u)
		: 1u;
	if (shadow.IsValid && shadow.Size == mapSize)
	{
		WriteVulkanDeferredShadowDescriptor();
		return true;
	}

	m_Graphics.Device->WaitForGPU();
	const VkDevice device = vulkanDevice->GetVkDevice();
	if (shadow.Framebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(device, shadow.Framebuffer, nullptr);
		shadow.Framebuffer = VK_NULL_HANDLE;
	}
	if (shadow.DepthSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(device, shadow.DepthSampler, nullptr);
		shadow.DepthSampler = VK_NULL_HANDLE;
	}
	if (shadow.DepthImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(device, shadow.DepthImageView, nullptr);
		shadow.DepthImageView = VK_NULL_HANDLE;
	}
	if (shadow.DepthImage != VK_NULL_HANDLE)
	{
		vkDestroyImage(device, shadow.DepthImage, nullptr);
		shadow.DepthImage = VK_NULL_HANDLE;
	}
	if (shadow.DepthMemory != VK_NULL_HANDLE)
	{
		vkFreeMemory(device, shadow.DepthMemory, nullptr);
		shadow.DepthMemory = VK_NULL_HANDLE;
	}

	const VkImageCreateInfo imageCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.extent = { mapSize, mapSize, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE
	};
	if (vkCreateImage(device, &imageCreateInfo, nullptr, &shadow.DepthImage) != VK_SUCCESS)
	{
		shadow.IsValid = false;
		return false;
	}

	VkMemoryRequirements memoryRequirements = {};
	vkGetImageMemoryRequirements(device, shadow.DepthImage, &memoryRequirements);
	const VkMemoryAllocateInfo allocateInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memoryRequirements.size,
		.memoryTypeIndex = vulkanDevice->FindMemoryTypeForTexture(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	};
	if (vkAllocateMemory(device, &allocateInfo, nullptr, &shadow.DepthMemory) != VK_SUCCESS)
	{
		shadow.IsValid = false;
		return false;
	}
	vkBindImageMemory(device, shadow.DepthImage, shadow.DepthMemory, 0);

	const VkImageViewCreateInfo imageViewCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = shadow.DepthImage,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		}
	};
	if (vkCreateImageView(device, &imageViewCreateInfo, nullptr, &shadow.DepthImageView) != VK_SUCCESS)
	{
		shadow.IsValid = false;
		return false;
	}

	const VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.compareEnable = VK_TRUE,
		.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.minLod = 0.0f,
		.maxLod = 1.0f,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
	};
	if (vkCreateSampler(device, &samplerCreateInfo, nullptr, &shadow.DepthSampler) != VK_SUCCESS)
	{
		shadow.IsValid = false;
		return false;
	}

	const VkFramebufferCreateInfo framebufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = shadow.RenderPass,
		.attachmentCount = 1,
		.pAttachments = &shadow.DepthImageView,
		.width = mapSize,
		.height = mapSize,
		.layers = 1
	};
	if (vkCreateFramebuffer(device, &framebufferCreateInfo, nullptr, &shadow.Framebuffer) != VK_SUCCESS)
	{
		shadow.IsValid = false;
		return false;
	}

	shadow.Size = mapSize;
	shadow.IsValid = true;
	WriteVulkanDeferredShadowDescriptor();
	return true;
}

void Engine::WriteVulkanDeferredShadowDescriptor()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	auto& shadow = m_StaticMeshRenderer.Vulkan.Shadow;
	if (!vulkanDevice || deferred.LightingDescriptorSet == VK_NULL_HANDLE || shadow.DepthSampler == VK_NULL_HANDLE || shadow.DepthImageView == VK_NULL_HANDLE)
	{
		return;
	}

	const VkDescriptorImageInfo shadowInfo = {
		.sampler = shadow.DepthSampler,
		.imageView = shadow.DepthImageView,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	};
	const VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 5,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &shadowInfo
	};
	vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), 1, &write, 0, nullptr);
}

void Engine::WriteVulkanToneMapDescriptor()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto vulkanLightingBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightingBuffer.get());
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	if (!vulkanDevice ||
		!vulkanLightingBuffer ||
		deferred.ToneMapDescriptorSet == VK_NULL_HANDLE ||
		deferred.HdrSampler == VK_NULL_HANDLE ||
		deferred.HdrColorImageView == VK_NULL_HANDLE)
	{
		return;
	}

	const VkDescriptorBufferInfo postProcessBufferInfo = {
		.buffer = vulkanLightingBuffer->GetVkBuffer(),
		.offset = 0,
		.range = sizeof(DeferredLightingConstants)
	};
	const VkDescriptorImageInfo hdrImageInfo = {
		.sampler = deferred.HdrSampler,
		.imageView = deferred.HdrColorImageView,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	};
	const VkWriteDescriptorSet writes[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = deferred.ToneMapDescriptorSet,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &postProcessBufferInfo
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = deferred.ToneMapDescriptorSet,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &hdrImageInfo
		}
	};
	vkUpdateDescriptorSets(vulkanDevice->GetVkDevice(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
}

bool Engine::EnsureVulkanDeferredResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	auto vulkanLightingBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightingBuffer.get());
	auto vulkanLightBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredLightBuffer.get());
	auto vulkanTileRangeBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredTileRangeBuffer.get());
	auto vulkanTileIndexBuffer = dynamic_cast<VulkanBuffer*>(m_StaticMeshRenderer.DeferredTileLightIndexBuffer.get());
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	if (!vulkanDevice || !vulkanLightingBuffer || !vulkanLightBuffer || !vulkanTileRangeBuffer || !vulkanTileIndexBuffer ||
		deferred.GeometryRenderPass == VK_NULL_HANDLE || deferred.LightingDescriptorSetLayout == VK_NULL_HANDLE)
	{
		return false;
	}
	if (deferred.LightingRenderPass == VK_NULL_HANDLE ||
		deferred.ToneMapDescriptorSetLayout == VK_NULL_HANDLE ||
		deferred.ToneMapPipelineLayout == VK_NULL_HANDLE ||
		deferred.ToneMapPipeline == VK_NULL_HANDLE)
	{
		return false;
	}

	if (!EnsureVulkanShadowResources())
	{
		return false;
	}

	const VkExtent2D extent = vulkanDevice->GetVkSwapchainExtent();
	const uint32_t width = (std::max)(extent.width, 1u);
	const uint32_t height = (std::max)(extent.height, 1u);
	bool gBuffersValid = true;
	for (size_t targetIndex = 0; targetIndex < deferred.GBufferImages.size(); ++targetIndex)
	{
		gBuffersValid = gBuffersValid &&
			deferred.GBufferImages[targetIndex] != VK_NULL_HANDLE &&
			deferred.GBufferMemories[targetIndex] != VK_NULL_HANDLE &&
			deferred.GBufferImageViews[targetIndex] != VK_NULL_HANDLE;
	}

	if (deferred.IsValid &&
		deferred.Width == width &&
		deferred.Height == height &&
		gBuffersValid &&
		deferred.GeometryFramebuffer != VK_NULL_HANDLE &&
		deferred.LightingFramebuffer != VK_NULL_HANDLE &&
		deferred.HdrColorImage != VK_NULL_HANDLE &&
		deferred.HdrColorMemory != VK_NULL_HANDLE &&
		deferred.HdrColorImageView != VK_NULL_HANDLE &&
		deferred.HdrSampler != VK_NULL_HANDLE &&
		deferred.LightingDescriptorSet != VK_NULL_HANDLE &&
		deferred.ToneMapDescriptorSet != VK_NULL_HANDLE)
	{
		WriteVulkanDeferredShadowDescriptor();
		WriteVulkanToneMapDescriptor();
		return true;
	}

	m_Graphics.Device->WaitForGPU();
	const VkDevice device = vulkanDevice->GetVkDevice();
	if (deferred.LightingFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(device, deferred.LightingFramebuffer, nullptr);
		deferred.LightingFramebuffer = VK_NULL_HANDLE;
	}
	if (deferred.GeometryFramebuffer != VK_NULL_HANDLE)
	{
		vkDestroyFramebuffer(device, deferred.GeometryFramebuffer, nullptr);
		deferred.GeometryFramebuffer = VK_NULL_HANDLE;
	}
	if (deferred.ToneMapDescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(device, deferred.ToneMapDescriptorPool, nullptr);
		deferred.ToneMapDescriptorPool = VK_NULL_HANDLE;
		deferred.ToneMapDescriptorSet = VK_NULL_HANDLE;
	}
	if (deferred.LightingDescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(device, deferred.LightingDescriptorPool, nullptr);
		deferred.LightingDescriptorPool = VK_NULL_HANDLE;
		deferred.LightingDescriptorSet = VK_NULL_HANDLE;
	}
	if (deferred.HdrSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(device, deferred.HdrSampler, nullptr);
		deferred.HdrSampler = VK_NULL_HANDLE;
	}
	if (deferred.GBufferSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(device, deferred.GBufferSampler, nullptr);
		deferred.GBufferSampler = VK_NULL_HANDLE;
	}
	if (deferred.HdrColorImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(device, deferred.HdrColorImageView, nullptr);
		deferred.HdrColorImageView = VK_NULL_HANDLE;
	}
	if (deferred.HdrColorImage != VK_NULL_HANDLE)
	{
		vkDestroyImage(device, deferred.HdrColorImage, nullptr);
		deferred.HdrColorImage = VK_NULL_HANDLE;
	}
	if (deferred.HdrColorMemory != VK_NULL_HANDLE)
	{
		vkFreeMemory(device, deferred.HdrColorMemory, nullptr);
		deferred.HdrColorMemory = VK_NULL_HANDLE;
	}
	for (size_t targetIndex = 0; targetIndex < deferred.GBufferImageViews.size(); ++targetIndex)
	{
		if (deferred.GBufferImageViews[targetIndex] != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, deferred.GBufferImageViews[targetIndex], nullptr);
			deferred.GBufferImageViews[targetIndex] = VK_NULL_HANDLE;
		}
		if (deferred.GBufferImages[targetIndex] != VK_NULL_HANDLE)
		{
			vkDestroyImage(device, deferred.GBufferImages[targetIndex], nullptr);
			deferred.GBufferImages[targetIndex] = VK_NULL_HANDLE;
		}
		if (deferred.GBufferMemories[targetIndex] != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, deferred.GBufferMemories[targetIndex], nullptr);
			deferred.GBufferMemories[targetIndex] = VK_NULL_HANDLE;
		}
	}
	if (deferred.DepthImageView != VK_NULL_HANDLE)
	{
		vkDestroyImageView(device, deferred.DepthImageView, nullptr);
		deferred.DepthImageView = VK_NULL_HANDLE;
	}
	if (deferred.DepthImage != VK_NULL_HANDLE)
	{
		vkDestroyImage(device, deferred.DepthImage, nullptr);
		deferred.DepthImage = VK_NULL_HANDLE;
	}
	if (deferred.DepthMemory != VK_NULL_HANDLE)
	{
		vkFreeMemory(device, deferred.DepthMemory, nullptr);
		deferred.DepthMemory = VK_NULL_HANDLE;
	}

	auto createImage = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& imageView)
	{
		const VkImageCreateInfo imageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = format,
			.extent = { width, height, 1 },
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE
		};
		if (vkCreateImage(device, &imageCreateInfo, nullptr, &image) != VK_SUCCESS)
		{
			return false;
		}
		VkMemoryRequirements memoryRequirements = {};
		vkGetImageMemoryRequirements(device, image, &memoryRequirements);
		const VkMemoryAllocateInfo allocateInfo = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = memoryRequirements.size,
			.memoryTypeIndex = vulkanDevice->FindMemoryTypeForTexture(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};
		if (vkAllocateMemory(device, &allocateInfo, nullptr, &memory) != VK_SUCCESS)
		{
			return false;
		}
		vkBindImageMemory(device, image, memory, 0);

		const VkImageViewCreateInfo imageViewCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = format,
			.subresourceRange = {
				.aspectMask = aspect,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};
		return vkCreateImageView(device, &imageViewCreateInfo, nullptr, &imageView) == VK_SUCCESS;
	};

	for (size_t targetIndex = 0; targetIndex < deferred.GBufferImages.size(); ++targetIndex)
	{
		if (!createImage(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT,
			deferred.GBufferImages[targetIndex],
			deferred.GBufferMemories[targetIndex],
			deferred.GBufferImageViews[targetIndex]))
		{
			deferred.IsValid = false;
			return false;
		}
	}
	if (!createImage(
		VK_FORMAT_D32_SFLOAT,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT,
		deferred.DepthImage,
		deferred.DepthMemory,
		deferred.DepthImageView))
	{
		deferred.IsValid = false;
		return false;
	}
	if (!createImage(
		VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT,
		deferred.HdrColorImage,
		deferred.HdrColorMemory,
		deferred.HdrColorImageView))
	{
		deferred.IsValid = false;
		return false;
	}

	const VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 1.0f
	};
	if (vkCreateSampler(device, &samplerCreateInfo, nullptr, &deferred.GBufferSampler) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}
	if (vkCreateSampler(device, &samplerCreateInfo, nullptr, &deferred.HdrSampler) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}

	std::array<VkImageView, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 1> framebufferAttachments = {};
	for (size_t targetIndex = 0; targetIndex < deferred.GBufferImageViews.size(); ++targetIndex)
	{
		framebufferAttachments[targetIndex] = deferred.GBufferImageViews[targetIndex];
	}
	framebufferAttachments.back() = deferred.DepthImageView;
	const VkFramebufferCreateInfo framebufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = deferred.GeometryRenderPass,
		.attachmentCount = static_cast<uint32_t>(framebufferAttachments.size()),
		.pAttachments = framebufferAttachments.data(),
		.width = width,
		.height = height,
		.layers = 1
	};
	if (vkCreateFramebuffer(device, &framebufferCreateInfo, nullptr, &deferred.GeometryFramebuffer) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}

	const VkFramebufferCreateInfo lightingFramebufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = deferred.LightingRenderPass,
		.attachmentCount = 1,
		.pAttachments = &deferred.HdrColorImageView,
		.width = width,
		.height = height,
		.layers = 1
	};
	if (vkCreateFramebuffer(device, &lightingFramebufferCreateInfo, nullptr, &deferred.LightingFramebuffer) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}

	const VkDescriptorPoolSize poolSizes[] = {
		{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 },
		{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = static_cast<uint32_t>(Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 1) },
		{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 }
	};
	const VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
		.pPoolSizes = poolSizes
	};
	if (vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, nullptr, &deferred.LightingDescriptorPool) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}
	const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = deferred.LightingDescriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &deferred.LightingDescriptorSetLayout
	};
	if (vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &deferred.LightingDescriptorSet) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}

	const VkDescriptorPoolSize toneMapPoolSizes[] = {
		{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 },
		{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 }
	};
	const VkDescriptorPoolCreateInfo toneMapDescriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = static_cast<uint32_t>(std::size(toneMapPoolSizes)),
		.pPoolSizes = toneMapPoolSizes
	};
	if (vkCreateDescriptorPool(device, &toneMapDescriptorPoolCreateInfo, nullptr, &deferred.ToneMapDescriptorPool) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}
	const VkDescriptorSetAllocateInfo toneMapDescriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = deferred.ToneMapDescriptorPool,
		.descriptorSetCount = 1,
		.pSetLayouts = &deferred.ToneMapDescriptorSetLayout
	};
	if (vkAllocateDescriptorSets(device, &toneMapDescriptorSetAllocateInfo, &deferred.ToneMapDescriptorSet) != VK_SUCCESS)
	{
		deferred.IsValid = false;
		return false;
	}

	const VkDescriptorBufferInfo lightingBufferInfo = {
		.buffer = vulkanLightingBuffer->GetVkBuffer(),
		.offset = 0,
		.range = sizeof(DeferredLightingConstants)
	};
	const VkDescriptorBufferInfo lightBufferInfo = {
		.buffer = vulkanLightBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredLightBuffer ? m_StaticMeshRenderer.DeferredLightBuffer->GetSize() : sizeof(LightGpuData)
	};
	const VkDescriptorBufferInfo tileRangeBufferInfo = {
		.buffer = vulkanTileRangeBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredTileRangeBuffer ? m_StaticMeshRenderer.DeferredTileRangeBuffer->GetSize() : sizeof(DeferredTileLightRange)
	};
	const VkDescriptorBufferInfo tileIndexBufferInfo = {
		.buffer = vulkanTileIndexBuffer->GetVkBuffer(),
		.offset = 0,
		.range = m_StaticMeshRenderer.DeferredTileLightIndexBuffer ? m_StaticMeshRenderer.DeferredTileLightIndexBuffer->GetSize() : sizeof(uint32_t)
	};
	std::array<VkDescriptorImageInfo, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount> imageInfos = {};
	for (size_t targetIndex = 0; targetIndex < imageInfos.size(); ++targetIndex)
	{
		imageInfos[targetIndex] = {
			.sampler = deferred.GBufferSampler,
			.imageView = deferred.GBufferImageViews[targetIndex],
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};
	}
	std::array<VkWriteDescriptorSet, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 5> writes = {};
	writes[0] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &lightingBufferInfo
	};
	for (size_t targetIndex = 0; targetIndex < imageInfos.size(); ++targetIndex)
	{
		writes[targetIndex + 1] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = deferred.LightingDescriptorSet,
			.dstBinding = static_cast<uint32_t>(targetIndex + 1),
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &imageInfos[targetIndex]
		};
	}
	const VkDescriptorImageInfo shadowInfo = {
		.sampler = m_StaticMeshRenderer.Vulkan.Shadow.DepthSampler,
		.imageView = m_StaticMeshRenderer.Vulkan.Shadow.DepthImageView,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
	};
	writes[Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 1] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 5,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &shadowInfo
	};
	writes[Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 2] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 10,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &lightBufferInfo
	};
	writes[Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 3] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 11,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &tileRangeBufferInfo
	};
	writes[Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 4] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = deferred.LightingDescriptorSet,
		.dstBinding = 12,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &tileIndexBufferInfo
	};
	vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	deferred.Width = width;
	deferred.Height = height;
	deferred.IsValid = true;
	WriteVulkanToneMapDescriptor();
	return true;
}

void Engine::DestroyVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		auto savedMaterialTextures = std::move(m_StaticMeshRenderer.Vulkan.MaterialTextures);
		const size_t savedMaterialCount = m_StaticMeshRenderer.Vulkan.MaterialCount;
		auto savedEntityMaterials = std::move(m_StaticMeshRenderer.Vulkan.EntityMaterials);

		m_StaticMeshRenderer.Vulkan = {};

		m_StaticMeshRenderer.Vulkan.MaterialTextures = std::move(savedMaterialTextures);
		m_StaticMeshRenderer.Vulkan.MaterialCount = savedMaterialCount;
		m_StaticMeshRenderer.Vulkan.EntityMaterials = std::move(savedEntityMaterials);
		return;
	}

	DestroyVulkanDeferredResources();
	DestroyVulkanSkyboxResources();
	DestroyVulkanShadowResources();

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

	for (auto& [entityId, entityResources] : m_StaticMeshRenderer.Vulkan.EntityMaterials)
	{
		(void)entityId;
		if (entityResources.DescriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), entityResources.DescriptorPool, nullptr);
			entityResources.DescriptorPool = VK_NULL_HANDLE;
		}
		entityResources.DescriptorSets.clear();
	}

	// 파이프라인 관련 핸들만 초기화합니다.
	// Vulkan material texture는 DestroyTextureResources()가 담당하므로, 리사이즈 중에는 벡터를 보존해야
	// descriptor set과 pipeline만 재생성하면서 기존 텍스처를 다시 사용할 수 있습니다.
	auto savedMaterialTextures = std::move(m_StaticMeshRenderer.Vulkan.MaterialTextures);
	const size_t savedMaterialCount = m_StaticMeshRenderer.Vulkan.MaterialCount;
	auto savedEntityMaterials = std::move(m_StaticMeshRenderer.Vulkan.EntityMaterials);

	m_StaticMeshRenderer.Vulkan = {};

	m_StaticMeshRenderer.Vulkan.MaterialTextures = std::move(savedMaterialTextures);
	m_StaticMeshRenderer.Vulkan.MaterialCount = savedMaterialCount;
	m_StaticMeshRenderer.Vulkan.EntityMaterials = std::move(savedEntityMaterials);
}

void Engine::DestroyVulkanSkyboxResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		m_StaticMeshRenderer.Vulkan.SkyboxVertexShader = VK_NULL_HANDLE;
		m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader = VK_NULL_HANDLE;
		m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout = VK_NULL_HANDLE;
		m_StaticMeshRenderer.Vulkan.SkyboxPipeline = VK_NULL_HANDLE;
		return;
	}

	const VkDevice device = vulkanDevice->GetVkDevice();
	if (m_StaticMeshRenderer.Vulkan.SkyboxPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, m_StaticMeshRenderer.Vulkan.SkyboxPipeline, nullptr);
		m_StaticMeshRenderer.Vulkan.SkyboxPipeline = VK_NULL_HANDLE;
	}
	if (m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(device, m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout, nullptr);
		m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout = VK_NULL_HANDLE;
	}
	if (m_StaticMeshRenderer.Vulkan.SkyboxVertexShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, m_StaticMeshRenderer.Vulkan.SkyboxVertexShader, nullptr);
		m_StaticMeshRenderer.Vulkan.SkyboxVertexShader = VK_NULL_HANDLE;
	}
	if (m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader, nullptr);
		m_StaticMeshRenderer.Vulkan.SkyboxFragmentShader = VK_NULL_HANDLE;
	}
}

void Engine::DrawVulkanSkybox(const Editor::ViewportPanelState& viewport, const Camera& camera)
{
	if (!m_SkyboxSettings.Enabled)
	{
		return;
	}

	auto commandList = dynamic_cast<VulkanCommandList*>(m_Graphics.CommandList.get());
	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	if (!commandList ||
		commandBuffer == VK_NULL_HANDLE ||
		m_StaticMeshRenderer.Vulkan.SkyboxPipeline == VK_NULL_HANDLE ||
		m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout == VK_NULL_HANDLE)
	{
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	if (right <= left || bottom <= top)
	{
		return;
	}

	commandList->BeginSwapchainRenderPassForExternalCommands();
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), static_cast<float>(right - left), static_cast<float>(bottom - top));
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	const Rendering::SkyboxGpuConstants skyboxConstants = Rendering::BuildSkyboxGpuConstants(m_SkyboxSettings, camera);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.SkyboxPipeline);
	vkCmdPushConstants(
		commandBuffer,
		m_StaticMeshRenderer.Vulkan.SkyboxPipelineLayout,
		VK_SHADER_STAGE_FRAGMENT_BIT,
		0,
		static_cast<uint32_t>(sizeof(Rendering::SkyboxGpuConstants)),
		&skyboxConstants);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);
}

void Engine::DestroyVulkanShadowResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		m_StaticMeshRenderer.Vulkan.Shadow = {};
		return;
	}

	const VkDevice device = vulkanDevice->GetVkDevice();
	auto& shadow = m_StaticMeshRenderer.Vulkan.Shadow;
	if (shadow.Pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, shadow.Pipeline, nullptr);
	if (shadow.Framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, shadow.Framebuffer, nullptr);
	if (shadow.RenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, shadow.RenderPass, nullptr);
	if (shadow.VertexShader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shadow.VertexShader, nullptr);
	if (shadow.DepthSampler != VK_NULL_HANDLE) vkDestroySampler(device, shadow.DepthSampler, nullptr);
	if (shadow.DepthImageView != VK_NULL_HANDLE) vkDestroyImageView(device, shadow.DepthImageView, nullptr);
	if (shadow.DepthImage != VK_NULL_HANDLE) vkDestroyImage(device, shadow.DepthImage, nullptr);
	if (shadow.DepthMemory != VK_NULL_HANDLE) vkFreeMemory(device, shadow.DepthMemory, nullptr);
	shadow = {};
}

void Engine::DestroyVulkanDeferredResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Graphics.Device.get());
	if (!vulkanDevice)
	{
		m_StaticMeshRenderer.Vulkan.Deferred = {};
		return;
	}

	const VkDevice device = vulkanDevice->GetVkDevice();
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	if (deferred.GeometryPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, deferred.GeometryPipeline, nullptr);
	if (deferred.LightingPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, deferred.LightingPipeline, nullptr);
	if (deferred.ToneMapPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, deferred.ToneMapPipeline, nullptr);
	if (deferred.LightingPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, deferred.LightingPipelineLayout, nullptr);
	if (deferred.ToneMapPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, deferred.ToneMapPipelineLayout, nullptr);
	if (deferred.ToneMapDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, deferred.ToneMapDescriptorPool, nullptr);
	if (deferred.LightingDescriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, deferred.LightingDescriptorPool, nullptr);
	if (deferred.ToneMapDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, deferred.ToneMapDescriptorSetLayout, nullptr);
	if (deferred.LightingDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, deferred.LightingDescriptorSetLayout, nullptr);
	if (deferred.LightingFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, deferred.LightingFramebuffer, nullptr);
	if (deferred.GeometryFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, deferred.GeometryFramebuffer, nullptr);
	if (deferred.LightingRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, deferred.LightingRenderPass, nullptr);
	if (deferred.GeometryRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, deferred.GeometryRenderPass, nullptr);
	if (deferred.HdrSampler != VK_NULL_HANDLE) vkDestroySampler(device, deferred.HdrSampler, nullptr);
	if (deferred.GBufferSampler != VK_NULL_HANDLE) vkDestroySampler(device, deferred.GBufferSampler, nullptr);

	for (VkShaderModule shaderModule : {
		deferred.GeometryVertexShader,
		deferred.GeometryFragmentShader,
		deferred.LightingVertexShader,
		deferred.LightingFragmentShader,
		deferred.ToneMapVertexShader,
		deferred.ToneMapFragmentShader })
	{
		if (shaderModule != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
		}
	}

	if (deferred.HdrColorImageView != VK_NULL_HANDLE) vkDestroyImageView(device, deferred.HdrColorImageView, nullptr);
	if (deferred.HdrColorImage != VK_NULL_HANDLE) vkDestroyImage(device, deferred.HdrColorImage, nullptr);
	if (deferred.HdrColorMemory != VK_NULL_HANDLE) vkFreeMemory(device, deferred.HdrColorMemory, nullptr);
	for (size_t targetIndex = 0; targetIndex < deferred.GBufferImageViews.size(); ++targetIndex)
	{
		if (deferred.GBufferImageViews[targetIndex] != VK_NULL_HANDLE) vkDestroyImageView(device, deferred.GBufferImageViews[targetIndex], nullptr);
		if (deferred.GBufferImages[targetIndex] != VK_NULL_HANDLE) vkDestroyImage(device, deferred.GBufferImages[targetIndex], nullptr);
		if (deferred.GBufferMemories[targetIndex] != VK_NULL_HANDLE) vkFreeMemory(device, deferred.GBufferMemories[targetIndex], nullptr);
	}
	if (deferred.DepthImageView != VK_NULL_HANDLE) vkDestroyImageView(device, deferred.DepthImageView, nullptr);
	if (deferred.DepthImage != VK_NULL_HANDLE) vkDestroyImage(device, deferred.DepthImage, nullptr);
	if (deferred.DepthMemory != VK_NULL_HANDLE) vkFreeMemory(device, deferred.DepthMemory, nullptr);

	deferred = {};
}

void Engine::DrawVulkanShadowDepthPass(const Camera& camera)
{
	(void)camera;
	if (!m_ShadowFrameData.Enabled || !m_ShadowFrameData.HasDirectionalCaster || !EnsureVulkanShadowResources())
	{
		return;
	}

	auto commandList = dynamic_cast<VulkanCommandList*>(m_Graphics.CommandList.get());
	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	auto& shadow = m_StaticMeshRenderer.Vulkan.Shadow;
	if (!commandList || commandBuffer == VK_NULL_HANDLE || shadow.Framebuffer == VK_NULL_HANDLE || shadow.RenderPass == VK_NULL_HANDLE || shadow.Pipeline == VK_NULL_HANDLE)
	{
		return;
	}

	commandList->EndRenderPassForExternalCommands();
	m_Graphics.CommandList->SetVertexBuffer(m_StaticMeshRenderer.VertexBuffer.get());
	m_Graphics.CommandList->SetIndexBuffer(m_StaticMeshRenderer.IndexBuffer.get());

	const VkClearValue clearValue = {
		.depthStencil = { 1.0f, 0 }
	};
	const VkRenderPassBeginInfo renderPassBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = shadow.RenderPass,
		.framebuffer = shadow.Framebuffer,
		.renderArea = {
			.offset = { 0, 0 },
			.extent = { shadow.Size, shadow.Size }
		},
		.clearValueCount = 1,
		.pClearValues = &clearValue
	};
	vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_Graphics.CommandList->SetViewport(0.0f, 0.0f, static_cast<float>(shadow.Size), static_cast<float>(shadow.Size));
	m_Graphics.CommandList->SetScissorRect(0, 0, static_cast<long>(shadow.Size), static_cast<long>(shadow.Size));
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow.Pipeline);

	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_RenderState.RenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		const std::vector<VkDescriptorSet>* selectedDescriptorSets = &m_StaticMeshRenderer.Vulkan.DescriptorSets;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Vulkan.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Vulkan.EntityMaterials.end()
			&& !entityMaterialIt->second.DescriptorSets.empty())
		{
			selectedDescriptorSets = &entityMaterialIt->second.DescriptorSets;
		}
		if (!selectedDescriptorSets || selectedDescriptorSets->empty())
		{
			continue;
		}

		const uint64_t cameraOffset = UpdateShadowCameraBuffer(entityId);
		if (cameraOffset == InvalidCameraConstantOffset())
		{
			continue;
		}
		const uint32_t dynamicOffset = static_cast<uint32_t>(cameraOffset);
		const VkDescriptorSet descriptorSet = selectedDescriptorSets->front();
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			m_StaticMeshRenderer.Vulkan.PipelineLayout,
			0,
			1,
			&descriptorSet,
			1,
			&dynamicOffset);

		if (meshAsset->Submeshes.empty())
		{
			if (!IsMaterialTransparent(entityId, 0))
			{
				RecordIndexedDraw(Rendering::DrawSubmissionKind::Shadow, static_cast<uint32_t>(meshAsset->Indices.size()), 1);
				m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			}
			continue;
		}

		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				RecordIndexedDraw(Rendering::DrawSubmissionKind::Shadow, submesh.IndexCount, 1);
				m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
			}
		}
	}

	vkCmdEndRenderPass(commandBuffer);
}

void Engine::DrawVulkanToneMapPass(const Editor::ViewportPanelState& viewport)
{
	auto commandList = dynamic_cast<VulkanCommandList*>(m_Graphics.CommandList.get());
	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	if (!commandList ||
		commandBuffer == VK_NULL_HANDLE ||
		deferred.ToneMapPipeline == VK_NULL_HANDLE ||
		deferred.ToneMapPipelineLayout == VK_NULL_HANDLE ||
		deferred.ToneMapDescriptorSet == VK_NULL_HANDLE)
	{
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	const float width = static_cast<float>(right - left);
	const float height = static_cast<float>(bottom - top);
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	commandList->BeginSwapchainRenderPassForExternalCommands();
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, deferred.ToneMapPipeline);
	vkCmdBindDescriptorSets(
		commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		deferred.ToneMapPipelineLayout,
		0,
		1,
		&deferred.ToneMapDescriptorSet,
		0,
		nullptr);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);
}

void Engine::DrawVulkanDeferredTriangle(const Editor::ViewportPanelState& viewport, const Camera& camera, const DeferredPassTimingIndices& timings)
{
	if (!EnsureVulkanDeferredResources())
	{
		DrawVulkanTriangle(camera);
		return;
	}

	auto commandList = dynamic_cast<VulkanCommandList*>(m_Graphics.CommandList.get());
	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	auto& deferred = m_StaticMeshRenderer.Vulkan.Deferred;
	if (!commandList || commandBuffer == VK_NULL_HANDLE || deferred.GeometryFramebuffer == VK_NULL_HANDLE || deferred.LightingDescriptorSet == VK_NULL_HANDLE)
	{
		DrawVulkanTriangle(camera);
		return;
	}

	const long left = (std::max)(0L, static_cast<long>(std::floor(viewport.Left)));
	const long top = (std::max)(0L, static_cast<long>(std::floor(viewport.Top)));
	const long right = (std::min)(static_cast<long>(m_ClientWidth), static_cast<long>(std::ceil(viewport.Left + viewport.Width)));
	const long bottom = (std::min)(static_cast<long>(m_ClientHeight), static_cast<long>(std::ceil(viewport.Top + viewport.Height)));
	const float width = static_cast<float>(right - left);
	const float height = static_cast<float>(bottom - top);

	const auto geometryBegin = std::chrono::steady_clock::now();
	commandList->EndRenderPassForExternalCommands();
	std::array<VkClearValue, Rendering::VulkanStaticMeshResources::DeferredResources::GBufferCount + 1> clearValues = {};
	clearValues.back().depthStencil = { 1.0f, 0 };
	const VkRenderPassBeginInfo gbufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = deferred.GeometryRenderPass,
		.framebuffer = deferred.GeometryFramebuffer,
		.renderArea = {
			.offset = { 0, 0 },
			.extent = { deferred.Width, deferred.Height }
		},
		.clearValueCount = static_cast<uint32_t>(clearValues.size()),
		.pClearValues = clearValues.data()
	};
	vkCmdBeginRenderPass(commandBuffer, &gbufferBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, deferred.GeometryPipeline);

	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}
		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		const std::vector<VkDescriptorSet>* selectedDescriptorSets = &m_StaticMeshRenderer.Vulkan.DescriptorSets;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Vulkan.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Vulkan.EntityMaterials.end()
			&& !entityMaterialIt->second.DescriptorSets.empty())
		{
			selectedDescriptorSets = &entityMaterialIt->second.DescriptorSets;
		}
		if (!selectedDescriptorSets || selectedDescriptorSets->empty())
		{
			continue;
		}

		auto drawOpaqueMaterial = [&](uint32_t indexCount, uint32_t indexOffset, size_t materialIndex)
		{
			const VkDescriptorSet descriptorSet = (*selectedDescriptorSets)[materialIndex];
			const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, false);
			if (cameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			const uint32_t dynamicOffset = static_cast<uint32_t>(cameraOffset);
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				1,
				&dynamicOffset);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::DeferredGeometry, indexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(indexCount, 1, indexOffset, 0, 0);
		};

		if (meshAsset->Submeshes.empty())
		{
			if (!IsMaterialTransparent(entityId, 0))
			{
				drawOpaqueMaterial(static_cast<uint32_t>(meshAsset->Indices.size()), 0, 0);
			}
			continue;
		}
		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				const size_t materialIndex = submesh.MaterialIndex < selectedDescriptorSets->size() ? submesh.MaterialIndex : 0;
				drawOpaqueMaterial(submesh.IndexCount, submesh.IndexOffset, materialIndex);
			}
		}
	}
	vkCmdEndRenderPass(commandBuffer);

	const auto geometryEnd = std::chrono::steady_clock::now();
	m_RenderGraph.SetPassCpuTime(timings.Geometry, std::chrono::duration<double, std::milli>(geometryEnd - geometryBegin).count());

	MeasureRenderGraphPass(m_RenderGraph, timings.TileCulling, [this, &camera, left, top, right, bottom]()
		{
			static_cast<void>(UpdateDeferredLightTiles(
				camera,
				static_cast<uint32_t>((std::max)(left, 0L)),
				static_cast<uint32_t>((std::max)(top, 0L)),
				static_cast<uint32_t>((std::max)(right - left, 1L)),
				static_cast<uint32_t>((std::max)(bottom - top, 1L)),
				static_cast<uint32_t>((std::max)(m_ClientWidth, 1)),
				static_cast<uint32_t>((std::max)(m_ClientHeight, 1))));
		});

	const auto lightingBegin = std::chrono::steady_clock::now();
	const auto cameraPosition = camera.GetPosition();
	DeferredLightingConstants lightingConstants = {};
	lightingConstants.CameraPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f };
	lightingConstants.AmbientColorIntensity = {
		std::clamp(m_AmbientColor.x, 0.0f, 4.0f),
		std::clamp(m_AmbientColor.y, 0.0f, 4.0f),
		std::clamp(m_AmbientColor.z, 0.0f, 4.0f),
		std::clamp(m_AmbientIntensity, 0.0f, 2.0f)
	};
	lightingConstants.ExposureDebug = {
		std::clamp(m_Exposure, 0.05f, 8.0f),
		static_cast<float>(static_cast<uint32_t>(m_MaterialDebugView)),
		0.0f,
		0.0f
	};
	lightingConstants.LightCountParams = {
		static_cast<float>(m_StaticMeshRenderer.DeferredLightCount),
		static_cast<float>(m_StaticMeshRenderer.DeferredTileCountX),
		static_cast<float>(m_StaticMeshRenderer.DeferredTileCountY),
		m_StaticMeshRenderer.DeferredTileCountX > 0 && m_StaticMeshRenderer.DeferredTileCountY > 0 ? 1.0f : 0.0f
	};
	lightingConstants.ScreenSize = {
		static_cast<float>((std::max)(m_ClientWidth, 1)),
		static_cast<float>((std::max)(m_ClientHeight, 1)),
		1.0f / static_cast<float>((std::max)(m_ClientWidth, 1)),
		1.0f / static_cast<float>((std::max)(m_ClientHeight, 1))
	};
	Rendering::ShadowFrameData shadowData = m_ShadowFrameData;
	if (!m_StaticMeshRenderer.Vulkan.Shadow.IsValid)
	{
		shadowData.Params.x = 0.0f;
	}
	lightingConstants.ShadowViewProjection = shadowData.LightViewProjection;
	lightingConstants.ShadowParams = shadowData.Params;
	lightingConstants.ShadowDirection = shadowData.DirectionToLight;
	lightingConstants.Skybox = Rendering::BuildSkyboxGpuConstants(m_SkyboxSettings, camera);
	if (WriteDeferredLightingConstants(lightingConstants) == InvalidCameraConstantOffset())
	{
		return;
	}

	VkClearValue hdrClearValue = {};
	const std::array<float, 4> hdrClear = BuildSkyClearColor(m_SkyboxSettings);
	hdrClearValue.color.float32[0] = hdrClear[0];
	hdrClearValue.color.float32[1] = hdrClear[1];
	hdrClearValue.color.float32[2] = hdrClear[2];
	hdrClearValue.color.float32[3] = hdrClear[3];
	const VkRenderPassBeginInfo lightingBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = deferred.LightingRenderPass,
		.framebuffer = deferred.LightingFramebuffer,
		.renderArea = {
			.offset = { 0, 0 },
			.extent = { deferred.Width, deferred.Height }
		},
		.clearValueCount = 1,
		.pClearValues = &hdrClearValue
	};
	vkCmdBeginRenderPass(commandBuffer, &lightingBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
	m_Graphics.CommandList->SetViewport(static_cast<float>(left), static_cast<float>(top), width, height);
	m_Graphics.CommandList->SetScissorRect(left, top, right, bottom);
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, deferred.LightingPipeline);
	vkCmdBindDescriptorSets(
		commandBuffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		deferred.LightingPipelineLayout,
		0,
		1,
		&deferred.LightingDescriptorSet,
		0,
	nullptr);
	RecordFullscreenDraw(Rendering::DrawSubmissionKind::Fullscreen, 3, 1);
	m_Graphics.CommandList->DrawInstanced(3, 1, 0, 0);
	vkCmdEndRenderPass(commandBuffer);
	const auto lightingEnd = std::chrono::steady_clock::now();
	m_RenderGraph.SetPassCpuTime(timings.Lighting, std::chrono::duration<double, std::milli>(lightingEnd - lightingBegin).count());

	MeasureRenderGraphPass(m_RenderGraph, timings.PostProcess, [this, &viewport]()
		{
			DrawVulkanToneMapPass(viewport);
		});
	MeasureRenderGraphPass(m_RenderGraph, timings.Transparency, [this, &camera]()
		{
			DrawVulkanForwardTransparentPass(camera);
		});
}

void Engine::DrawVulkanForwardTransparentPass(const Camera& camera)
{
	if (m_StaticMeshRenderer.Vulkan.TransparentPipeline == VK_NULL_HANDLE)
	{
		return;
	}

	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_Graphics.CommandList->GetNativeResource());
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.TransparentPipeline);
	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}
		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		const std::vector<VkDescriptorSet>* selectedDescriptorSets = &m_StaticMeshRenderer.Vulkan.DescriptorSets;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Vulkan.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Vulkan.EntityMaterials.end()
			&& !entityMaterialIt->second.DescriptorSets.empty())
		{
			selectedDescriptorSets = &entityMaterialIt->second.DescriptorSets;
		}
		if (!selectedDescriptorSets || selectedDescriptorSets->empty())
		{
			continue;
		}

		auto drawTransparentMaterial = [&](uint32_t indexCount, uint32_t indexOffset, size_t materialIndex)
		{
			const VkDescriptorSet descriptorSet = (*selectedDescriptorSets)[materialIndex];
			const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, false);
			if (cameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			const uint32_t dynamicOffset = static_cast<uint32_t>(cameraOffset);
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				1,
				&dynamicOffset);
			RecordIndexedDraw(Rendering::DrawSubmissionKind::Transparent, indexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(indexCount, 1, indexOffset, 0, 0);
		};

		if (meshAsset->Submeshes.empty())
		{
			if (IsMaterialTransparent(entityId, 0))
			{
				drawTransparentMaterial(static_cast<uint32_t>(meshAsset->Indices.size()), 0, 0);
			}
			continue;
		}
		for (const auto& submesh : meshAsset->Submeshes)
		{
			if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
			{
				const size_t materialIndex = submesh.MaterialIndex < selectedDescriptorSets->size() ? submesh.MaterialIndex : 0;
				drawTransparentMaterial(submesh.IndexCount, submesh.IndexOffset, materialIndex);
			}
		}
	}
}

void Engine::DrawVulkanTriangle(const Camera& camera)
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

	const bool drawTransparentInSecondPass = m_StaticMeshRenderer.Vulkan.TransparentPipeline != VK_NULL_HANDLE;
	const bool drawOpaquePass = true;

	// Vulkan은 현재 열린 render pass 안에서 그래픽 파이프라인을 바인딩하고 draw를 기록합니다.
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.Pipeline);

	const Scene& runtimeScene = GetRuntimeScene();
	for (EntityId entityId : m_ViewportVisibleRenderEntities)
	{
		if (!runtimeScene.IsMeshEnabled(entityId))
		{
			continue;
		}

		const Asset::StaticMeshAsset* meshAsset = runtimeScene.GetMeshAsset(entityId);
		if (!meshAsset)
		{
			continue;
		}

		UploadEntityGeometry(entityId);
		const uint64_t cameraOffset = UpdateCameraBuffer(entityId, camera);
		if (cameraOffset == InvalidCameraConstantOffset())
		{
			continue;
		}
		const uint32_t cameraDynamicOffset = static_cast<uint32_t>(cameraOffset);

		const std::vector<VkDescriptorSet>* selectedDescriptorSets = &m_StaticMeshRenderer.Vulkan.DescriptorSets;
		if (auto entityMaterialIt = m_StaticMeshRenderer.Vulkan.EntityMaterials.find(entityId);
			entityMaterialIt != m_StaticMeshRenderer.Vulkan.EntityMaterials.end()
			&& !entityMaterialIt->second.DescriptorSets.empty())
		{
			selectedDescriptorSets = &entityMaterialIt->second.DescriptorSets;
		}
		if (!selectedDescriptorSets || selectedDescriptorSets->empty())
		{
			continue;
		}

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
			const VkDescriptorSet descriptorSet = selectedDescriptorSets->front();
			uint32_t selectedCameraDynamicOffset = cameraDynamicOffset;
			if (entityIsTransparent)
			{
				const uint64_t transparentCameraOffset = UpdateCameraBuffer(entityId, camera, 0, false);
				if (transparentCameraOffset == InvalidCameraConstantOffset())
				{
					continue;
				}
				selectedCameraDynamicOffset = static_cast<uint32_t>(transparentCameraOffset);
			}
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				1,
				&selectedCameraDynamicOffset);
			RecordIndexedDraw(entityIsTransparent ? Rendering::DrawSubmissionKind::Transparent : Rendering::DrawSubmissionKind::Opaque, static_cast<uint32_t>(meshAsset->Indices.size()), 1);
			m_Graphics.CommandList->DrawIndexedInstanced(static_cast<uint32_t>(meshAsset->Indices.size()), 1, 0, 0, 0);
			continue;
		}

		auto drawSubmesh = [&](const Asset::StaticMeshSubmesh& submesh, bool useDeferredLighting)
		{
			const size_t materialIndex = submesh.MaterialIndex < selectedDescriptorSets->size() ? submesh.MaterialIndex : 0;
			const VkDescriptorSet descriptorSet = (*selectedDescriptorSets)[materialIndex];
			const uint64_t materialCameraOffset = UpdateCameraBuffer(entityId, camera, materialIndex, useDeferredLighting);
			if (materialCameraOffset == InvalidCameraConstantOffset())
			{
				return;
			}
			const uint32_t materialCameraDynamicOffset = static_cast<uint32_t>(materialCameraOffset);

			// Vulkan 경로는 submesh.MaterialIndex에 대응하는 descriptor set을 바인딩해 material별 texture를 선택합니다.
			// 그런 다음 해당 submesh의 index 범위만 DrawIndexedInstanced로 기록해 멀티 머티리얼 메시를 올바르게 렌더링합니다.
			vkCmdBindDescriptorSets(
				commandBuffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				m_StaticMeshRenderer.Vulkan.PipelineLayout,
				0,
				1,
				&descriptorSet,
				1,
				&materialCameraDynamicOffset);
			RecordIndexedDraw(IsMaterialTransparent(entityId, materialIndex) ? Rendering::DrawSubmissionKind::Transparent : Rendering::DrawSubmissionKind::Opaque, submesh.IndexCount, 1);
			m_Graphics.CommandList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
		};

		if (drawOpaquePass)
		{
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.Pipeline);
			// Vulkan의 불투명 패스는 forward/deferred/forward+ 공통으로 먼저 실행합니다.
			// deferred 모드에서는 같은 geometry draw를 사용하되, fragment shader가 별도 unlimited light buffer를 순회합니다.
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (!IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh, m_RenderMode == RenderMode::Deferred);
				}
			}
		}

		if (drawTransparentInSecondPass)
		{
			// Vulkan 투명 패스는 alpha blend가 켜진 전용 파이프라인으로 그려야 유리 오브젝트가 반투명하게 합성됩니다.
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_StaticMeshRenderer.Vulkan.TransparentPipeline);
			// Deferred 모드의 투명 물체는 G-Buffer가 아니라 forward fallback으로 그립니다.
			// 따라서 투명 패스의 camera constants는 항상 8-light forward 배열만 사용합니다.
			for (const auto& submesh : meshAsset->Submeshes)
			{
				if (IsMaterialTransparent(entityId, submesh.MaterialIndex))
				{
					drawSubmesh(submesh, false);
				}
			}
		}
	}
}

bool Engine::CreateTriangleVertexBuffer()
{
	const Asset::StaticMeshAsset* spiderMesh = RenderSystem::GetPrimaryRenderableMesh(GetRuntimeScene());
	if (!spiderMesh || spiderMesh->Vertices.empty())
	{
		const BufferDesc bufferDesc = {
			.Size = sizeof(Asset::StaticMeshVertex),
			.Stride = sizeof(Asset::StaticMeshVertex),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.VertexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
		return m_StaticMeshRenderer.VertexBuffer != nullptr;
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
	const Asset::StaticMeshAsset* spiderMesh = RenderSystem::GetPrimaryRenderableMesh(GetRuntimeScene());
	if (!spiderMesh || spiderMesh->Indices.empty())
	{
		const BufferDesc bufferDesc = {
			.Size = sizeof(uint32_t),
			.Stride = sizeof(uint32_t),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.IndexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
		return m_StaticMeshRenderer.IndexBuffer != nullptr;
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

bool Engine::EnsureGeometryBufferCapacity(size_t vertexCount, size_t indexCount)
{
	if (!m_Graphics.Device || vertexCount == 0 || indexCount == 0)
	{
		return false;
	}

	const uint64_t requiredVertexBytes = static_cast<uint64_t>(vertexCount * sizeof(Asset::StaticMeshVertex));
	const uint64_t requiredIndexBytes = static_cast<uint64_t>(indexCount * sizeof(uint32_t));

	if (!m_StaticMeshRenderer.VertexBuffer || m_StaticMeshRenderer.VertexBuffer->GetSize() < requiredVertexBytes)
	{
		const BufferDesc bufferDesc = {
			.Size = requiredVertexBytes,
			.Stride = sizeof(Asset::StaticMeshVertex),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.VertexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
		if (!m_StaticMeshRenderer.VertexBuffer)
		{
			return false;
		}
	}

	if (!m_StaticMeshRenderer.IndexBuffer || m_StaticMeshRenderer.IndexBuffer->GetSize() < requiredIndexBytes)
	{
		const BufferDesc bufferDesc = {
			.Size = requiredIndexBytes,
			.Stride = sizeof(uint32_t),
			.Heap = HeapType::Upload,
			.InitialState = ResourceState::GenericRead
		};
		m_StaticMeshRenderer.IndexBuffer = m_Graphics.Device->CreateBuffer(bufferDesc);
		if (!m_StaticMeshRenderer.IndexBuffer)
		{
			return false;
		}
	}

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
