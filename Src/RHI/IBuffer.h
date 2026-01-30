#pragma once
#include "GraphicsCommon.h"
#include "IGpuResource.h"

class IBuffer : public IGpuResource
{
public:
	virtual ~IBuffer() = default;

	// 버퍼 크기(바이트 단위) 반환
	virtual uint64_t GetSize() const = 0;

	// CPU에서 버퍼 메모리에 데이터 쓰기
	// Upload 힙에만 해당
	virtual void Map(void** ppData) = 0;
	virtual void Unmap() = 0;
};