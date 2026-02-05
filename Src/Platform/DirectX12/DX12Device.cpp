#include "DX12Device.h"

#include <stdexcept>
#include <string>
#include <iostream>
#include "d3dx12.h" // CD3D12... 유틸리티 클래스
#include "DX12CommandList.h"
#include "Utils/Utils.h"

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
	UINT dxgiFactoryFlags = 0;

	// 디버그 레이어 활성화
#if defined(_DEBUG)
	ComPtr<ID3D12Debug> debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
		dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

	// DXGI 팩토리 생성
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));

	// 하드웨어 어댑터 선택
	ComPtr<IDXGIAdapter1> hardwareAdapter;
	GetHardwareAdapter(m_dxgiFactory.Get(), &hardwareAdapter);

	if (!hardwareAdapter)
	{
		throw std::runtime_error("Failed to find a D3D12 compatible GPU adapter.");
	}

    ThrowIfFailed(D3D12CreateDevice(
        hardwareAdapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_d3dDevice)
	));

	// 커맨드 큐 디스크립터 생성
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	// 커맨드 큐 생성
	ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));


	// 스왑 체인 디스크립터 생성
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_Width;
	swapChainDesc.Height = m_Height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Win10/11 표준
	swapChainDesc.SampleDesc.Count = 1;

	// 윈도우와의 연관 설정
	ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(static_cast<HWND>(m_hWnd), DXGI_MWA_NO_ALT_ENTER));

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

	// RTV 및 DSV 힙 생성
	CreateRtvAndDsvHeaps();
	// 렌더 타겟 뷰 생성
	CreateRenderTargetViews();
	// 깊이-스텐실 뷰 생성
	CreateDepthStencilView();

	// 동기화 펜스 생성
	ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	m_fenceValues[m_frameIndex]++;
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (m_fenceEvent == nullptr)
	{
		ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	}

	// 초기 GPU 대기
	WaitForGPU();

	return true;
}

void DX12Device::WaitForGPU()
{
	// 현재 프레임의 펜스 값으로 시그널 및 대기
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]));
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	m_fenceValues[m_frameIndex]++;
}

void DX12Device::MoveToNextFrame()
{
	// 현재 프레임의 펜스 값을 저장
	const uint64_t currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

	// 다음 프레임 인덱스 계산
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// 다음 프레임의 펜스 값 업데이트
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
	// TODO : DX12Buffer 구현 필요
    return nullptr;
}

void DX12Device::ExecuteCommandList(ICommandList* cmdList)
{
	// 추상 인터페이스를 DX12 구현체로 캐스팅
	// CommandList가 하나뿐이라 배열로 만들지 않고 바로 주소로 넘김

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
		// IDXGIFactory6 사용 가능 시 고성능 GPU 우선 선택
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
