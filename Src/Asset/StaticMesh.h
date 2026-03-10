#pragma once

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Asset
{
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
		std::filesystem::path DiffuseTexturePath;
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
		std::vector<AnimationVectorKey> PositionKeys;
		std::vector<AnimationQuaternionKey> RotationKeys;
		std::vector<AnimationVectorKey> ScalingKeys;
	};

	struct AnimationClip
	{
		std::string Name;
		double DurationTicks = 0.0;
		double TicksPerSecond = 25.0;
		std::vector<AnimationChannel> Channels;
		std::unordered_map<std::string, uint32_t> ChannelIndices;
	};

	struct SkeletonNode
	{
		std::string Name;
		int32_t ParentIndex = -1;
		DirectX::XMFLOAT4X4 LocalBindTransform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		std::vector<uint32_t> Children;
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
		bool IsAnimated = false;
		uint32_t AnimationCount = 0;
		uint32_t BoneCount = 0;
		DirectX::XMFLOAT4X4 RootInverseTransform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f };
		std::vector<StaticMeshVertex> Vertices;
		std::vector<StaticMeshVertex> BindPoseVertices;
		std::vector<uint32_t> Indices;
		std::vector<StaticMeshSubmesh> Submeshes;
		std::vector<StaticMeshMaterial> Materials;
		std::vector<SkeletonNode> Nodes;
		std::vector<SkeletonBone> Bones;
		std::vector<AnimationClip> Animations;
		std::unordered_map<std::string, uint32_t> NodeIndices;
		std::unordered_map<std::string, uint32_t> BoneIndices;
	};
}
