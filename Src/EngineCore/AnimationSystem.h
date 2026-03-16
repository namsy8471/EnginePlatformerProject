#pragma once

#include "Scene.h"
#include "RHI/IBuffer.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace AnimationSystem
{
	[[nodiscard]] inline DirectX::XMMATRIX LoadMatrix(const DirectX::XMFLOAT4X4& matrix)
	{
		return DirectX::XMLoadFloat4x4(&matrix);
	}

	[[nodiscard]] inline DirectX::XMVECTOR InterpolateVectorKey(const std::vector<Asset::AnimationVectorKey>& keys, double timeTicks)
	{
		if (keys.empty())
		{
			return DirectX::XMVectorZero();
		}

		if (keys.size() == 1 || timeTicks <= keys.front().TimeTicks)
		{
			return DirectX::XMLoadFloat3(&keys.front().Value);
		}

		for (size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex)
		{
			if (timeTicks < keys[keyIndex + 1].TimeTicks)
			{
				const double delta = keys[keyIndex + 1].TimeTicks - keys[keyIndex].TimeTicks;
				const float factor = delta > 0.0 ? static_cast<float>((timeTicks - keys[keyIndex].TimeTicks) / delta) : 0.0f;
				return DirectX::XMVectorLerp(
					DirectX::XMLoadFloat3(&keys[keyIndex].Value),
					DirectX::XMLoadFloat3(&keys[keyIndex + 1].Value),
					factor);
			}
		}

		return DirectX::XMLoadFloat3(&keys.back().Value);
	}

	[[nodiscard]] inline DirectX::XMVECTOR InterpolateQuaternionKey(const std::vector<Asset::AnimationQuaternionKey>& keys, double timeTicks)
	{
		if (keys.empty())
		{
			return DirectX::XMQuaternionIdentity();
		}

		if (keys.size() == 1 || timeTicks <= keys.front().TimeTicks)
		{
			return DirectX::XMLoadFloat4(&keys.front().Value);
		}

		for (size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex)
		{
			if (timeTicks < keys[keyIndex + 1].TimeTicks)
			{
				const double delta = keys[keyIndex + 1].TimeTicks - keys[keyIndex].TimeTicks;
				const float factor = delta > 0.0 ? static_cast<float>((timeTicks - keys[keyIndex].TimeTicks) / delta) : 0.0f;
				return DirectX::XMQuaternionSlerp(
					DirectX::XMLoadFloat4(&keys[keyIndex].Value),
					DirectX::XMLoadFloat4(&keys[keyIndex + 1].Value),
					factor);
			}
		}

		return DirectX::XMLoadFloat4(&keys.back().Value);
	}

	[[nodiscard]] inline Math::Transform InterpolateChannelTransform(const Asset::AnimationChannel& channel, const Math::Transform& fallbackTransform, double timeTicks)
	{
		Math::Transform interpolatedTransform = fallbackTransform;
		if (!channel.PositionKeys.empty())
		{
			Math::Store(interpolatedTransform.Translation, InterpolateVectorKey(channel.PositionKeys, timeTicks));
		}
		if (!channel.RotationKeys.empty())
		{
			Math::Store(interpolatedTransform.Rotation, InterpolateQuaternionKey(channel.RotationKeys, timeTicks));
		}
		if (!channel.ScalingKeys.empty())
		{
			Math::Store(interpolatedTransform.Scale, InterpolateVectorKey(channel.ScalingKeys, timeTicks));
		}
		return interpolatedTransform;
	}

	[[nodiscard]] inline Math::Transform BuildAnimatedLocalTransform(const Asset::SkeletonNode& node, const Asset::AnimationClip& clip, double timeTicks)
	{
		auto channelIndex = clip.ChannelIndices.find(node.Name);
		if (channelIndex == clip.ChannelIndices.end())
		{
			return node.LocalBindPose;
		}

		const Asset::AnimationChannel& channel = clip.Channels[channelIndex->second];
		return InterpolateChannelTransform(channel, node.LocalBindPose, timeTicks);
	}

	inline void ComputeGlobalNodeTransforms(const Asset::StaticMeshAsset& meshAsset, const std::vector<Math::Transform>& localTransforms, uint32_t nodeIndex, const Math::Transform& parentTransform, std::vector<Math::Transform>& globalTransforms)
	{
		globalTransforms[nodeIndex] = localTransforms[nodeIndex] * parentTransform;
		for (uint32_t childIndex : meshAsset.Nodes[nodeIndex].Children)
		{
			ComputeGlobalNodeTransforms(meshAsset, localTransforms, childIndex, globalTransforms[nodeIndex], globalTransforms);
		}
	}

	inline void UpdateAnimatedMesh(Scene& scene, EntityId entityId, float deltaTime, float& animationTimeSeconds, IBuffer* vertexBuffer)
	{
		Asset::StaticMeshAsset* meshAsset = scene.GetMeshAsset(entityId);
		if (!meshAsset || !meshAsset->IsAnimated || meshAsset->Animations.empty() || meshAsset->Bones.empty())
		{
			return;
		}

		const Asset::AnimationClip& clip = meshAsset->Animations.front();
		if (clip.DurationTicks <= 0.0)
		{
			return;
		}

		animationTimeSeconds += deltaTime;
		const double ticksPerSecond = clip.TicksPerSecond > 0.0 ? clip.TicksPerSecond : 25.0;
		const double animationTimeTicks = std::fmod(static_cast<double>(animationTimeSeconds) * ticksPerSecond, clip.DurationTicks);

		std::vector<Math::Transform> localTransforms;
		localTransforms.reserve(meshAsset->Nodes.size());
		for (const auto& node : meshAsset->Nodes)
		{
			localTransforms.push_back(BuildAnimatedLocalTransform(node, clip, animationTimeTicks));
		}

		std::vector<Math::Transform> globalTransforms(meshAsset->Nodes.size(), Math::Transform::Identity());
		if (!meshAsset->Nodes.empty())
		{
			ComputeGlobalNodeTransforms(*meshAsset, localTransforms, 0, Math::Transform::Identity(), globalTransforms);
		}

		std::vector<DirectX::XMMATRIX> boneMatrices(meshAsset->Bones.size(), DirectX::XMMatrixIdentity());
		const DirectX::XMMATRIX rootInverseTransform = LoadMatrix(meshAsset->RootInverseTransform);
		for (size_t boneIndex = 0; boneIndex < meshAsset->Bones.size(); ++boneIndex)
		{
			const auto& bone = meshAsset->Bones[boneIndex];
			boneMatrices[boneIndex] = LoadMatrix(bone.OffsetMatrix) * globalTransforms[bone.NodeIndex].ToXmMatrix() * rootInverseTransform;
		}

		meshAsset->Vertices = meshAsset->BindPoseVertices;
		for (size_t vertexIndex = 0; vertexIndex < meshAsset->Vertices.size(); ++vertexIndex)
		{
			const auto& bindVertex = meshAsset->BindPoseVertices[vertexIndex];
			auto& animatedVertex = meshAsset->Vertices[vertexIndex];

			const DirectX::XMVECTOR bindPosition = DirectX::XMLoadFloat3(&bindVertex.Position);
			const DirectX::XMVECTOR bindNormal = DirectX::XMLoadFloat3(&bindVertex.Normal);
			DirectX::XMVECTOR skinnedPosition = DirectX::XMVectorZero();
			DirectX::XMVECTOR skinnedNormal = DirectX::XMVectorZero();
			float totalWeight = 0.0f;

			for (size_t influenceIndex = 0; influenceIndex < bindVertex.BoneWeights.size(); ++influenceIndex)
			{
				const float weight = bindVertex.BoneWeights[influenceIndex];
				if (weight <= 0.0f)
				{
					continue;
				}

				const uint32_t boneIndex = bindVertex.BoneIndices[influenceIndex];
				const DirectX::XMMATRIX boneMatrix = boneIndex < boneMatrices.size() ? boneMatrices[boneIndex] : DirectX::XMMatrixIdentity();
				skinnedPosition += DirectX::XMVectorScale(DirectX::XMVector3TransformCoord(bindPosition, boneMatrix), weight);
				skinnedNormal += DirectX::XMVectorScale(DirectX::XMVector3TransformNormal(bindNormal, boneMatrix), weight);
				totalWeight += weight;
			}

			if (totalWeight > 0.0f)
			{
				DirectX::XMStoreFloat3(&animatedVertex.Position, skinnedPosition);
				const DirectX::XMVECTOR normalizedNormal = DirectX::XMVector3Normalize(skinnedNormal);
				DirectX::XMStoreFloat3(&animatedVertex.Normal, normalizedNormal);
			}
		}

		if (!vertexBuffer)
		{
			return;
		}

		void* mappedData = nullptr;
		vertexBuffer->Map(&mappedData);
		std::memcpy(mappedData, meshAsset->Vertices.data(), meshAsset->Vertices.size() * sizeof(Asset::StaticMeshVertex));
		vertexBuffer->Unmap();
	}
}
