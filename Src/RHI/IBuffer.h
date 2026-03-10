#pragma once
#include "GraphicsCommon.h"
#include "IGpuResource.h"

class IBuffer : public IGpuResource
{
public:
	virtual ~IBuffer() = default;

	// 버퍼 크기(바이트 단위) 반환
	virtual uint64_t GetSize() const = 0;
	virtual uint32_t GetStride() const = 0;

	// CPU에서 업로드 메모리에 접근하기 위한 매핑
	// Upload 힙에서만 해당
	virtual void Map(void** ppData) = 0;
	virtual void Unmap() = 0;
};