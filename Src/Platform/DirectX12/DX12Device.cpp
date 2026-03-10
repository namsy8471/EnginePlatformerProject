#include "DX12Device.h"

#include <stdexcept>
#include <string>
#include <iostream>
#include <cstdio>
#include "d3dx12.h" // CD3DX12 유틸리티 클래스
#include "DX12Buffer.h"
#include "DX12CommandList.h"
#include "Utils/Utils.h"

namespace
{
	void LogDX12Message(const char* level, const char* message)
	{
		char buffer[512] = {};
		snprintf(buffer, sizeof(buffer), "[DX12][%s] %s\n", level, message);
		OutputDebugStringA(buffer);
	}

	void LogDX12StageSuccess(const char* stage)
	{
		LogDX12Message("OK", stage);
	}

	void LogDX12Warning(const char* message)
	{
		LogDX12Message("WARN", message);
	}

	void LogDX12Error(const char* message)
	{
		LogDX12Message("ERROR", message);
	}
}

DX12Device::DX12Device(void* windowHandle, int width, int height)
	: m_hWnd(windowHandle), m_Width(width), m_Height(height), m_frameIndex(0), m_fenceEvent(nullptr), m_rtvDescriptorSize(0)
{
    // 펜스 값 초기화
    for (int i = 0; i < FrameCount; ++i) m_fenceValues[i] = 0;
}

DX12Device::~DX12Device()
{
    Shutdown();
}

void DX12Device::Shutdown()
{
    WaitForGPU();
    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
    }
	// ComPtr는 자동으로 해제됨
}

bool DX12Device::Init()
{
	try
	{
		UINT dxgiFactoryFlags = 0;

		// Debug build에서는 DX12 디버그 레이어를 명시적으로 켜서 유효성 오류를 바로 디버거에 전달합니다.
#if defined(_DEBUG) || defined(DEBUG)
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			LogDX12StageSuccess("Debug layer enabled.");

			ComPtr<ID3D12Debug1> debugController1;
			if (SUCCEEDED(debugController.As(&debugController1)))
			{
				debugController1->SetEnableGPUBasedValidation(TRUE);
				LogDX12StageSuccess("GPU-based validation enabled.");
			}
		}
		else
		{
			LogDX12Warning("D3D12 debug interface is not available.");
		}
#endif

		// DXGI 팩토리 생성
		ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));
		LogDX12StageSuccess("DXGI factory created.");

		// 하드웨어 어댑터 검색
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(m_dxgiFactory.Get(), &hardwareAdapter);

		if (!hardwareAdapter)
		{
			throw std::runtime_error("Failed to find a D3D12 compatible GPU adapter.");
		}
		LogDX12StageSuccess("Hardware adapter selected.");

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_d3dDevice)
		));
		LogDX12StageSuccess("D3D12 device created.");

#if defined(_DEBUG) || defined(DEBUG)
		// InfoQueue를 통해 런타임 오류/경고를 디버거에서 즉시 확인합니다.
		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(m_d3dDevice.As(&infoQueue)))
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
			LogDX12StageSuccess("Info queue breakpoints configured.");
		}
#endif

		// 커맨드 큐 디스크립터 생성
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

		// 커맨드 큐 생성
		ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
		LogDX12StageSuccess("Command queue created.");

		// 스왑 체인 디스크립터 생성
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.BufferCount = FrameCount;
		swapChainDesc.Width = m_Width;
		swapChainDesc.Height = m_Height;
		swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Win10/11 표준
		swapChainDesc.SampleDesc.Count = 1;

		// 스왑 체인 생성
		ComPtr<IDXGISwapChain1> swapChain1;
		ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(
			m_commandQueue.Get(),
			static_cast<HWND>(m_hWnd),
			&swapChainDesc,
			nullptr,
			nullptr,
			&swapChain1
		));

		// IDXGISwapChain3 인터페이스로 변환
		ThrowIfFailed(swapChain1.As(&m_swapChain));
		m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
		LogDX12StageSuccess("Swapchain created.");

		// 전체화면 전환 비활성화
		ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(static_cast<HWND>(m_hWnd), DXGI_MWA_NO_ALT_ENTER));
		LogDX12StageSuccess("Alt+Enter fullscreen toggle disabled.");

		// RTV 및 DSV 힙 생성
		CreateRtvAndDsvHeaps();
		LogDX12StageSuccess("RTV/DSV heaps created.");
		// 렌더 타겟 뷰 생성
		CreateRenderTargetViews();
		LogDX12StageSuccess("Render target views created.");
		// 깊이-스텐실 뷰 생성
		CreateDepthStencilView();
		LogDX12StageSuccess("Depth stencil view created.");

		// 동기화 펜스 생성
		ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValues[m_frameIndex]++;
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
		LogDX12StageSuccess("Fence and fence event created.");

		// 초기 GPU 동기화
		WaitForGPU();
		LogDX12StageSuccess("Initial GPU synchronization completed.");
		LogDX12StageSuccess("DX12 device initialization completed.");

		return true;
	}
	catch (const std::exception& ex)
	{
		LogDX12Error(ex.what());
		Shutdown();
		return false;
	}
	catch (...)
	{
		LogDX12Error("Unknown exception during DX12 initialization.");
		Shutdown();
		return false;
	}
}

void DX12Device::WaitForGPU()
{
	// 현재 프레임의 펜스 값 계산
	UINT64 fenceValueToSignal = m_fenceValues[m_frameIndex] + 1;

	// 2. GPU에 신호: "이 작업이 끝나면 이 펜스 값으로 완료 처리"
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceValueToSignal));

	// 3. CPU는 해당 펜스 값이 완료될 때까지 대기
	if (m_fence->GetCompletedValue() < fenceValueToSignal)
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValueToSignal, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// 4. 현재 프레임의 펜스 값 갱신
	m_fenceValues[m_frameIndex] = fenceValueToSignal;
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void DX12Device::MoveToNextFrame()
{
	// 현재 프레임의 펜스 값 저장
	const uint64_t currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

	// 다음 백버퍼 인덱스 갱신
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// 다음 프레임의 펜스 완료 대기
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	// 다음 프레임의 펜스 값 증가
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

ICommandList* DX12Device::CreateCommandList()
{
	// DX12CommandList 인스턴스 생성
	return new DX12CommandList(this);
}

IBuffer* DX12Device::CreateBuffer(const BufferDesc& desc)
{
	return new DX12Buffer(this, desc);
}

void DX12Device::ExecuteCommandList(ICommandList* cmdList)
{
	// 추상 인터페이스를 DX12 객체로 캐스팅
	// CommandList는 하나뿐이라 배열로 만들지 않고 바로 주소로 전달

	auto native = static_cast<ID3D12CommandList*>(cmdList->GetNativeResource());
	if(!native){
		throw std::runtime_error("Invalid command list native resource.");
	}
		
	ID3D12CommandList* ppCommandLists[] = { native };

	m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
}

IGpuResource* DX12Device::GetBackBufferResource()
{
	return m_renderTargets[m_frameIndex].get();
}

void DX12Device::Present()
{
	ThrowIfFailed(m_swapChain->Present(1, 0));
}

void DX12Device::Resize(int width, int height)
{
	// 현재 DX12 경로는 리사이즈를 아직 구현하지 않았으므로 크기만 저장해 둡니다.
	m_Width = width;
	m_Height = height;
}

void* DX12Device::GetCurrentBackBufferRTV()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtvHandle.Offset(static_cast<INT>(m_frameIndex), m_rtvDescriptorSize);
	return reinterpret_cast<void*>(rtvHandle.ptr);
}

void* DX12Device::GetDepthStencilView()
{
	return reinterpret_cast<void*>(m_dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr);
}

void DX12Device::GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter)
{
	*ppAdapter = nullptr;

	ComPtr<IDXGIFactory6> factory6;
	if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
	{
		// IDXGIFactory6가 있으면 고성능 GPU 우선 검색
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT adapterIndex = 0;
			SUCCEEDED(factory6->EnumAdapterByGpuPreference(
				adapterIndex,
				DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
				IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())));
			++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			if (SUCCEEDED(D3D12CreateDevice(
				adapter.Get(),
				D3D_FEATURE_LEVEL_11_0,
				_uuidof(ID3D12Device),
				nullptr)))
			{
				*ppAdapter = adapter.Detach();
				return;
			}
		}
	}
	else {
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != pFactory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
			{
				// 소프트웨어 어댑터는 건너뜀
				continue;
			}

			// D3D12 디바이스 생성 가능 여부 확인
			if (SUCCEEDED(D3D12CreateDevice(
				adapter.Get(),
				D3D_FEATURE_LEVEL_11_0,
				_uuidof(ID3D12Device),
				nullptr)))
			{
				*ppAdapter = adapter.Detach();
				return; // 적합한 어댑터 발견
			}
		}
	}
}

void DX12Device::CreateRtvAndDsvHeaps()
{
	// RTV 힙 생성
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = FrameCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
	m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// DSV 힙 생성
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
}

void DX12Device::CreateRenderTargetViews()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

	for (UINT i = 0; i < FrameCount; i++)
	{
		ComPtr<ID3D12Resource> backBuffer;
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
		
		m_renderTargets[i] = std::make_unique<DX12Resource>(backBuffer);

		m_d3dDevice->CreateRenderTargetView(
			m_renderTargets[i]->GetD3D12Resource(),
			nullptr, 
			rtvHandle
		);
		
		rtvHandle.Offset(1, m_rtvDescriptorSize);
	}
}

void DX12Device::CreateDepthStencilView()
{
	D3D12_RESOURCE_DESC depthStencilDesc = {};
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = m_Width;
	depthStencilDesc.Height = m_Height;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT; // 32비트 깊이
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = DXGI_FORMAT_D32_FLOAT;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	// 힙 속성: Default Heap (GPU 전용)
	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

	ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&optClear,
		IID_PPV_ARGS(&m_depthStencil)
	));

	m_d3dDevice->CreateDepthStencilView(m_depthStencil.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}
