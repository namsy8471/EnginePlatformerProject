// GraphicsCommon.h 다양한 그래픽 관련 열거형 정의
#pragma once
#include <cstdint>

enum class GraphicsAPI : std::uint8_t
{
	Direct3D12,
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
	BGRA8_UNORM, // 스왑체인용으로 자주 쓰임
	D32_FLOAT,   // 깊이 버퍼
};

// 메모리 힙 유형 (CPU-GPU 간 데이터 전송 전략)
enum class HeapType : std::uint8_t
{
	Default,  // GPU 전용 (VRAM). CPU 접근 불가. 가장 빠름.
	Upload,   // CPU -> GPU (시스템 메모리). 맵핑 가능.
	Readback  // GPU -> CPU (스크린샷 등).
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
	CopyDest,               // 데이터 복사 목적지 (Upload용)
	CopySource,             // 데이터 복사 원본
	Present,                // 화면 출력용 (Swapchain Present) 필수!
	GenericRead             // 일반적인 읽기
};