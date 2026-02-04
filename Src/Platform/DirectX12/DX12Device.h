#pragma once
#include "RHI/IGraphicsDevice.h"

// DirectX 12 헤더
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

// 라이브러리 링크
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "DX12Resource.h"
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

class DX12Device : public IGraphicsDevice
{
public:
	DX12Device(void* windowHandle, int width, int height);
	virtual ~DX12Device();
	
	// IGraphicsDevice을(를) 통해 상속됨
	bool Init() override;
	void Shutdown() override;

	void WaitForGPU() override;
	void MoveToNextFrame() override;
	void Present() override;

	// 생성
	ICommandList* CreateCommandList() override;
	IBuffer* CreateBuffer(const BufferDesc& desc) override;

	// 뷰 접근
	void* GetCurrentBackBufferRTV() override;
	void* GetDepthStencilView() override;

public:
	
	ID3D12Device* GetD3DDevice() const { return m_d3dDevice.Get(); }
	ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }

private:
	void GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter);
	DX12Resource* GetBackBufferResource() const { return m_renderTargets[m_frameIndex].get(); }

	void CreateRtvAndDsvHeaps();
	void CreateRenderTargetViews();
	void CreateDepthStencilView();

private:
	static const uint8_t FrameCount = 2; // 이중 버퍼링

	void* m_hWnd;
	int m_Width;
	int m_Height;

	// DX12 Core
	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<ID3D12Device> m_d3dDevice;
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<IDXGISwapChain3> m_swapChain;

	// Descriptors
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	uint32_t m_rtvDescriptorSize = 0;

	// Render Targets
	std::unique_ptr<DX12Resource> m_renderTargets[FrameCount];
	ComPtr<ID3D12Resource> m_depthStencil;

	// Synchronization
	ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent = nullptr;
	uint64_t m_fenceValues[FrameCount] = {};
	uint64_t m_frameIndex = 0;
};