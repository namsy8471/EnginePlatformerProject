#pragma once
#include "GraphicsCommon.h"
#include "IGpuResource.h"

class IBuffer : public IGpuResource
{
public:
	virtual ~IBuffer() = default;

	// 버퍼 크기(바이트 단위) 반환
	[[nodiscard]] virtual uint64_t GetSize() const noexcept = 0;
	[[nodiscard]] virtual uint32_t GetStride() const noexcept = 0;

	// CPU에서 업로드 메모리에 접근하기 위한 매핑
	// Upload 힙에서만 해당
	virtual void Map(void** ppData) = 0;
	virtual void Unmap() = 0;
};