// GraphicsCommon.h: 다양한 그래픽 공통 정의를 제공
#pragma once
#include <cstdint>

enum class GraphicsAPI : std::uint8_t
{
	DirectX12,
	Vulkan
};

enum class ShaderType : std::uint8_t
{
	Vertex,
	Pixel,
	Geometry,
	Compute,
	Domain,
	Hull
};

enum class TextureFormat : std::uint8_t
{
	RGBA8,
	RGB8,
	RGBA16F,
	RGBA32F,
	D24S8,
	D32F,
	BGRA8,
	RGBA8_UNORM, // 일반적인 8비트 RGBA 포맷
	BGRA8_UNORM, // 백버퍼용으로 자주 사용
	D32_FLOAT,   // 깊이 버퍼
};

// 메모리 힙 종류 (CPU-GPU 간 사용 방식 구분)
enum class HeapType : std::uint8_t
{
	Default,  // GPU 전용 (VRAM). CPU 접근 불가. 렌더링용.
	Upload,   // CPU -> GPU (시스템 메모리). 업로드용.
	Readback  // GPU -> CPU (결과 읽기용).
};

enum class ResourceState : std::uint8_t
{
	Common,					// 기본 상태
	VertexAndConstantBuffer,// VBV, CBV
	IndexBuffer,			// IBV
	RenderTarget,			// RTV
	DepthStencil,			// DSV
	ShaderResource,			// SRV
	UnorderedAccess,		// UAV
	PixelShaderResource,    // 픽셀 셰이더에서 읽기
	NonPixelShaderResource, // 컴퓨트/버텍스 셰이더 등에서 읽기
	CopyDest,               // 복사 대상 리소스
	CopySource,             // 복사 소스 리소스
	Present,                // 화면 출력용 (Swapchain Present)
	GenericRead             // 일반적인 읽기
};