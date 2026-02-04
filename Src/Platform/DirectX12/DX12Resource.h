#pragma once
#include "RHI/IGpuResource.h"
#include <d3d12.h>
#include <wrl.h>

class DX12Resource : public IGpuResource
{
public:
	DX12Resource(Microsoft::WRL::ComPtr<ID3D12Resource> resource)
		: m_d3dResource(resource)
	{
	}
	virtual ~DX12Resource() = default;

	// IGpuResource을(를) 통해 상속됨
	void* GetNativeResource() const override
	{
		return m_d3dResource.Get();
	}

	// D3D12 내부에서 편하게 쓰기 위한 헬퍼
	ID3D12Resource* GetD3D12Resource() const
	{
		return m_d3dResource.Get();
	}

protected:
	Microsoft::WRL::ComPtr<ID3D12Resource> m_d3dResource;
};