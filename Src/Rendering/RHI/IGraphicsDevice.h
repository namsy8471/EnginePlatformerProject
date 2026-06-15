#pragma once
#include "GraphicsCommon.h"

class ICommandList;
class IBuffer;
class IGpuResource;

struct BufferDesc
{
	uint64_t Size;
	uint32_t Stride = 0;
	HeapType Heap;       // Default vs Upload
	ResourceState InitialState;
	// const char* DebugName; // 디버그 이름
};

class IGraphicsDevice
{
public:
	virtual ~IGraphicsDevice() = default;

	// 초기화 및 종료
	[[nodiscard]] virtual bool Init() = 0;
	virtual void Shutdown() = 0;

	// CPU-GPU 동기화 (Fence)
	virtual void WaitForGPU() = 0;
	virtual void MoveToNextFrame() = 0; // SwapChain Present 후 인덱스 이동

	// 생성
	[[nodiscard]] virtual ICommandList* CreateCommandList() = 0;

	// 버퍼 생성 (버텍스, 인덱스, 업로드 버퍼 등)
	[[nodiscard]] virtual IBuffer* CreateBuffer(const BufferDesc& desc) = 0;

	// 화면 표시
	virtual void Present() = 0;
	virtual void Resize(int width, int height) = 0;

	// 현재 백버퍼 RTV 핸들을 반환하는 함수 (추상화 계층 유지)
	[[nodiscard]] virtual void* GetCurrentBackBufferRTV() = 0;
	[[nodiscard]] virtual void* GetDepthStencilView() = 0;

	// 커맨드 리스트 실행 함수
	virtual void ExecuteCommandList(ICommandList* cmdList) = 0;

	// 현재 백버퍼 리소스를 반환하는 함수 (Barrier용)
	[[nodiscard]] virtual IGpuResource* GetBackBufferResource() = 0;

	// 팩토리 메서드
	// windowHandle은 플랫폼별 윈도우 핸들 (예: HWND)
	[[nodiscard]] static IGraphicsDevice* Create(GraphicsAPI api, void* windowHandle, int width, int height);
};