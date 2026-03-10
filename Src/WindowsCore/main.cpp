// main.cpp : 애플리케이션에 대한 진입점을 정의합니다.

#include "EngineCore/GameApp.h"
#include "Resource.h"

#include "RHI/IGraphicsDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"        // 나중에 쓸 테니 미리 넣어둬도 됨
#include "RHI/GraphicsCommon.h" // GraphicsAPI Enum 때문에 필요

#include "Platform/DirectX12/DX12Device.h"
#include "Platform/DirectX12/d3dx12.h"
#include "Platform/Vulkan/VulkanDevice.h"

#include <d3dcompiler.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <wrl.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	const char* GraphicsApiToString(GraphicsAPI api)
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

	void LogMainTrace(const char* message)
	{
		char buffer[512] = {};
		snprintf(buffer, sizeof(buffer), "[Main][TRACE] %s\n", message);
		OutputDebugStringA(buffer);
	}

	struct TriangleVertex
{
    float Position[2];
    float Color[4];
};

	constexpr TriangleVertex kDx12TriangleVertices[] =
{
    { { 0.0f, 0.5f },  { 1.0f, 0.0f, 0.0f, 1.0f } },
    { { 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
    { { -0.5f, -0.5f },{ 0.0f, 0.0f, 1.0f, 1.0f } }
};

	// Vulkan은 DX12와 같은 정점 데이터를 그대로 사용하면 최종 화면에서 역삼각형으로 보입니다.
	constexpr TriangleVertex kVulkanTriangleVertices[] =
	{
		{ { 0.0f, 0.5f },  { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.5f, -0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { -0.5f, -0.5f },{ 0.0f, 0.0f, 1.0f, 1.0f } }
	};

	constexpr const char* kDx12TriangleShaderSource = R"(
struct VSInput
{
    float2 Position : POSITION;
    float4 Color : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position, 0.0f, 1.0f);
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.Color;
}
 )";

	constexpr const char* kVulkanTriangleVertexShaderSource = R"(#version 450
layout(location = 0) out vec4 outColor;

void main()
{
	// 버텍스 버퍼 경로 진단을 위해 gl_VertexIndex만으로 삼각형 위치/색을 직접 생성합니다.
	const vec2 positions[3] = vec2[](
		vec2(0.0, 0.5),
		vec2(0.5, -0.5),
		vec2(-0.5, -0.5));

	const vec4 colors[3] = vec4[](
		vec4(1.0, 0.0, 0.0, 1.0),
		vec4(0.0, 1.0, 0.0, 1.0),
		vec4(0.0, 0.0, 1.0, 1.0));

	// Vulkan은 DX12와 화면 Y 방향 해석이 달라서 같은 위치 데이터를 주면 최종 출력이 역삼각형으로 보입니다.
	gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
	outColor = colors[gl_VertexIndex];
}
 )";

	constexpr const char* kVulkanTriangleFragmentShaderSource = R"(#version 450
layout(location = 0) in vec4 inColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = inColor;
}
 )";
}

// 나중에는 별도 파일(Engine.h/cpp)로 분리될 테스트용 클래스
class TestEngine : public GameApp
{
	IGraphicsDevice* m_Device = nullptr;
	ICommandList* m_CmdList = nullptr;
	IBuffer* m_VertexBuffer = nullptr;
	GraphicsAPI m_CurrentApi = GraphicsAPI::Vulkan;
	std::wstring m_WindowTitleBase = L"EnginePlatformer - Vulkan";
	uint32_t m_FrameCount = 0;
	std::chrono::steady_clock::time_point m_LastFpsUpdate = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point m_RenderStartTime = std::chrono::steady_clock::now();

	struct Dx12TriangleResources
	{
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12PipelineState> PipelineState;
	} m_Dx12Triangle;

	struct VulkanTriangleResources
	{
		VkShaderModule VertexShader = VK_NULL_HANDLE;
		VkShaderModule PixelShader = VK_NULL_HANDLE;
		VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
		VkPipeline Pipeline = VK_NULL_HANDLE;
		bool IsValid = false;
	} m_VulkanTriangle;

public:
    TestEngine(HINSTANCE hInstance) : GameApp(hInstance) {}
    ~TestEngine()
    {
        ShutdownGraphics();

		glslang_finalize_process();
    }

    bool Init() override
    {
        if (!GameApp::Init()) return false;

		if (!glslang_initialize_process())
			return false;

		if (!SwitchGraphicsAPI(m_CurrentApi))
			return false;

        return true;
    }

	LRESULT MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override
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
				{
					if (m_CurrentApi != GraphicsAPI::Vulkan)
					{
						const GraphicsAPI previousApi = m_CurrentApi;
						if (!SwitchGraphicsAPI(GraphicsAPI::Vulkan))
						{
							SwitchGraphicsAPI(previousApi);
						}
					}
					return 0;
				}
			}
		}

		return GameApp::MsgProc(hWnd, msg, wParam, lParam);
	}

    void OnResize() override
    {
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

    void Update(float dt) override
    {
        // TODO: 게임 로직
    }

    void Render() override
    {
		if (!m_Device || !m_CmdList)
		{
			return;
		}

		if (m_CurrentApi == GraphicsAPI::Vulkan)
		{
			LogMainTrace("Entered Vulkan Render().");
		}

		// TODO: Vulkan 그리기 명령
		// 커맨드 리스트 리셋
        m_CmdList->Reset();

		// 뷰포트 시저포트 설정
		m_CmdList->SetViewport(0, 0, static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
		m_CmdList->SetScissorRect(0, 0, m_ClientWidth, m_ClientHeight);

		// 백버퍼를 렌더타겟 상태로 전환
		IGpuResource* backBuffer = m_Device->GetBackBufferResource();
        m_CmdList->ResourceBarrier(
            backBuffer,
            ResourceState::Present,
			ResourceState::RenderTarget);

		// 렌더타겟 및 뎁스스텐실 뷰 설정
		void* rtvHandle = m_Device->GetCurrentBackBufferRTV();
		void* dsvHandle = m_Device->GetDepthStencilView();

		m_CmdList->SetRenderTargets(rtvHandle, dsvHandle);

		const auto now = std::chrono::steady_clock::now();
		const std::chrono::duration<float> elapsed = now - m_RenderStartTime;
		const float color = (sinf(elapsed.count() * 2.0f) + 1.0f) * 0.5f;

		// 화면 클리어
		float clearColor[4] = { color, 0.2f, 0.4f, 1.0f };
		m_CmdList->ClearRenderTarget(rtvHandle, clearColor);
		m_CmdList->ClearDepthStencil(dsvHandle, 1.0, 0);
		m_CmdList->SetVertexBuffer(m_VertexBuffer);

		if (m_CurrentApi == GraphicsAPI::DirectX12)
		{
			DrawDx12Triangle();
		}
		else
		{
			DrawVulkanTriangle();
		}

        m_CmdList->ResourceBarrier(
            backBuffer,
            ResourceState::RenderTarget,
			ResourceState::Present);

		m_CmdList->Close();

		// 커맨드 리스트 실행
		m_Device->ExecuteCommandList(m_CmdList);

		// 화면 출력
		m_Device->Present();

		m_Device->MoveToNextFrame();
		UpdateWindowTitleWithFps();
    }

private:
	void UpdateRendererMenuState()
	{
		HMENU menu = GetMenu(m_hMainWnd);
		if (!menu)
		{
			return;
		}

		CheckMenuItem(menu, IDM_RENDERER_DX12, MF_BYCOMMAND | (m_CurrentApi == GraphicsAPI::DirectX12 ? MF_CHECKED : MF_UNCHECKED));
		CheckMenuItem(menu, IDM_RENDERER_VULKAN, MF_BYCOMMAND | (m_CurrentApi == GraphicsAPI::Vulkan ? MF_CHECKED : MF_UNCHECKED));
		DrawMenuBar(m_hMainWnd);
	}

	void ResetFpsCounter()
	{
		m_FrameCount = 0;
		m_LastFpsUpdate = std::chrono::steady_clock::now();
		SetWindowTextW(m_hMainWnd, m_WindowTitleBase.c_str());
	}

	void UpdateWindowTitleWithFps()
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

	bool SwitchGraphicsAPI(GraphicsAPI api)
	{
		char switchMessage[256] = {};
		snprintf(switchMessage, sizeof(switchMessage), "SwitchGraphicsAPI: %s -> %s", GraphicsApiToString(m_CurrentApi), GraphicsApiToString(api));
		LogMainTrace(switchMessage);

		ShutdownGraphics();

		m_CurrentApi = api;
		m_Device = IGraphicsDevice::Create(api, m_hMainWnd, m_ClientWidth, m_ClientHeight);
		if (!m_Device || !m_Device->Init())
		{
			LogMainTrace("SwitchGraphicsAPI failed during device initialization.");
			ShutdownGraphics();
			MessageBoxW(m_hMainWnd, L"그래픽 디바이스 초기화에 실패했습니다.", L"Graphics API Error", MB_OK | MB_ICONERROR);
			return false;
		}

		m_CmdList = m_Device->CreateCommandList();
		if (!CreateTriangleVertexBuffer())
		{
			LogMainTrace("SwitchGraphicsAPI failed during vertex buffer creation.");
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
			LogMainTrace("SwitchGraphicsAPI failed during pipeline resource creation.");
			ShutdownGraphics();
			return false;
		}

		m_WindowTitleBase = api == GraphicsAPI::DirectX12 ? L"EnginePlatformer - DirectX12" : L"EnginePlatformer - Vulkan";
		m_RenderStartTime = std::chrono::steady_clock::now();
		ResetFpsCounter();
		UpdateRendererMenuState();
		LogMainTrace("SwitchGraphicsAPI completed successfully.");
		return triangleInitResult;
	}

	void ShutdownGraphics()
	{
		if (m_Device)
		{
			LogMainTrace("ShutdownGraphics waiting for GPU.");
			m_Device->WaitForGPU();
		}

		DestroyDx12TriangleResources();
		DestroyVulkanTriangleResources();

		delete m_VertexBuffer;
		m_VertexBuffer = nullptr;

		delete m_CmdList;
		m_CmdList = nullptr;

		delete m_Device;
		m_Device = nullptr;

		LogMainTrace("ShutdownGraphics completed.");
	}

	bool CreateDx12TriangleResources()
	{
		auto dx12Device = dynamic_cast<DX12Device*>(m_Device);
		if (!dx12Device)
		{
			return false;
		}

		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;
		ComPtr<ID3DBlob> errors;

		if (FAILED(D3DCompile(kDx12TriangleShaderSource, strlen(kDx12TriangleShaderSource), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &errors)))
		{
			return false;
		}

		if (FAILED(D3DCompile(kDx12TriangleShaderSource, strlen(kDx12TriangleShaderSource), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pixelShader, &errors)))
		{
			return false;
		}

		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)))
		{
			return false;
		}

		if (FAILED(dx12Device->GetD3DDevice()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_Dx12Triangle.RootSignature))))
		{
			return false;
		}

		static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleDesc.Count = 1;

		return SUCCEEDED(dx12Device->GetD3DDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_Dx12Triangle.PipelineState)));
	}

	bool CreateTriangleVertexBuffer()
	{
		const TriangleVertex* triangleVertices = m_CurrentApi == GraphicsAPI::DirectX12 ? kDx12TriangleVertices : kVulkanTriangleVertices;
		const size_t triangleVertexBufferSize = m_CurrentApi == GraphicsAPI::DirectX12 ? sizeof(kDx12TriangleVertices) : sizeof(kVulkanTriangleVertices);

		BufferDesc bufferDesc = {};
		bufferDesc.Size = triangleVertexBufferSize;
		bufferDesc.Stride = sizeof(TriangleVertex);
		bufferDesc.Heap = HeapType::Upload;
		bufferDesc.InitialState = ResourceState::GenericRead;

		m_VertexBuffer = m_Device->CreateBuffer(bufferDesc);
		if (!m_VertexBuffer)
		{
			return false;
		}

		void* mappedData = nullptr;
		m_VertexBuffer->Map(&mappedData);
		memcpy(mappedData, triangleVertices, triangleVertexBufferSize);
		m_VertexBuffer->Unmap();
		return true;
	}

	void DestroyDx12TriangleResources()
	{
		m_Dx12Triangle.PipelineState.Reset();
		m_Dx12Triangle.RootSignature.Reset();
	}

	void DrawDx12Triangle()
	{
		auto native = static_cast<ID3D12GraphicsCommandList*>(m_CmdList->GetNativeResource());
		if (!native || !m_Dx12Triangle.PipelineState || !m_Dx12Triangle.RootSignature)
		{
			return;
		}

		native->SetGraphicsRootSignature(m_Dx12Triangle.RootSignature.Get());
		native->SetPipelineState(m_Dx12Triangle.PipelineState.Get());
		native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_CmdList->DrawInstanced(3, 1, 0, 0);
	}

	std::vector<uint32_t> CompileVulkanShader(glslang_source_t language, glslang_stage_t stage, const char* source, const char* entryPoint)
	{
		// Vulkan은 최종적으로 SPIR-V를 사용하므로, glslang을 통해 셰이더 문자열을 런타임에 SPIR-V로 변환합니다.
		glslang_input_t input = {};
		input.language = language;
		input.stage = stage;
		input.client = GLSLANG_CLIENT_VULKAN;
		input.client_version = GLSLANG_TARGET_VULKAN_1_2;
		input.target_language = GLSLANG_TARGET_SPV;
		input.target_language_version = GLSLANG_TARGET_SPV_1_5;
		input.code = source;
		input.default_version = 100;
		input.default_profile = GLSLANG_NO_PROFILE;
		input.force_default_version_and_profile = false;
		input.forward_compatible = false;
		input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
		if (language == GLSLANG_SOURCE_HLSL)
		{
			input.messages = static_cast<glslang_messages_t>(input.messages | GLSLANG_MSG_READ_HLSL_BIT);
		}
		input.resource = glslang_default_resource();

		glslang_shader_t* shader = glslang_shader_create(&input);
		glslang_shader_set_entry_point(shader, entryPoint);

		if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
		{
			MessageBoxA(m_hMainWnd, glslang_shader_get_info_log(shader), "Vulkan Shader Compile Error", MB_OK | MB_ICONERROR);
			glslang_shader_delete(shader);
			return {};
		}

		glslang_program_t* program = glslang_program_create();
		glslang_program_add_shader(program, shader);
		if (!glslang_program_link(program, input.messages))
		{
			MessageBoxA(m_hMainWnd, glslang_program_get_info_log(program), "Vulkan Shader Link Error", MB_OK | MB_ICONERROR);
			glslang_program_delete(program);
			glslang_shader_delete(shader);
			return {};
		}

		glslang_program_SPIRV_generate(program, stage);
		const size_t wordCount = glslang_program_SPIRV_get_size(program);
		std::vector<uint32_t> spirv(wordCount);
		glslang_program_SPIRV_get(program, spirv.data());

		const char* spvMessages = glslang_program_SPIRV_get_messages(program);
		if (spvMessages && spvMessages[0] != '\0')
		{
			OutputDebugStringA("[Vulkan SPIR-V] ");
			OutputDebugStringA(spvMessages);
			OutputDebugStringA("\n");
		}

		glslang_program_delete(program);
		glslang_shader_delete(shader);
		return spirv;
	}

	bool CreateVulkanTriangleResources()
	{
		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
		if (!vulkanDevice)
		{
			return false;
		}

		const std::vector<uint32_t> vertexShaderCode = CompileVulkanShader(GLSLANG_SOURCE_GLSL, GLSLANG_STAGE_VERTEX, kVulkanTriangleVertexShaderSource, "main");
		const std::vector<uint32_t> pixelShaderCode = CompileVulkanShader(GLSLANG_SOURCE_GLSL, GLSLANG_STAGE_FRAGMENT, kVulkanTriangleFragmentShaderSource, "main");
		if (vertexShaderCode.empty() || pixelShaderCode.empty())
		{
			return false;
		}

		VkShaderModuleCreateInfo vertexModuleCreateInfo = {};
		vertexModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		vertexModuleCreateInfo.codeSize = vertexShaderCode.size() * sizeof(uint32_t);
		vertexModuleCreateInfo.pCode = vertexShaderCode.data();

		if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &vertexModuleCreateInfo, nullptr, &m_VulkanTriangle.VertexShader) != VK_SUCCESS)
		{
			return false;
		}

		VkShaderModuleCreateInfo pixelModuleCreateInfo = {};
		pixelModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		pixelModuleCreateInfo.codeSize = pixelShaderCode.size() * sizeof(uint32_t);
		pixelModuleCreateInfo.pCode = pixelShaderCode.data();

		if (vkCreateShaderModule(vulkanDevice->GetVkDevice(), &pixelModuleCreateInfo, nullptr, &m_VulkanTriangle.PixelShader) != VK_SUCCESS)
		{
			return false;
		}

		VkPipelineShaderStageCreateInfo shaderStages[2] = {};
		shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		shaderStages[0].module = m_VulkanTriangle.VertexShader;
		shaderStages[0].pName = "main";
		shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderStages[1].module = m_VulkanTriangle.PixelShader;
		shaderStages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput = {};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = 0;
		vertexInput.pVertexBindingDescriptions = nullptr;
		vertexInput.vertexAttributeDescriptionCount = 0;
		vertexInput.pVertexAttributeDescriptions = nullptr;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState = {};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = {};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
		rasterizer.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisampling = {};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
		colorBlendAttachment.colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo colorBlending = {};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;

		VkPipelineDepthStencilStateCreateInfo depthStencil = {};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = VK_FALSE;
		depthStencil.depthWriteEnable = VK_FALSE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {};
		pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

		if (vkCreatePipelineLayout(vulkanDevice->GetVkDevice(), &pipelineLayoutCreateInfo, nullptr, &m_VulkanTriangle.PipelineLayout) != VK_SUCCESS)
		{
			return false;
		}

		VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
		pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCreateInfo.stageCount = 2;
		pipelineCreateInfo.pStages = shaderStages;
		pipelineCreateInfo.pVertexInputState = &vertexInput;
		pipelineCreateInfo.pInputAssemblyState = &inputAssembly;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pRasterizationState = &rasterizer;
		pipelineCreateInfo.pMultisampleState = &multisampling;
		pipelineCreateInfo.pColorBlendState = &colorBlending;
		pipelineCreateInfo.pDepthStencilState = &depthStencil;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.layout = m_VulkanTriangle.PipelineLayout;
		pipelineCreateInfo.renderPass = vulkanDevice->GetVkRenderPass();
		pipelineCreateInfo.subpass = 0;

		if (vkCreateGraphicsPipelines(vulkanDevice->GetVkDevice(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &m_VulkanTriangle.Pipeline) != VK_SUCCESS)
		{
			return false;
		}

		m_VulkanTriangle.IsValid = true;
		return true;
	}

	void DestroyVulkanTriangleResources()
	{
		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
		if (!vulkanDevice)
		{
			m_VulkanTriangle = {};
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

		if (m_VulkanTriangle.VertexShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_VulkanTriangle.VertexShader, nullptr);
		}

		if (m_VulkanTriangle.PixelShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(vulkanDevice->GetVkDevice(), m_VulkanTriangle.PixelShader, nullptr);
		}

		m_VulkanTriangle = {};
	}

	void DrawVulkanTriangle()
	{
		if (!m_VulkanTriangle.IsValid)
		{
			LogMainTrace("DrawVulkanTriangle skipped because pipeline is invalid.");
			return;
		}

		auto vulkanDevice = dynamic_cast<VulkanDevice*>(m_Device);
		auto commandBuffer = reinterpret_cast<VkCommandBuffer>(m_CmdList->GetNativeResource());
		if (!vulkanDevice || commandBuffer == VK_NULL_HANDLE)
		{
			LogMainTrace("DrawVulkanTriangle skipped because Vulkan device or command buffer is invalid.");
			return;
		}

		// Vulkan은 현재 열린 render pass 안에서 그래픽 파이프라인을 바인딩하고 draw를 기록합니다.
		LogMainTrace("Recording Vulkan triangle draw.");
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_VulkanTriangle.Pipeline);
		m_CmdList->DrawInstanced(3, 1, 0, 0);
	}
};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

    // 디버그 모드에서 메모리 누수 감지 켜기
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    TestEngine theApp(hInstance);

    if (!theApp.Init())
        return 0;

    return theApp.Run();
}



