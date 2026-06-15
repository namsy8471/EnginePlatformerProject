#pragma once

#include "Rendering/RHI/IBuffer.h"

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
using VkImage = struct VkImage_T*;
using VkImageView = struct VkImageView_T*;
using VkPipeline = struct VkPipeline_T*;
using VkPipelineLayout = struct VkPipelineLayout_T*;
using VkSampler = struct VkSampler_T*;
using VkShaderModule = struct VkShaderModule_T*;

namespace Rendering
{
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
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderResourceHeap;
		std::vector<MaterialTexture> MaterialTextures;
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
		VkPipelineLayout PipelineLayout = nullptr;
		VkPipeline Pipeline = nullptr;
		VkPipeline TransparentPipeline = nullptr;
		bool IsValid = false;
	};

	struct StaticMeshRenderer
	{
		IBuffer* VertexBuffer = nullptr;
		IBuffer* IndexBuffer = nullptr;
		IBuffer* CameraBuffer = nullptr;
		Dx12StaticMeshResources Dx12;
		VulkanStaticMeshResources Vulkan;
	};
}
