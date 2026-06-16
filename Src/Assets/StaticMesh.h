#pragma once

#include "Math/Transform.h"
#include "Memory/StdAllocator.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Asset
{
	enum class PrimitiveMeshKind : uint8_t
	{
		None,
		Cube,
		Sphere,
		Capsule,
		Plane
	};

	enum class MaterialShadingModel : uint32_t
	{
		Phong,
		PBR,
		Unlit
	};

	enum class MaterialTextureSlot : uint32_t
	{
		BaseColor,
		Normal,
		Metallic,
		Roughness,
		MetallicRoughness,
		AO,
		Emissive,
		Opacity,
		Specular,
		Shininess,
		Count
	};

	inline constexpr size_t kMaterialTextureSlotCount = static_cast<size_t>(MaterialTextureSlot::Count);

	[[nodiscard]] constexpr size_t MaterialTextureSlotIndex(MaterialTextureSlot slot) noexcept
	{
		return static_cast<size_t>(slot);
	}

	[[nodiscard]] constexpr std::string_view MaterialShadingModelName(MaterialShadingModel model) noexcept
	{
		switch (model)
		{
		case MaterialShadingModel::PBR:
			return "PBR";
		case MaterialShadingModel::Unlit:
			return "Unlit";
		case MaterialShadingModel::Phong:
		default:
			return "Phong";
		}
	}

	[[nodiscard]] constexpr MaterialShadingModel MaterialShadingModelFromName(std::string_view text) noexcept
	{
		if (text == "PBR")
		{
			return MaterialShadingModel::PBR;
		}
		if (text == "Unlit")
		{
			return MaterialShadingModel::Unlit;
		}
		return MaterialShadingModel::Phong;
	}

	[[nodiscard]] constexpr std::string_view MaterialTextureSlotName(MaterialTextureSlot slot) noexcept
	{
		switch (slot)
		{
		case MaterialTextureSlot::BaseColor:
			return "Base Color";
		case MaterialTextureSlot::Normal:
			return "Normal";
		case MaterialTextureSlot::Metallic:
			return "Metallic";
		case MaterialTextureSlot::Roughness:
			return "Roughness";
		case MaterialTextureSlot::MetallicRoughness:
			return "Metallic/Roughness";
		case MaterialTextureSlot::AO:
			return "AO";
		case MaterialTextureSlot::Emissive:
			return "Emissive";
		case MaterialTextureSlot::Opacity:
			return "Opacity";
		case MaterialTextureSlot::Specular:
			return "Specular";
		case MaterialTextureSlot::Shininess:
			return "Shininess";
		case MaterialTextureSlot::Count:
		default:
			return "Unknown";
		}
	}

	[[nodiscard]] constexpr std::string_view MaterialTextureSlotKey(MaterialTextureSlot slot) noexcept
	{
		switch (slot)
		{
		case MaterialTextureSlot::BaseColor:
			return "baseColor";
		case MaterialTextureSlot::Normal:
			return "normal";
		case MaterialTextureSlot::Metallic:
			return "metallic";
		case MaterialTextureSlot::Roughness:
			return "roughness";
		case MaterialTextureSlot::MetallicRoughness:
			return "metallicRoughness";
		case MaterialTextureSlot::AO:
			return "ao";
		case MaterialTextureSlot::Emissive:
			return "emissive";
		case MaterialTextureSlot::Opacity:
			return "opacity";
		case MaterialTextureSlot::Specular:
			return "specular";
		case MaterialTextureSlot::Shininess:
			return "shininess";
		case MaterialTextureSlot::Count:
		default:
			return "unknown";
		}
	}

	[[nodiscard]] constexpr MaterialTextureSlot MaterialTextureSlotFromKey(std::string_view key) noexcept
	{
		if (key == "baseColor")
		{
			return MaterialTextureSlot::BaseColor;
		}
		if (key == "normal")
		{
			return MaterialTextureSlot::Normal;
		}
		if (key == "metallic")
		{
			return MaterialTextureSlot::Metallic;
		}
		if (key == "roughness")
		{
			return MaterialTextureSlot::Roughness;
		}
		if (key == "metallicRoughness")
		{
			return MaterialTextureSlot::MetallicRoughness;
		}
		if (key == "ao")
		{
			return MaterialTextureSlot::AO;
		}
		if (key == "emissive")
		{
			return MaterialTextureSlot::Emissive;
		}
		if (key == "opacity")
		{
			return MaterialTextureSlot::Opacity;
		}
		if (key == "specular")
		{
			return MaterialTextureSlot::Specular;
		}
		if (key == "shininess")
		{
			return MaterialTextureSlot::Shininess;
		}
		return MaterialTextureSlot::Count;
	}

	[[nodiscard]] constexpr bool IsMaterialTextureSlotSrgb(MaterialTextureSlot slot) noexcept
	{
		return slot == MaterialTextureSlot::BaseColor
			|| slot == MaterialTextureSlot::Emissive
			|| slot == MaterialTextureSlot::Specular;
	}

	struct StaticMeshVertex
	{
		DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
		DirectX::XMFLOAT2 TexCoord = { 0.0f, 0.0f };
		DirectX::XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 Tangent = { 1.0f, 0.0f, 0.0f };
		float TangentSign = 1.0f;
		std::array<uint32_t, 4> BoneIndices = { 0, 0, 0, 0 };
		std::array<float, 4> BoneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
	};

	struct StaticMeshSubmesh
	{
		uint32_t VertexOffset = 0;
		uint32_t VertexCount = 0;
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		uint32_t NodeIndex = 0;
		uint32_t MaterialIndex = 0;
		std::string Name;
	};

	struct EmbeddedMaterialTexture
	{
		Memory::Vector<unsigned char, Memory::MemoryTag::Asset> Pixels;
		int Width = 0;
		int Height = 0;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return Width > 0 && Height > 0 && !Pixels.empty();
		}
	};

	struct MaterialTextureBinding
	{
		std::filesystem::path Path;
		EmbeddedMaterialTexture Embedded;
		bool IsOverride = false;

		[[nodiscard]] bool HasSource() const noexcept
		{
			return !Path.empty() || Embedded.IsValid();
		}
	};

	struct StaticMeshMaterial
	{
		std::string Name;
		MaterialShadingModel ShadingModel = MaterialShadingModel::Phong;
		DirectX::XMFLOAT4 DiffuseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 ImportedDiffuseTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 SpecularColor = { 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
		float MetallicFactor = 0.0f;
		float RoughnessFactor = 0.5f;
		float Shininess = 32.0f;
		float Opacity = 1.0f;
		bool UseVertexColor = false;
		bool NormalYFlip = false;
		std::array<MaterialTextureBinding, kMaterialTextureSlotCount> TextureBindings = {};
		std::filesystem::path DiffuseTexturePath;
		Memory::Vector<unsigned char, Memory::MemoryTag::Asset> EmbeddedDiffuseTexturePixels;
		int EmbeddedDiffuseTextureWidth = 0;
		int EmbeddedDiffuseTextureHeight = 0;
		std::filesystem::path NormalTexturePath;
		std::filesystem::path MetallicRoughnessTexturePath;
	};

	[[nodiscard]] inline const MaterialTextureBinding& GetMaterialTextureBinding(const StaticMeshMaterial& material, MaterialTextureSlot slot) noexcept
	{
		return material.TextureBindings[MaterialTextureSlotIndex(slot)];
	}

	[[nodiscard]] inline MaterialTextureBinding& GetMaterialTextureBinding(StaticMeshMaterial& material, MaterialTextureSlot slot) noexcept
	{
		return material.TextureBindings[MaterialTextureSlotIndex(slot)];
	}

	inline void SetMaterialTexturePath(StaticMeshMaterial& material, MaterialTextureSlot slot, const std::filesystem::path& path, bool isOverride = false)
	{
		MaterialTextureBinding& binding = GetMaterialTextureBinding(material, slot);
		binding.Path = path;
		binding.Embedded = {};
		binding.IsOverride = isOverride;

		switch (slot)
		{
		case MaterialTextureSlot::BaseColor:
			material.DiffuseTexturePath = path;
			material.EmbeddedDiffuseTexturePixels.clear();
			material.EmbeddedDiffuseTextureWidth = 0;
			material.EmbeddedDiffuseTextureHeight = 0;
			break;
		case MaterialTextureSlot::Normal:
			material.NormalTexturePath = path;
			break;
		case MaterialTextureSlot::MetallicRoughness:
			material.MetallicRoughnessTexturePath = path;
			break;
		default:
			break;
		}
	}

	inline void SetMaterialEmbeddedTexture(StaticMeshMaterial& material, MaterialTextureSlot slot, EmbeddedMaterialTexture&& embedded)
	{
		MaterialTextureBinding& binding = GetMaterialTextureBinding(material, slot);
		binding.Path.clear();
		binding.Embedded = std::move(embedded);
		binding.IsOverride = false;

		if (slot == MaterialTextureSlot::BaseColor)
		{
			material.DiffuseTexturePath.clear();
			material.EmbeddedDiffuseTexturePixels.assign(binding.Embedded.Pixels.begin(), binding.Embedded.Pixels.end());
			material.EmbeddedDiffuseTextureWidth = binding.Embedded.Width;
			material.EmbeddedDiffuseTextureHeight = binding.Embedded.Height;
		}
	}

	[[nodiscard]] inline std::filesystem::path GetMaterialTexturePath(const StaticMeshMaterial& material, MaterialTextureSlot slot)
	{
		const MaterialTextureBinding& binding = GetMaterialTextureBinding(material, slot);
		if (!binding.Path.empty())
		{
			return binding.Path;
		}

		switch (slot)
		{
		case MaterialTextureSlot::BaseColor:
			return material.DiffuseTexturePath;
		case MaterialTextureSlot::Normal:
			return material.NormalTexturePath;
		case MaterialTextureSlot::MetallicRoughness:
			return material.MetallicRoughnessTexturePath;
		default:
			return {};
		}
	}

	struct AnimationVectorKey
	{
		double TimeTicks = 0.0;
		DirectX::XMFLOAT3 Value = { 0.0f, 0.0f, 0.0f };
	};

	struct AnimationQuaternionKey
	{
		double TimeTicks = 0.0;
		DirectX::XMFLOAT4 Value = { 0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct AnimationChannel
	{
		std::string NodeName;
		Memory::Vector<AnimationVectorKey, Memory::MemoryTag::Asset> PositionKeys;
		Memory::Vector<AnimationQuaternionKey, Memory::MemoryTag::Asset> RotationKeys;
		Memory::Vector<AnimationVectorKey, Memory::MemoryTag::Asset> ScalingKeys;
	};

	struct AnimationClip
	{
		std::string Name;
		double DurationTicks = 0.0;
		double TicksPerSecond = 25.0;
		Memory::Vector<AnimationChannel, Memory::MemoryTag::Asset> Channels;
		Memory::UnorderedMap<std::string, uint32_t, Memory::MemoryTag::Asset> ChannelIndices;
	};

	struct SkeletonNode
	{
		std::string Name;
		int32_t ParentIndex = -1;
		Math::Transform LocalBindPose = Math::Transform::Identity();
		DirectX::XMFLOAT4X4 LocalBindTransform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		Memory::Vector<uint32_t, Memory::MemoryTag::Asset> Children;
	};

	struct SkeletonBone
	{
		std::string Name;
		uint32_t NodeIndex = 0;
		DirectX::XMFLOAT4X4 OffsetMatrix = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
	};

	struct StaticMeshAsset
	{
		std::filesystem::path SourcePath;
		PrimitiveMeshKind PrimitiveKind = PrimitiveMeshKind::None;
		bool IsAnimated = false;
		uint32_t AnimationCount = 0;
		uint32_t BoneCount = 0;
		DirectX::XMFLOAT4X4 RootInverseTransform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		Memory::Vector<StaticMeshVertex, Memory::MemoryTag::Asset> Vertices;
		Memory::Vector<StaticMeshVertex, Memory::MemoryTag::Asset> BindPoseVertices;
		Memory::Vector<uint32_t, Memory::MemoryTag::Asset> Indices;
		Memory::Vector<StaticMeshSubmesh, Memory::MemoryTag::Asset> Submeshes;
		Memory::Vector<StaticMeshMaterial, Memory::MemoryTag::Asset> Materials;
		Memory::Vector<SkeletonNode, Memory::MemoryTag::Asset> Nodes;
		Memory::Vector<SkeletonBone, Memory::MemoryTag::Asset> Bones;
		Memory::Vector<AnimationClip, Memory::MemoryTag::Asset> Animations;
		Memory::UnorderedMap<std::string, uint32_t, Memory::MemoryTag::Asset> NodeIndices;
		Memory::UnorderedMap<std::string, uint32_t, Memory::MemoryTag::Asset> BoneIndices;
	};
}
