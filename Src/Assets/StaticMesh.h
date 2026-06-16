#pragma once

#include "Math/Transform.h"
#include "Memory/StdAllocator.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
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

	struct StaticMeshVertex
	{
		DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Normal = { 0.0f, 1.0f, 0.0f };
		DirectX::XMFLOAT2 TexCoord = { 0.0f, 0.0f };
		DirectX::XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT3 Tangent = { 1.0f, 0.0f, 0.0f };
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

	struct StaticMeshMaterial
	{
		std::string Name;
		DirectX::XMFLOAT4 DiffuseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::filesystem::path DiffuseTexturePath;
		Memory::Vector<unsigned char, Memory::MemoryTag::Asset> EmbeddedDiffuseTexturePixels;
		int EmbeddedDiffuseTextureWidth = 0;
		int EmbeddedDiffuseTextureHeight = 0;
		std::filesystem::path NormalTexturePath;
		std::filesystem::path MetallicRoughnessTexturePath;
	};

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
