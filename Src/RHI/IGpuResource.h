#pragma once

// IGpuResource: 그래픽 리소스의 공통 인터페이스
class IGpuResource
{
public:
	virtual ~IGpuResource() = default;

	// 네이티브 그래픽 리소스 핸들 반환 (예: ID3D12Resource*)
	virtual void* GetNativeResource() const = 0;
};