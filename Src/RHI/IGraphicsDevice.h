#pragma once
#include "GraphicsCommon.h"

class ICommandList;
class IBuffer;
class IGpuResource;

struct BufferDesc
{
	uint64_t Size;
	HeapType Heap;       // Default vs Upload
	ResourceState InitialState;
	// const char* DebugName; // 디버깅용 이름
};

class IGraphicsDevice
{
public:
	virtual ~IGraphicsDevice() = default;

	// 초기화 및 종료
	virtual bool Init() = 0;
	virtual void Shutdown() = 0;

	// CPU-GPU 동기화 (Fence)
	virtual void WaitForGPU() = 0;
	virtual void MoveToNextFrame() = 0; // SwapChain Present 후 인덱스 관리

	// 생성
	virtual ICommandList* CreateCommandList() = 0;

	// 버퍼 생성 (버텍스, 인덱스, 상수 버퍼 통합)
	virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;

	// 화면 출력
	virtual void Present() = 0;

	// 현재 백버퍼의 RTV 핸들을 얻어오는 함수 (추상화 레벨 조절 필요)
	virtual void* GetCurrentBackBufferRTV() = 0;
	virtual void* GetDepthStencilView() = 0;

	// 명령 리스트 제출 함수
	virtual void ExecuteCommandList(ICommandList* cmdList) = 0;

	// 현재 백버퍼 리소스를 가져오는 함수 (Barrier용)
	virtual IGpuResource* GetBackBufferResource() = 0;

	// 팩토리 메서드
	// windowHandle은 플랫폼별 윈도우 핸들 (예: HWND)
	static IGraphicsDevice* Create(GraphicsAPI api, void* windowHandle, int width, int height);
};