#pragma once

#include "Rendering/RHI/IBuffer.h"
#include "Rendering/Systems/RenderSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <wrl.h>

struct ID3D12DescriptorHeap;
struct ID3D12PipelineState;
struct ID3D12Resource;
struct ID3D12RootSignature;
using VkDescriptorPool = struct VkDescriptorPool_T*;
using VkDescriptorSet = struct VkDescriptorSet_T*;
using VkDescriptorSetLayout = struct VkDescriptorSetLayout_T*;
using VkDeviceMemory = struct VkDeviceMemory_T*;
using VkFramebuffer = struct VkFramebuffer_T*;
using VkImage = struct VkImage_T*;
using VkImageView = struct VkImageView_T*;
using VkPipeline = struct VkPipeline_T*;
using VkPipelineLayout = struct VkPipelineLayout_T*;
using VkRenderPass = struct VkRenderPass_T*;
using VkSampler = struct VkSampler_T*;
using VkShaderModule = struct VkShaderModule_T*;

namespace Rendering
{
	enum class DrawSubmissionKind : std::uint8_t
	{
		Opaque,
		Transparent,
		Shadow,
		DeferredGeometry,
		Fullscreen,
		Benchmark
	};

	struct RenderFrameStats
	{
		uint64_t FrameIndex = 0;
		uint32_t RenderEntityCount = 0;
		uint32_t EnabledMeshEntityCount = 0;
		uint32_t TransparentEntityCount = 0;
		uint32_t DrawCallCount = 0;
		uint32_t IndexedDrawCallCount = 0;
		uint32_t FullscreenDrawCallCount = 0;
		uint32_t InstancedDrawCallCount = 0;
		uint32_t OpaqueDrawCallCount = 0;
		uint32_t TransparentDrawCallCount = 0;
		uint32_t ShadowDrawCallCount = 0;
		uint32_t DeferredGeometryDrawCallCount = 0;
		uint32_t BenchmarkDrawCallCount = 0;
		uint64_t SubmittedIndexCount = 0;
		uint64_t SubmittedTriangleCount = 0;
		uint64_t SubmittedInstanceCount = 0;
		uint32_t ViewCullingRequestCount = 0;
		uint32_t ViewCullingTestCount = 0;
		uint32_t ViewVisibleListEntityCount = 0;
		uint32_t ViewVisibleEntityCount = 0;
		uint32_t ViewCulledEntityCount = 0;
		uint32_t ViewCullingCacheHitCount = 0;
		uint32_t ViewCullingCacheMissCount = 0;
		uint32_t SceneViewCullingRequestCount = 0;
		uint32_t SceneViewVisibleListEntityCount = 0;
		uint32_t SceneViewCulledEntityCount = 0;
		uint32_t GameViewCullingRequestCount = 0;
		uint32_t GameViewVisibleListEntityCount = 0;
		uint32_t GameViewCulledEntityCount = 0;
		uint32_t DeferredTileViewportCount = 0;
		uint32_t DeferredTileCountTotal = 0;
		uint32_t DeferredTileLightReferenceCount = 0;
		uint32_t DeferredMaxTileLightCount = 0;
		uint32_t DeferredFullTileLightCount = 0;
	};

	struct Dx12StaticMeshResources
	{
		struct MaterialTexture
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
			Microsoft::WRL::ComPtr<ID3D12Resource> TextureUpload;
		};

		Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> TransparentPipelineState;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyboxRootSignature;
		Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyboxPipelineState;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
		std::vector<MaterialTexture> MaterialTextures;
		size_t MaterialCount = 0;

		struct DeferredResources
		{
			static constexpr size_t GBufferCount = 4;
			static constexpr size_t ShadowSrvIndex = GBufferCount;
			static constexpr size_t LightingSrvCount = GBufferCount + 1;
			static constexpr size_t HdrSrvIndex = LightingSrvCount;
			static constexpr size_t PostProcessSrvCount = LightingSrvCount + 1;
			std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, GBufferCount> GBufferTextures;
			Microsoft::WRL::ComPtr<ID3D12Resource> HdrColorTexture;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRtvHeap;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> HdrRtvHeap;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferSrvHeap;
			Microsoft::WRL::ComPtr<ID3D12PipelineState> GeometryPipelineState;
			Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
			Microsoft::WRL::ComPtr<ID3D12PipelineState> LightingPipelineState;
			Microsoft::WRL::ComPtr<ID3D12RootSignature> ToneMapRootSignature;
			Microsoft::WRL::ComPtr<ID3D12PipelineState> ToneMapPipelineState;
			uint32_t Width = 0;
			uint32_t Height = 0;
			uint32_t RtvDescriptorSize = 0;
			bool IsValid = false;
		} Deferred;

		struct ShadowResources
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> DepthTexture;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DsvHeap;
			Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
			Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
			uint32_t Size = 0;
			bool IsValid = false;
		} Shadow;

		struct EntityMaterialResources
		{
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
			std::vector<MaterialTexture> MaterialTextures;
			size_t MaterialCount = 0;
		};
		std::unordered_map<uint32_t, EntityMaterialResources> EntityMaterials;
	};

	struct VulkanStaticMeshResources
	{
		struct MaterialTexture
		{
			VkImage Image = nullptr;
			VkDeviceMemory ImageMemory = nullptr;
			VkImageView ImageView = nullptr;
			VkSampler Sampler = nullptr;
		};

		VkShaderModule VertexShader = nullptr;
		VkShaderModule PixelShader = nullptr;
		VkDescriptorSetLayout DescriptorSetLayout = nullptr;
		VkDescriptorPool DescriptorPool = nullptr;
		std::vector<VkDescriptorSet> DescriptorSets;
		std::vector<MaterialTexture> MaterialTextures;
		size_t MaterialCount = 0;
		VkPipelineLayout PipelineLayout = nullptr;
		VkPipeline Pipeline = nullptr;
		VkPipeline TransparentPipeline = nullptr;
		VkShaderModule SkyboxVertexShader = nullptr;
		VkShaderModule SkyboxFragmentShader = nullptr;
		VkPipelineLayout SkyboxPipelineLayout = nullptr;
		VkPipeline SkyboxPipeline = nullptr;
		bool IsValid = false;

		struct DeferredResources
		{
			static constexpr size_t GBufferCount = 4;
			std::array<VkImage, GBufferCount> GBufferImages = {};
			std::array<VkDeviceMemory, GBufferCount> GBufferMemories = {};
			std::array<VkImageView, GBufferCount> GBufferImageViews = {};
			VkImage HdrColorImage = nullptr;
			VkDeviceMemory HdrColorMemory = nullptr;
			VkImageView HdrColorImageView = nullptr;
			VkImage DepthImage = nullptr;
			VkDeviceMemory DepthMemory = nullptr;
			VkImageView DepthImageView = nullptr;
			VkSampler GBufferSampler = nullptr;
			VkSampler HdrSampler = nullptr;
			VkRenderPass GeometryRenderPass = nullptr;
			VkRenderPass LightingRenderPass = nullptr;
			VkFramebuffer GeometryFramebuffer = nullptr;
			VkFramebuffer LightingFramebuffer = nullptr;
			VkShaderModule GeometryVertexShader = nullptr;
			VkShaderModule GeometryFragmentShader = nullptr;
			VkShaderModule LightingVertexShader = nullptr;
			VkShaderModule LightingFragmentShader = nullptr;
			VkShaderModule ToneMapVertexShader = nullptr;
			VkShaderModule ToneMapFragmentShader = nullptr;
			VkDescriptorSetLayout LightingDescriptorSetLayout = nullptr;
			VkDescriptorSetLayout ToneMapDescriptorSetLayout = nullptr;
			VkDescriptorPool LightingDescriptorPool = nullptr;
			VkDescriptorPool ToneMapDescriptorPool = nullptr;
			VkDescriptorSet LightingDescriptorSet = nullptr;
			VkDescriptorSet ToneMapDescriptorSet = nullptr;
			VkPipelineLayout LightingPipelineLayout = nullptr;
			VkPipelineLayout ToneMapPipelineLayout = nullptr;
			VkPipeline GeometryPipeline = nullptr;
			VkPipeline LightingPipeline = nullptr;
			VkPipeline ToneMapPipeline = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool IsValid = false;
		} Deferred;

		struct ShadowResources
		{
			VkImage DepthImage = nullptr;
			VkDeviceMemory DepthMemory = nullptr;
			VkImageView DepthImageView = nullptr;
			VkSampler DepthSampler = nullptr;
			VkRenderPass RenderPass = nullptr;
			VkFramebuffer Framebuffer = nullptr;
			VkShaderModule VertexShader = nullptr;
			VkPipeline Pipeline = nullptr;
			uint32_t Size = 0;
			bool IsValid = false;
		} Shadow;

		struct EntityMaterialResources
		{
			VkDescriptorPool DescriptorPool = nullptr;
			std::vector<VkDescriptorSet> DescriptorSets;
			std::vector<MaterialTexture> MaterialTextures;
			size_t MaterialCount = 0;
		};
		std::unordered_map<uint32_t, EntityMaterialResources> EntityMaterials;
	};

	struct StaticMeshRenderer
	{
		std::unique_ptr<IBuffer> VertexBuffer;
		std::unique_ptr<IBuffer> IndexBuffer;
		std::unique_ptr<IBuffer> CameraBuffer;
		std::unique_ptr<IBuffer> DeferredLightBuffer;
		std::unique_ptr<IBuffer> DeferredLightingBuffer;
		std::unique_ptr<IBuffer> DeferredTileRangeBuffer;
		std::unique_ptr<IBuffer> DeferredTileLightIndexBuffer;
		std::vector<LightGpuData> DeferredCpuLights;
		uint64_t CameraBufferStride = 256;
		uint32_t CameraBufferCapacity = 0;
		uint32_t CameraBufferCursor = 0;
		uint32_t DeferredLightBufferCapacity = 0;
		uint32_t DeferredLightCount = 0;
		uint32_t DeferredTileRangeCapacity = 0;
		uint32_t DeferredTileLightIndexCapacity = 0;
		uint32_t DeferredTileCountX = 0;
		uint32_t DeferredTileCountY = 0;
		uint32_t DeferredTileLightReferenceCount = 0;
		uint32_t DeferredMaxTileLightCount = 0;
		uint32_t DeferredFullTileLightCount = 0;
		Dx12StaticMeshResources Dx12;
		VulkanStaticMeshResources Vulkan;
	};
}
