#include "Engine.h"
#include "Asset/AssimpModelLoader.h"
#include "Input/InputSystem.h"
#include "Resource.h"
#include "Utilities/ShaderUtils.h"

#include "Platform/DirectX12/DX12Buffer.h"
#include "Platform/DirectX12/DX12Device.h"
#include "Platform/DirectX12/d3dx12.h"
#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanDevice.h"

#include <d3dcompiler.h>
#include <glslang/Include/glslang_c_interface.h>
#include <stb_image.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace
{
	// 렌더 윈도우 클래스 등록 및 관리
	LRESULT CALLBACK RenderWindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
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

	// 삼각형 정점 데이터
	struct TriangleVertex
	{
		float Position[2];
		float Color[4];
	};

	struct alignas(16) CameraConstants
	{
		DirectX::XMFLOAT4X4 ViewProjection = {};
		DirectX::XMFLOAT4 CameraPosition = {};
		DirectX::XMFLOAT4 DebugOptions = {};
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

	// 셰이더 파일 경로
	[[nodiscard]] constexpr std::string_view GetDx12ShaderPath() noexcept
	{
		return "Src/Platform/DirectX12/Shaders/Triangle.hlsl";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanVertexShaderPath() noexcept
	{
		return "Src/Platform/Vulkan/Shaders/Triangle.vert";
	}

	[[nodiscard]] constexpr std::string_view GetVulkanFragmentShaderPath() noexcept
	{
		return "Src/Platform/Vulkan/Shaders/Triangle.frag";
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

	[[nodiscard]] DirectX::XMMATRIX LoadMatrix(const DirectX::XMFLOAT4X4& matrix)
	{
		return DirectX::XMLoadFloat4x4(&matrix);
	}

	[[nodiscard]] DirectX::XMVECTOR InterpolateVectorKey(const std::vector<Asset::AnimationVectorKey>& keys, double timeTicks)
	{
		if (keys.empty())
		{
			return DirectX::XMVectorZero();
		}

		if (keys.size() == 1 || timeTicks <= keys.front().TimeTicks)
		{
			return DirectX::XMLoadFloat3(&keys.front().Value);
		}

		for (size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex)
		{
			if (timeTicks < keys[keyIndex + 1].TimeTicks)
			{
				const double delta = keys[keyIndex + 1].TimeTicks - keys[keyIndex].TimeTicks;
				const float factor = delta > 0.0 ? static_cast<float>((timeTicks - keys[keyIndex].TimeTicks) / delta) : 0.0f;
				return DirectX::XMVectorLerp(
					DirectX::XMLoadFloat3(&keys[keyIndex].Value),
					DirectX::XMLoadFloat3(&keys[keyIndex + 1].Value),
					factor);
			}
		}

		return DirectX::XMLoadFloat3(&keys.back().Value);
	}

	[[nodiscard]] DirectX::XMVECTOR InterpolateQuaternionKey(const std::vector<Asset::AnimationQuaternionKey>& keys, double timeTicks)
	{
		if (keys.empty())
		{
			return DirectX::XMQuaternionIdentity();
		}

		if (keys.size() == 1 || timeTicks <= keys.front().TimeTicks)
		{
			return DirectX::XMLoadFloat4(&keys.front().Value);
		}

		for (size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex)
		{
			if (timeTicks < keys[keyIndex + 1].TimeTicks)
			{
				const double delta = keys[keyIndex + 1].TimeTicks - keys[keyIndex].TimeTicks;
				const float factor = delta > 0.0 ? static_cast<float>((timeTicks - keys[keyIndex].TimeTicks) / delta) : 0.0f;
				return DirectX::XMQuaternionSlerp(
					DirectX::XMLoadFloat4(&keys[keyIndex].Value),
					DirectX::XMLoadFloat4(&keys[keyIndex + 1].Value),
					factor);
			}
		}

		return DirectX::XMLoadFloat4(&keys.back().Value);
	}

	[[nodiscard]] DirectX::XMMATRIX BuildAnimatedLocalTransform(const Asset::SkeletonNode& node, const Asset::AnimationClip& clip, const Asset::StaticMeshAsset& meshAsset, double timeTicks)
	{
		auto channelIndex = clip.ChannelIndices.find(node.Name);
		if (channelIndex == clip.ChannelIndices.end())
		{
			return LoadMatrix(node.LocalBindTransform);
		}

		const Asset::AnimationChannel& channel = clip.Channels[channelIndex->second];
		const DirectX::XMVECTOR translation = channel.PositionKeys.empty()
			? DirectX::XMVectorZero()
			: InterpolateVectorKey(channel.PositionKeys, timeTicks);
		const DirectX::XMVECTOR rotation = channel.RotationKeys.empty()
			? DirectX::XMQuaternionIdentity()
			: InterpolateQuaternionKey(channel.RotationKeys, timeTicks);
		const DirectX::XMVECTOR scaling = channel.ScalingKeys.empty()
			? DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f)
			: InterpolateVectorKey(channel.ScalingKeys, timeTicks);

		UNREFERENCED_PARAMETER(meshAsset);
		return DirectX::XMMatrixScalingFromVector(scaling) *
			DirectX::XMMatrixRotationQuaternion(rotation) *
			DirectX::XMMatrixTranslationFromVector(translation);
	}

	void ComputeGlobalNodeTransforms(const Asset::StaticMeshAsset& meshAsset, const std::vector<DirectX::XMMATRIX>& localTransforms, uint32_t nodeIndex, const DirectX::XMMATRIX& parentTransform, std::vector<DirectX::XMMATRIX>& globalTransforms)
	{
		globalTransforms[nodeIndex] = localTransforms[nodeIndex] * parentTransform;
		for (uint32_t childIndex : meshAsset.Nodes[nodeIndex].Children)
		{
			ComputeGlobalNodeTransforms(meshAsset, localTransforms, childIndex, globalTransforms[nodeIndex], globalTransforms);
		}
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

	if (!SwitchGraphicsAPI(m_CurrentApi))
	{
		return false;
	}

	return true;
}

LRESULT Engine::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_COMMAND)
	{
		switch (LOWORD(wParam))
		{
		case IDM_RENDERER_DX12:
			if (m_CurrentApi != GraphicsAPI::DirectX12)
			{
				const GraphicsAPI previousApi = m_CurrentApi;
				if (!SwitchGraphicsAPI(GraphicsAPI::DirectX12))
				{
					SwitchGraphicsAPI(previousApi);
				}
			}
			return 0;

		case IDM_RENDERER_VULKAN:
			if (m_CurrentApi != GraphicsAPI::Vulkan)
			{
				const GraphicsAPI previousApi = m_CurrentApi;
				if (!SwitchGraphicsAPI(GraphicsAPI::Vulkan))
				{
					SwitchGraphicsAPI(previousApi);
				}
			}
			return 0;

		case IDM_VIEW_UV_DEBUG:
			m_IsUvDebugViewEnabled = !m_IsUvDebugViewEnabled;
			UpdateRendererMenuState();
			UpdateWindowTitleWithFps();
			return 0;
		}
	}

	return GameApp::MsgProc(hWnd, msg, wParam, lParam);
}

void Engine::Update(float deltaTime)
{
	UpdateAnimatedMesh(deltaTime);
	m_Camera.Update(deltaTime, m_hRenderWnd ? m_hRenderWnd : m_hMainWnd);
}

void Engine::Render()
{
	if (!m_Device || !m_CmdList)
	{
		return;
	}

	// 커맨드 리스트 리셋 및 기본 설정
	m_CmdList->Reset();
	m_CmdList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
	m_CmdList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

	// 백버퍼를 렌더타겟 상태로 전환
	IGpuResource* backBuffer = m_Device->GetBackBufferResource();
	m_CmdList->ResourceBarrier(backBuffer, ResourceState::Present, ResourceState::RenderTarget);

	// 렌더타겟 설정
	void* rtvHandle = m_Device->GetCurrentBackBufferRTV();
	void* dsvHandle = m_Device->GetDepthStencilView();
	m_CmdList->SetRenderTargets(rtvHandle, dsvHandle);

	// 애니메이션 효과를 위한 시간 기반 색상 계산
	const auto now = std::chrono::steady_clock::now();
	const std::chrono::duration<float> elapsed = now - m_RenderStartTime;
	const float color = (std::sinf(elapsed.count() * 2.0f) + 1.0f) * 0.5f;

	// 화면 클리어
	const float clearColor[4] = { color, 0.2f, 0.4f, 1.0f };
	m_CmdList->ClearRenderTarget(rtvHandle, clearColor);
	m_CmdList->ClearDepthStencil(dsvHandle, 1.0f, 0);
	UpdateCameraBuffer();

	// 정점 버퍼 바인딩 및 삼각형 그리기
	m_CmdList->SetVertexBuffer(m_VertexBuffer);
	m_CmdList->SetIndexBuffer(m_IndexBuffer);

	if (m_CurrentApi == GraphicsAPI::DirectX12)
	{
		DrawDx12Triangle();
	}
	else
	{
		DrawVulkanTriangle();
	}

	// 백버퍼를 Present 상태로 전환
	m_CmdList->ResourceBarrier(backBuffer, ResourceState::RenderTarget, ResourceState::Present);
	m_CmdList->Close();

	// 커맨드 리스트 실행 및 화면 출력
	m_Device->ExecuteCommandList(m_CmdList);
	m_Device->Present();
	m_Device->MoveToNextFrame();

	UpdateWindowTitleWithFps();
}

void Engine::OnResize()
{
	ResizeRenderWindow();
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);

	if (m_Device)
	{
		m_Device->Resize(m_ClientWidth, m_ClientHeight);

		// Vulkan 파이프라인은 렌더패스 호환성을 유지하더라도 리사이즈 시 재생성해 두는 편이 학습상 명확합니다.
		if (m_CurrentApi == GraphicsAPI::Vulkan)
		{
			DestroyVulkanTriangleResources();
			CreateVulkanTriangleResources();
		}
	}
}

bool Engine::SwitchGraphicsAPI(GraphicsAPI api)
{
	std::string switchMessage = "SwitchGraphicsAPI: ";
	switchMessage.append(GraphicsApiToString(m_CurrentApi));
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

	m_CurrentApi = api;
	m_Device = IGraphicsDevice::Create(api, m_hRenderWnd, m_ClientWidth, m_ClientHeight);
	if (!m_Device || !m_Device->Init())
	{
		LogEngineTrace("SwitchGraphicsAPI failed during device initialization.");
		ShutdownGraphics();
		MessageBoxW(m_hMainWnd, L"그래픽 디바이스 초기화에 실패했습니다.", L"Graphics API Error", MB_OK | MB_ICONERROR);
		return false;
	}

	m_CmdList = m_Device->CreateCommandList();
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

	m_WindowTitleBase = api == GraphicsAPI::DirectX12 
		? L"EnginePlatformer - DirectX12" 
		: L"EnginePlatformer - Vulkan";
	m_Camera.SetLens(DirectX::XM_PIDIV4, static_cast<float>(m_ClientWidth) / static_cast<float>((std::max)(m_ClientHeight, 1)), 0.1f, 1000.0f);
	m_RenderStartTime = std::chrono::steady_clock::now();
	ResetFpsCounter();
	UpdateRendererMenuState();
	LogEngineTrace("SwitchGraphicsAPI completed successfully.");

	return true;
}

void Engine::ShutdownGraphics()
{
	if (m_Device)
	{
		LogEngineTrace("ShutdownGraphics waiting for GPU.");
		m_Device->WaitForGPU();
	}

	DestroyTextureResources();
	DestroyDx12TriangleResources();
	DestroyVulkanTriangleResources();
	DestroyTextureResources();

	delete m_VertexBuffer;
	m_VertexBuffer = nullptr;

	delete m_IndexBuffer;
	m_IndexBuffer = nullptr;

	delete m_CameraBuffer;
	m_CameraBuffer = nullptr;

	delete m_CmdList;
	m_CmdList = nullptr;

	delete m_Device;
	m_Device = nullptr;

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
	const std::filesystem::path spiderDirectory = "Assets/Models/Spider";
	if (!std::filesystem::exists(spiderDirectory))
	{
		MessageBoxW(m_hMainWnd, L"Assets/Models/Spider 폴더를 찾지 못했습니다.", L"Asset Error", MB_OK | MB_ICONERROR);
		return false;
	}

	Asset::AssimpModelLoader modelLoader;
	std::filesystem::path selectedModelPath;

	for (const auto& entry : std::filesystem::directory_iterator(spiderDirectory))
	{
		if (!entry.is_regular_file())
		{
			continue;
		}

		const auto extension = entry.path().extension().wstring();
		if (extension != L".fbx" && extension != L".obj" && extension != L".gltf" && extension != L".glb")
		{
			continue;
		}

		const bool isAnimated = modelLoader.HasAnimation(entry.path().string());
		auto loadedMesh = isAnimated
			? modelLoader.LoadAnimatedMesh(entry.path().string())
			: modelLoader.LoadStaticMesh(entry.path().string());

		if (loadedMesh && !loadedMesh->Vertices.empty() && !loadedMesh->Indices.empty())
		{
			selectedModelPath = entry.path();
			m_StaticMeshAsset = std::move(loadedMesh);

			if (isAnimated)
			{
				LogEngineTrace("Auto-routed Spider asset to animated loader.");
			}
			else
			{
				LogEngineTrace("Auto-routed Spider asset to static loader.");
			}
			break;
		}
	}

	if (selectedModelPath.empty())
	{
		MessageBoxW(m_hMainWnd, L"Spider 폴더에서 로드 가능한 모델을 찾지 못했습니다.", L"Asset Error", MB_OK | MB_ICONERROR);
		return false;
	}

	if (!m_StaticMeshAsset || m_StaticMeshAsset->Vertices.empty() || m_StaticMeshAsset->Indices.empty())
	{
		MessageBoxW(m_hMainWnd, L"Spider 정적 메시를 로드하지 못했습니다.", L"Asset Error", MB_OK | MB_ICONERROR);
		return false;
	}

	DirectX::XMFLOAT2 minUv(
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)());
	DirectX::XMFLOAT2 maxUv(
		(std::numeric_limits<float>::lowest)(),
		(std::numeric_limits<float>::lowest)());
	uint32_t outOfRangeUvCount = 0;
	for (const auto& vertex : m_StaticMeshAsset->Vertices)
	{
		minUv.x = (std::min)(minUv.x, vertex.TexCoord.x);
		minUv.y = (std::min)(minUv.y, vertex.TexCoord.y);
		maxUv.x = (std::max)(maxUv.x, vertex.TexCoord.x);
		maxUv.y = (std::max)(maxUv.y, vertex.TexCoord.y);
		if (vertex.TexCoord.x < 0.0f || vertex.TexCoord.x > 1.0f || vertex.TexCoord.y < 0.0f || vertex.TexCoord.y > 1.0f)
		{
			++outOfRangeUvCount;
		}
	}

	std::string uvLogMessage = "UV diagnostics - Min(";
	uvLogMessage.append(std::to_string(minUv.x));
	uvLogMessage.append(", ");
	uvLogMessage.append(std::to_string(minUv.y));
	uvLogMessage.append(") Max(");
	uvLogMessage.append(std::to_string(maxUv.x));
	uvLogMessage.append(", ");
	uvLogMessage.append(std::to_string(maxUv.y));
	uvLogMessage.append(") OutOfRangeVertexCount=");
	uvLogMessage.append(std::to_string(outOfRangeUvCount));
	LogEngineTrace(uvLogMessage);

	const std::filesystem::path spiderTextureDirectory = spiderDirectory / "textures";
	std::filesystem::path preferredSpiderDiffuseTexturePath;
	std::filesystem::path preferredSpiderNormalTexturePath;
	if (std::filesystem::exists(spiderTextureDirectory))
	{
		for (const auto& entry : std::filesystem::directory_iterator(spiderTextureDirectory))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}

			std::wstring fileName = entry.path().filename().wstring();
			std::transform(fileName.begin(), fileName.end(), fileName.begin(), [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });

			if (fileName.find(L"color") != std::wstring::npos)
			{
				const bool isPng = entry.path().extension() == L".png" || entry.path().extension() == L".PNG";
				if (preferredSpiderDiffuseTexturePath.empty() || isPng)
				{
					preferredSpiderDiffuseTexturePath = entry.path();
				}
			}

			if (preferredSpiderNormalTexturePath.empty() && (fileName.find(L"nrm") != std::wstring::npos || fileName.find(L"normal") != std::wstring::npos))
			{
				preferredSpiderNormalTexturePath = entry.path();
			}
		}
	}

	if (!preferredSpiderDiffuseTexturePath.empty())
	{
		for (auto& material : m_StaticMeshAsset->Materials)
		{
			material.DiffuseTexturePath = preferredSpiderDiffuseTexturePath;
			material.EmbeddedDiffuseTexturePixels.clear();
			material.EmbeddedDiffuseTextureWidth = 0;
			material.EmbeddedDiffuseTextureHeight = 0;
			if (!preferredSpiderNormalTexturePath.empty())
			{
				material.NormalTexturePath = preferredSpiderNormalTexturePath;
			}
		}

		std::string spiderTextureOverrideLogMessage = "Spider texture override - Diffuse=";
		spiderTextureOverrideLogMessage.append(preferredSpiderDiffuseTexturePath.string());
		if (!preferredSpiderNormalTexturePath.empty())
		{
			spiderTextureOverrideLogMessage.append(" Normal=");
			spiderTextureOverrideLogMessage.append(preferredSpiderNormalTexturePath.string());
		}
		LogEngineTrace(spiderTextureOverrideLogMessage);
	}

	if (!LoadMaterialTextures())
	{
		return false;
	}

	DirectX::XMFLOAT3 minBounds(
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)());
	DirectX::XMFLOAT3 maxBounds(
		(std::numeric_limits<float>::lowest)(),
		(std::numeric_limits<float>::lowest)(),
		(std::numeric_limits<float>::lowest)());

	for (const auto& vertex : m_StaticMeshAsset->Vertices)
	{
		minBounds.x = (std::min)(minBounds.x, vertex.Position.x);
		minBounds.y = (std::min)(minBounds.y, vertex.Position.y);
		minBounds.z = (std::min)(minBounds.z, vertex.Position.z);
		maxBounds.x = (std::max)(maxBounds.x, vertex.Position.x);
		maxBounds.y = (std::max)(maxBounds.y, vertex.Position.y);
		maxBounds.z = (std::max)(maxBounds.z, vertex.Position.z);
	}

	const DirectX::XMFLOAT3 center = {
		(minBounds.x + maxBounds.x) * 0.5f,
		(minBounds.y + maxBounds.y) * 0.5f,
		(minBounds.z + maxBounds.z) * 0.5f
	};
	const float extentX = maxBounds.x - minBounds.x;
	const float extentY = maxBounds.y - minBounds.y;
	const float extentZ = maxBounds.z - minBounds.z;
	const float maxExtent = (std::max)(extentX, (std::max)(extentY, extentZ));
	const float cameraDistance = (std::max)(maxExtent * 2.5f, 3.0f);
	m_Camera.LookAt(
		{ center.x, center.y + maxExtent * 0.35f, center.z - cameraDistance },
		center,
		{ 0.0f, 1.0f, 0.0f });

	std::string logMessage = "Loaded Spider static mesh: ";
	logMessage.append(selectedModelPath.string());
	LogEngineTrace(logMessage);
	return true;
}

bool Engine::LoadMaterialTextures()
{
	m_MaterialTextures.clear();

	if (!m_StaticMeshAsset)
	{
		return true;
	}

	const size_t textureCount = (std::max)(static_cast<size_t>(1), m_StaticMeshAsset->Materials.size());
	m_MaterialTextures.resize(textureCount);

	for (size_t materialIndex = 0; materialIndex < textureCount; ++materialIndex)
	{
		auto& materialTexture = m_MaterialTextures[materialIndex];
		materialTexture.Path.clear();
		materialTexture.Pixels = { 255, 255, 255, 255 };
		materialTexture.Width = 1;
		materialTexture.Height = 1;

		if (materialIndex >= m_StaticMeshAsset->Materials.size())
		{
			continue;
		}

		const auto& material = m_StaticMeshAsset->Materials[materialIndex];
		if (!material.DiffuseTexturePath.empty() && std::filesystem::exists(material.DiffuseTexturePath))
		{
			int width = 0;
			int height = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load(material.DiffuseTexturePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
			{
				std::string failureLogMessage = "Material texture load failed - MaterialIndex=";
				failureLogMessage.append(std::to_string(materialIndex));
				failureLogMessage.append(" Path=");
				failureLogMessage.append(material.DiffuseTexturePath.string());
				failureLogMessage.append(" SelectedPath=<fallback white>");
				LogEngineTrace(failureLogMessage);
				continue;
			}

			materialTexture.Path = material.DiffuseTexturePath;
			materialTexture.Width = width;
			materialTexture.Height = height;
			materialTexture.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			stbi_image_free(pixels);

			std::string loadedTextureLogMessage = "Material texture loaded - MaterialIndex=";
			loadedTextureLogMessage.append(std::to_string(materialIndex));
			loadedTextureLogMessage.append(" Path=");
			loadedTextureLogMessage.append(materialTexture.Path.string());
			loadedTextureLogMessage.append(" | SourceChannels=");
			loadedTextureLogMessage.append(std::to_string(channels));
			loadedTextureLogMessage.append(" | UploadedAs=sRGB RGBA8");
			LogEngineTrace(loadedTextureLogMessage);
			continue;
		}

		if (!material.EmbeddedDiffuseTexturePixels.empty() && material.EmbeddedDiffuseTextureWidth > 0 && material.EmbeddedDiffuseTextureHeight > 0)
		{
			materialTexture.Width = material.EmbeddedDiffuseTextureWidth;
			materialTexture.Height = material.EmbeddedDiffuseTextureHeight;
			materialTexture.Pixels = material.EmbeddedDiffuseTexturePixels;

			std::string embeddedTextureLogMessage = "Material embedded texture loaded - MaterialIndex=";
			embeddedTextureLogMessage.append(std::to_string(materialIndex));
			embeddedTextureLogMessage.append(" Path=<embedded>");
			embeddedTextureLogMessage.append(" | UploadedAs=sRGB RGBA8");
			LogEngineTrace(embeddedTextureLogMessage);
			continue;
		}

		if (material.DiffuseTexturePath.empty() || !std::filesystem::exists(material.DiffuseTexturePath))
		{
			std::string fallbackLogMessage = "Material texture fallback - MaterialIndex=";
			fallbackLogMessage.append(std::to_string(materialIndex));
			fallbackLogMessage.append(" SelectedPath=<fallback white>");
			LogEngineTrace(fallbackLogMessage);
			continue;
		}
	}

	std::string materialTextureSummaryLogMessage = "Material texture count=";
	materialTextureSummaryLogMessage.append(std::to_string(m_MaterialTextures.size()));
	LogEngineTrace(materialTextureSummaryLogMessage);
	return true;
}

bool Engine::CreateTextureResources()
{
	const size_t textureCount = (std::max)(static_cast<size_t>(1), m_MaterialTextures.size());

	if (m_CurrentApi == GraphicsAPI::DirectX12)
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Device);
		if (!dx12Device)
		{
			return false;
		}

		m_Dx12Triangle.MaterialTextures.clear();
		m_Dx12Triangle.MaterialTextures.resize(textureCount);
		m_Dx12Triangle.ShaderResourceHeap.Reset();

		ComPtr<ID3D12CommandAllocator> commandAllocator;
		ComPtr<ID3D12GraphicsCommandList> commandList;
		if (FAILED(dx12Device->GetD3DDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator))) ||
			FAILED(dx12Device->GetD3DDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
		{
			return false;
		}

		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			const auto& materialTexture = m_MaterialTextures[textureIndex];
			auto& dx12MaterialTexture = m_Dx12Triangle.MaterialTextures[textureIndex];
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
		if (FAILED(dx12Device->GetD3DDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_Dx12Triangle.ShaderResourceHeap))))
		{
			return false;
		}

		const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto cpuHandle = m_Dx12Triangle.ShaderResourceHeap->GetCPUDescriptorHandleForHeapStart();
		for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			dx12Device->GetD3DDevice()->CreateShaderResourceView(
				m_Dx12Triangle.MaterialTextures[textureIndex].Texture.Get(),
				&srvDesc,
				cpuHandle);
			cpuHandle.ptr += descriptorSize;
		}

		return true;
	}

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
	if (!vulkanDevice)
	{
		return false;
	}

	// Vulkan 경로는 material 수만큼 sampled image를 만들고, 각 material texture를 staging buffer를 통해
	// 개별 VkImage로 업로드합니다. 이후 descriptor set은 CreateVulkanTriangleResources()에서 material별로 생성합니다.
	m_VulkanTriangle.MaterialTextures.clear();
	m_VulkanTriangle.MaterialTextures.resize(textureCount);

	for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
	{
		const auto& materialTexture = m_MaterialTextures[textureIndex];
		auto& vulkanMaterialTexture = m_VulkanTriangle.MaterialTextures[textureIndex];

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

	return true;
}

void Engine::DestroyTextureResources()
{
	m_Dx12Triangle.ShaderResourceHeap.Reset();
	m_Dx12Triangle.MaterialTextures.clear();

	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
	if (!vulkanDevice)
	{
		m_VulkanTriangle.MaterialTextures.clear();
		return;
	}

	// Vulkan material texture는 material 수만큼 생성되므로 sampler/image view/image/memory를 모두 순회하며 해제합니다.
	// 이 정리는 파이프라인 정리와 분리되어 있어, 리사이즈 시 파이프라인만 재생성하고 텍스처는 재사용할 수 있습니다.
	for (auto& materialTexture : m_VulkanTriangle.MaterialTextures)
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

	m_VulkanTriangle.MaterialTextures.clear();
}

bool Engine::CreateDx12TriangleResources()
{
	auto dx12Device = dynamic_cast<DX12Device*>(m_Device);
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
		0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_Dx12Triangle.RootSignature))))
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
	psoDesc.pRootSignature = m_Dx12Triangle.RootSignature.Get();
	psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	return SUCCEEDED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_Dx12Triangle.PipelineState)));
}

void Engine::DestroyDx12TriangleResources()
{
	m_Dx12Triangle.PipelineState.Reset();
	m_Dx12Triangle.RootSignature.Reset();
}

void Engine::DrawDx12Triangle()
{
	auto native = static_cast<ID3D12GraphicsCommandList*>(m_CmdList->GetNativeResource());
	auto cameraResource = m_CameraBuffer ? static_cast<ID3D12Resource*>(m_CameraBuffer->GetNativeResource()) : nullptr;
	auto dx12Device = dynamic_cast<DX12Device*>(m_Device);
	if (!native || !cameraResource || !dx12Device || !m_Dx12Triangle.PipelineState || !m_Dx12Triangle.RootSignature)
	{
		return;
	}

	native->SetGraphicsRootSignature(m_Dx12Triangle.RootSignature.Get());
	native->SetGraphicsRootConstantBufferView(0, cameraResource->GetGPUVirtualAddress());
	if (!m_Dx12Triangle.ShaderResourceHeap || m_Dx12Triangle.MaterialTextures.empty())
	{
		return;
	}

	ID3D12DescriptorHeap* descriptorHeaps[] = { m_Dx12Triangle.ShaderResourceHeap.Get() };
	native->SetDescriptorHeaps(1, descriptorHeaps);
	native->SetPipelineState(m_Dx12Triangle.PipelineState.Get());
	native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	const UINT descriptorSize = dx12Device->GetD3DDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_GPU_DESCRIPTOR_HANDLE baseHandle = m_Dx12Triangle.ShaderResourceHeap->GetGPUDescriptorHandleForHeapStart();

	if (m_StaticMeshAsset->Submeshes.empty())
	{
		native->SetGraphicsRootDescriptorTable(1, baseHandle);
		m_CmdList->DrawIndexedInstanced(static_cast<uint32_t>(m_StaticMeshAsset ? m_StaticMeshAsset->Indices.size() : 0), 1, 0, 0, 0);
		return;
	}

	for (const auto& submesh : m_StaticMeshAsset->Submeshes)
	{
		const size_t materialIndex = submesh.MaterialIndex < m_Dx12Triangle.MaterialTextures.size() ? submesh.MaterialIndex : 0;
		D3D12_GPU_DESCRIPTOR_HANDLE materialHandle = baseHandle;
		materialHandle.ptr += static_cast<SIZE_T>(descriptorSize) * materialIndex;
		native->SetGraphicsRootDescriptorTable(1, materialHandle);
		m_CmdList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
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

	m_CameraBuffer = m_Device->CreateBuffer(bufferDesc);
	return m_CameraBuffer != nullptr;
}

void Engine::UpdateCameraBuffer()
{
	if (!m_CameraBuffer)
	{
		return;
	}

	CameraConstants cameraConstants = {};
	DirectX::XMStoreFloat4x4(&cameraConstants.ViewProjection, m_Camera.GetViewProjectionMatrix());
	const auto position = m_Camera.GetPosition();
	cameraConstants.CameraPosition = { position.x, position.y, position.z, 1.0f };
	cameraConstants.DebugOptions = { m_IsUvDebugViewEnabled ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };

	void* mappedData = nullptr;
	m_CameraBuffer->Map(&mappedData);
	std::memcpy(mappedData, &cameraConstants, sizeof(cameraConstants));
	m_CameraBuffer->Unmap();
}

void Engine::UpdateAnimatedMesh(float deltaTime)
{
	if (!m_StaticMeshAsset || !m_StaticMeshAsset->IsAnimated || m_StaticMeshAsset->Animations.empty() || m_StaticMeshAsset->Bones.empty())
	{
		return;
	}

	const Asset::AnimationClip& clip = m_StaticMeshAsset->Animations.front();
	if (clip.DurationTicks <= 0.0)
	{
		return;
	}

	m_AnimationTimeSeconds += deltaTime;
	const double ticksPerSecond = clip.TicksPerSecond > 0.0 ? clip.TicksPerSecond : 25.0;
	const double animationTimeTicks = std::fmod(static_cast<double>(m_AnimationTimeSeconds) * ticksPerSecond, clip.DurationTicks);

	std::vector<DirectX::XMMATRIX> localTransforms;
	localTransforms.reserve(m_StaticMeshAsset->Nodes.size());
	for (const auto& node : m_StaticMeshAsset->Nodes)
	{
		localTransforms.push_back(BuildAnimatedLocalTransform(node, clip, *m_StaticMeshAsset, animationTimeTicks));
	}

	std::vector<DirectX::XMMATRIX> globalTransforms(m_StaticMeshAsset->Nodes.size(), DirectX::XMMatrixIdentity());
	if (!m_StaticMeshAsset->Nodes.empty())
	{
		ComputeGlobalNodeTransforms(*m_StaticMeshAsset, localTransforms, 0, DirectX::XMMatrixIdentity(), globalTransforms);
	}

	std::vector<DirectX::XMMATRIX> boneMatrices(m_StaticMeshAsset->Bones.size(), DirectX::XMMatrixIdentity());
	const DirectX::XMMATRIX rootInverseTransform = LoadMatrix(m_StaticMeshAsset->RootInverseTransform);
	for (size_t boneIndex = 0; boneIndex < m_StaticMeshAsset->Bones.size(); ++boneIndex)
	{
		const auto& bone = m_StaticMeshAsset->Bones[boneIndex];
		boneMatrices[boneIndex] = LoadMatrix(bone.OffsetMatrix) * globalTransforms[bone.NodeIndex] * rootInverseTransform;
	}

	m_StaticMeshAsset->Vertices = m_StaticMeshAsset->BindPoseVertices;
	for (size_t vertexIndex = 0; vertexIndex < m_StaticMeshAsset->Vertices.size(); ++vertexIndex)
	{
		const auto& bindVertex = m_StaticMeshAsset->BindPoseVertices[vertexIndex];
		auto& animatedVertex = m_StaticMeshAsset->Vertices[vertexIndex];

		const DirectX::XMVECTOR bindPosition = DirectX::XMLoadFloat3(&bindVertex.Position);
		const DirectX::XMVECTOR bindNormal = DirectX::XMLoadFloat3(&bindVertex.Normal);
		DirectX::XMVECTOR skinnedPosition = DirectX::XMVectorZero();
		DirectX::XMVECTOR skinnedNormal = DirectX::XMVectorZero();
		float totalWeight = 0.0f;

		for (size_t influenceIndex = 0; influenceIndex < bindVertex.BoneWeights.size(); ++influenceIndex)
		{
			const float weight = bindVertex.BoneWeights[influenceIndex];
			if (weight <= 0.0f)
			{
				continue;
			}

			const uint32_t boneIndex = bindVertex.BoneIndices[influenceIndex];
			const DirectX::XMMATRIX boneMatrix = boneIndex < boneMatrices.size() ? boneMatrices[boneIndex] : DirectX::XMMatrixIdentity();
			skinnedPosition += DirectX::XMVectorScale(DirectX::XMVector3TransformCoord(bindPosition, boneMatrix), weight);
			skinnedNormal += DirectX::XMVectorScale(DirectX::XMVector3TransformNormal(bindNormal, boneMatrix), weight);
			totalWeight += weight;
		}

		if (totalWeight > 0.0f)
		{
			DirectX::XMStoreFloat3(&animatedVertex.Position, skinnedPosition);
			const DirectX::XMVECTOR normalizedNormal = DirectX::XMVector3Normalize(skinnedNormal);
			DirectX::XMStoreFloat3(&animatedVertex.Normal, normalizedNormal);
		}
	}

	if (!m_VertexBuffer)
	{
		return;
	}

	void* mappedData = nullptr;
	m_VertexBuffer->Map(&mappedData);
	std::memcpy(mappedData, m_StaticMeshAsset->Vertices.data(), m_StaticMeshAsset->Vertices.size() * sizeof(Asset::StaticMeshVertex));
	m_VertexBuffer->Unmap();
}

bool Engine::CreateVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
	auto vulkanCameraBuffer = dynamic_cast<VulkanBuffer*>(m_CameraBuffer);
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

	if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &vertexModuleCreateInfo, nullptr, &m_VulkanTriangle.VertexShader) != VK_SUCCESS)
	{
		return false;
	}

	const VkShaderModuleCreateInfo fragmentModuleCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fragmentShaderCode.size() * sizeof(uint32_t),
		.pCode = fragmentShaderCode.data()
	};

	if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &fragmentModuleCreateInfo, nullptr, &m_VulkanTriangle.PixelShader) != VK_SUCCESS)
	{
		return false;
	}

	// Vulkan 경로는 카메라 상수 버퍼를 uniform buffer + descriptor set으로 바인딩합니다.
	const VkDescriptorSetLayoutBinding cameraBinding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
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

	if (vkCreateDescriptorSetLayout(vulkanDevice->GetVkDevice(), &descriptorSetLayoutCreateInfo, nullptr, &m_VulkanTriangle.DescriptorSetLayout) != VK_SUCCESS)
	{
		return false;
	}

	const uint32_t materialTextureCount = static_cast<uint32_t>((std::max)(static_cast<size_t>(1), m_VulkanTriangle.MaterialTextures.size()));

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

	if (vkCreateDescriptorPool(vulkanDevice->GetVkDevice(), &descriptorPoolCreateInfo, nullptr, &m_VulkanTriangle.DescriptorPool) != VK_SUCCESS)
	{
		return false;
	}

	std::vector<VkDescriptorSetLayout> descriptorSetLayouts(materialTextureCount, m_VulkanTriangle.DescriptorSetLayout);
	const VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_VulkanTriangle.DescriptorPool,
		.descriptorSetCount = materialTextureCount,
		.pSetLayouts = descriptorSetLayouts.data()
	};
	m_VulkanTriangle.DescriptorSets.resize(materialTextureCount);

	if (vkAllocateDescriptorSets(vulkanDevice->GetVkDevice(), &descriptorSetAllocateInfo, m_VulkanTriangle.DescriptorSets.data()) != VK_SUCCESS)
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
		const auto& materialTexture = m_VulkanTriangle.MaterialTextures[materialIndex];
		const VkDescriptorImageInfo textureImageInfo = {
			.sampler = materialTexture.Sampler,
			.imageView = materialTexture.ImageView,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		};

		const VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_VulkanTriangle.DescriptorSets[materialIndex],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &cameraBufferInfo
		};
		const VkWriteDescriptorSet textureWriteDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = m_VulkanTriangle.DescriptorSets[materialIndex],
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
			.module = m_VulkanTriangle.VertexShader,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = m_VulkanTriangle.PixelShader,
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

	const VkPipelineColorBlendStateCreateInfo colorBlending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &colorBlendAttachment
	};

	const VkPipelineDepthStencilStateCreateInfo depthStencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		// Vulkan 경로도 DX12와 동일하게 depth test/write를 켜서
		// 카드형 메시나 겹치는 삼각형이 draw 순서가 아니라 깊이값 기준으로 올바르게 가려지게 합니다.
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS
	};

	const VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_VulkanTriangle.DescriptorSetLayout
	};

	if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &pipelineLayoutCreateInfo, nullptr, &m_VulkanTriangle.PipelineLayout) != VK_SUCCESS)
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
		.layout = m_VulkanTriangle.PipelineLayout,
		.renderPass = vulkanDevice->GetVkRenderPass(),
		.subpass = 0
	};

	if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_VulkanTriangle.Pipeline) != VK_SUCCESS)
	{
		return false;
	}

	m_VulkanTriangle.IsValid = true;
	return true;
}

void Engine::DestroyVulkanTriangleResources()
{
	auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
	if (!vulkanDevice)
	{
		auto savedMaterialTextures = std::move(m_VulkanTriangle.MaterialTextures);

		m_VulkanTriangle = {};

		m_VulkanTriangle.MaterialTextures = std::move(savedMaterialTextures);
		return;
	}

	if (m_VulkanTriangle.Pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(vulkanDevice->GetVkDevice(), m_VulkanTriangle.Pipeline, nullptr);
	}

	if (m_VulkanTriangle.PipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(vulkanDevice->GetVkDevice(), m_VulkanTriangle.PipelineLayout, nullptr);
	}

	if (m_VulkanTriangle.DescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(vulkanDevice->GetVkDevice(), m_VulkanTriangle.DescriptorPool, nullptr);
	}

	if (m_VulkanTriangle.DescriptorSetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(vulkanDevice->GetVkDevice(), m_VulkanTriangle.DescriptorSetLayout, nullptr);
	}

	if (m_VulkanTriangle.VertexShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_VulkanTriangle.VertexShader, nullptr);
	}

	if (m_VulkanTriangle.PixelShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_VulkanTriangle.PixelShader, nullptr);
	}

	// 파이프라인 관련 핸들만 초기화합니다.
	// Vulkan material texture는 DestroyTextureResources()가 담당하므로, 리사이즈 중에는 벡터를 보존해야
	// descriptor set과 pipeline만 재생성하면서 기존 텍스처를 다시 사용할 수 있습니다.
	auto savedMaterialTextures = std::move(m_VulkanTriangle.MaterialTextures);

	m_VulkanTriangle = {};

	m_VulkanTriangle.MaterialTextures = std::move(savedMaterialTextures);
}

void Engine::DrawVulkanTriangle()
{
	if (!m_VulkanTriangle.IsValid || !m_StaticMeshAsset)
	{
		return;
	}

	auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_CmdList->GetNativeResource());
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}

	if (m_VulkanTriangle.DescriptorSets.empty())
	{
		return;
	}

	// Vulkan은 현재 열린 render pass 안에서 그래픽 파이프라인을 바인딩하고 draw를 기록합니다.
	vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VulkanTriangle.Pipeline);

	if (m_StaticMeshAsset->Submeshes.empty())
	{
		// Vulkan fallback 경로에서는 첫 번째 material descriptor set을 사용해 전체 메시를 그립니다.
		const VkDescriptorSet descriptorSet = m_VulkanTriangle.DescriptorSets.front();
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			m_VulkanTriangle.PipelineLayout,
			0,
			1,
			&descriptorSet,
			0,
			nullptr);
		m_CmdList->DrawIndexedInstanced(static_cast<uint32_t>(m_StaticMeshAsset->Indices.size()), 1, 0, 0, 0);
		return;
	}

	for (const auto& submesh : m_StaticMeshAsset->Submeshes)
	{
		const size_t materialIndex = submesh.MaterialIndex < m_VulkanTriangle.DescriptorSets.size() ? submesh.MaterialIndex : 0;
		const VkDescriptorSet descriptorSet = m_VulkanTriangle.DescriptorSets[materialIndex];

		// Vulkan 경로는 submesh.MaterialIndex에 대응하는 descriptor set을 바인딩해 material별 texture를 선택합니다.
		// 그런 다음 해당 submesh의 index 범위만 DrawIndexedInstanced로 기록해 멀티 머티리얼 메시를 올바르게 렌더링합니다.
		vkCmdBindDescriptorSets(
			commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			m_VulkanTriangle.PipelineLayout,
			0,
			1,
			&descriptorSet,
			0,
			nullptr);
		m_CmdList->DrawIndexedInstanced(submesh.IndexCount, 1, submesh.IndexOffset, 0, 0);
	}
}

bool Engine::CreateTriangleVertexBuffer()
{
	if (!m_StaticMeshAsset || m_StaticMeshAsset->Vertices.empty())
	{
		return false;
	}

	const BufferDesc bufferDesc = {
		.Size = static_cast<uint64_t>(m_StaticMeshAsset->Vertices.size() * sizeof(Asset::StaticMeshVertex)),
		.Stride = sizeof(Asset::StaticMeshVertex),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_VertexBuffer = m_Device->CreateBuffer(bufferDesc);
	if (!m_VertexBuffer)
	{
		return false;
	}

	void* mappedData = nullptr;
	m_VertexBuffer->Map(&mappedData);
	std::memcpy(mappedData, m_StaticMeshAsset->Vertices.data(), static_cast<size_t>(bufferDesc.Size));
	m_VertexBuffer->Unmap();

	return true;
}

bool Engine::CreateIndexBuffer()
{
	if (!m_StaticMeshAsset || m_StaticMeshAsset->Indices.empty())
	{
		return false;
	}

	const BufferDesc bufferDesc = {
		.Size = static_cast<uint64_t>(m_StaticMeshAsset->Indices.size() * sizeof(uint32_t)),
		.Stride = sizeof(uint32_t),
		.Heap = HeapType::Upload,
		.InitialState = ResourceState::GenericRead
	};

	m_IndexBuffer = m_Device->CreateBuffer(bufferDesc);
	if (!m_IndexBuffer)
	{
		return false;
	}

	void* mappedData = nullptr;
	m_IndexBuffer->Map(&mappedData);
	std::memcpy(mappedData, m_StaticMeshAsset->Indices.data(), static_cast<size_t>(bufferDesc.Size));
	m_IndexBuffer->Unmap();
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
		MF_BYCOMMAND | (m_CurrentApi == GraphicsAPI::DirectX12 ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_RENDERER_VULKAN, 
		MF_BYCOMMAND | (m_CurrentApi == GraphicsAPI::Vulkan ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(menu, IDM_VIEW_UV_DEBUG,
		MF_BYCOMMAND | (m_IsUvDebugViewEnabled ? MF_CHECKED : MF_UNCHECKED));
	DrawMenuBar(m_hMainWnd);
}

void Engine::ResetFpsCounter()
{
	m_FrameCount = 0;
	m_LastFpsUpdate = std::chrono::steady_clock::now();
	std::wstring title = m_WindowTitleBase;
	if (m_IsUvDebugViewEnabled)
	{
		title.append(L" [UV Debug]");
	}
	SetWindowTextW(m_hMainWnd, title.c_str());
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
	swprintf_s(titleBuffer, L"%s%s | FPS: %.1f", m_WindowTitleBase.c_str(), m_IsUvDebugViewEnabled ? L" [UV Debug]" : L"", fps);
	SetWindowTextW(m_hMainWnd, titleBuffer);

	m_FrameCount = 0;
	m_LastFpsUpdate = now;
}
