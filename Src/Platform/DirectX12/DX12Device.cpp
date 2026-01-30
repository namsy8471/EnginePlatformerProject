#include "DX12Device.h"

bool DX12Device::Init()
{
    return false;
}

void DX12Device::Shutdown()
{
}

void DX12Device::WaitForGPU()
{
}

void DX12Device::MoveToNextFrame()
{
}

ICommandList* DX12Device::CreateCommandList()
{
    return nullptr;
}

IBuffer* DX12Device::CreateBuffer(const BufferDesc& desc)
{
    return nullptr;
}

void DX12Device::Present()
{
}

void* DX12Device::GetCurrentBackBufferRTV()
{
    return nullptr;
}

void* DX12Device::GetDepthStencilView()
{
    return nullptr;
}
