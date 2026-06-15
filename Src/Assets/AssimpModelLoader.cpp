#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "AssimpModelLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>

namespace Asset
{
	namespace
	{
		struct LoadedTextureImage
		{
			int Width = 0;
			int Height = 0;
			int Channels = 0;
			std::vector<unsigned char> Pixels;

			[[nodiscard]] bool IsValid() const noexcept
			{
				return Width > 0 && Height > 0 && !Pixels.empty();
			}
		};

		void LogAssimpMessage(std::string_view message)
		{
			std::string buffer = "[AssimpLoader] ";
			buffer.append(message);
			buffer.push_back('\n');
			OutputDebugStringA(buffer.c_str());
		}

		[[nodiscard]] DirectX::XMFLOAT3 ToFloat3(const aiVector3D& value)
		{
			return { value.x, value.y, value.z };
		}

		[[nodiscard]] DirectX::XMFLOAT4 ToFloat4(const aiQuaternion& value)
		{
			return { value.x, value.y, value.z, value.w };
		}

		[[nodiscard]] DirectX::XMFLOAT4X4 ToMatrix(const aiMatrix4x4& matrix)
		{
			return {
				matrix.a1, matrix.b1, matrix.c1, matrix.d1,
				matrix.a2, matrix.b2, matrix.c2, matrix.d2,
				matrix.a3, matrix.b3, matrix.c3, matrix.d3,
				matrix.a4, matrix.b4, matrix.c4, matrix.d4
			};
		}

		[[nodiscard]] std::string ToLowerInvariant(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
				{
					return static_cast<char>(std::tolower(character));
				});
			return value;
		}

		struct TextureResolveResult
		{
			std::string RawPath;
			std::vector<std::filesystem::path> Candidates;
			std::filesystem::path SelectedPath;
			bool IsEmbeddedReference = false;
		};

		[[nodiscard]] const char* TextureTypeName(aiTextureType textureType) noexcept
		{
			switch (textureType)
			{
			case aiTextureType_DIFFUSE:
				return "DIFFUSE";
			case aiTextureType_NORMALS:
				return "NORMALS";
			case aiTextureType_NORMAL_CAMERA:
				return "NORMAL_CAMERA";
			case aiTextureType_BASE_COLOR:
				return "BASE_COLOR";
			case aiTextureType_UNKNOWN:
				return "UNKNOWN";
			default:
				return "OTHER";
			}
		}

		[[nodiscard]] std::string PathKey(const std::filesystem::path& path)
		{
			return ToLowerInvariant(path.lexically_normal().string());
		}

		void AddUniqueCandidate(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate)
		{
			if (candidate.empty())
			{
				return;
			}

			const std::string candidateKey = PathKey(candidate);
			const auto duplicateIt = std::find_if(candidates.begin(), candidates.end(), [&candidateKey](const std::filesystem::path& existing)
				{
					return PathKey(existing) == candidateKey;
				});
			if (duplicateIt == candidates.end())
			{
				candidates.push_back(candidate.lexically_normal());
			}
		}

		void AddCandidateWithPreferredVariants(std::vector<std::filesystem::path>& candidates, const std::filesystem::path& candidate)
		{
			if (candidate.empty())
			{
				return;
			}

			const std::string extension = ToLowerInvariant(candidate.extension().string());
			if (extension != ".png" && extension != ".tga")
			{
				auto pngPath = candidate;
				pngPath.replace_extension(".png");
				AddUniqueCandidate(candidates, pngPath);

				auto tgaPath = candidate;
				tgaPath.replace_extension(".tga");
				AddUniqueCandidate(candidates, tgaPath);
			}

			AddUniqueCandidate(candidates, candidate);
		}

		[[nodiscard]] bool IsReadableTextureFile(const std::filesystem::path& texturePath)
		{
			std::error_code errorCode;
			if (!std::filesystem::is_regular_file(texturePath, errorCode))
			{
				return false;
			}

			int width = 0;
			int height = 0;
			int channels = 0;
			return stbi_info(texturePath.string().c_str(), &width, &height, &channels) != 0 && width > 0 && height > 0;
		}

		void AddRecursiveTextureMatches(
			std::vector<std::filesystem::path>& candidates,
			const std::filesystem::path& modelDirectory,
			const std::filesystem::path& importedFilename)
		{
			if (modelDirectory.empty() || importedFilename.empty())
			{
				return;
			}

			std::error_code errorCode;
			if (!std::filesystem::is_directory(modelDirectory, errorCode))
			{
				return;
			}

			std::vector<std::filesystem::path> exactMatches;
			std::vector<std::filesystem::path> variantMatches;
			const std::string filenameKey = ToLowerInvariant(importedFilename.filename().string());
			const std::string stemKey = ToLowerInvariant(importedFilename.stem().string());

			for (std::filesystem::recursive_directory_iterator it(modelDirectory, std::filesystem::directory_options::skip_permission_denied, errorCode), end;
				it != end && !errorCode;
				it.increment(errorCode))
			{
				std::error_code fileErrorCode;
				if (!it->is_regular_file(fileErrorCode))
				{
					continue;
				}

				const std::filesystem::path candidatePath = it->path();
				if (ToLowerInvariant(candidatePath.filename().string()) == filenameKey)
				{
					exactMatches.push_back(candidatePath);
					continue;
				}

				const std::string extension = ToLowerInvariant(candidatePath.extension().string());
				if (ToLowerInvariant(candidatePath.stem().string()) == stemKey && (extension == ".png" || extension == ".tga"))
				{
					variantMatches.push_back(candidatePath);
				}
			}

			std::sort(variantMatches.begin(), variantMatches.end());
			std::sort(exactMatches.begin(), exactMatches.end());
			for (const std::filesystem::path& match : variantMatches)
			{
				AddUniqueCandidate(candidates, match);
			}
			for (const std::filesystem::path& match : exactMatches)
			{
				AddUniqueCandidate(candidates, match);
			}
		}

		[[nodiscard]] TextureResolveResult ResolveTexturePath(const aiMaterial& material, aiTextureType textureType, const std::filesystem::path& sourcePath)
		{
			TextureResolveResult result;
			aiString texturePath;
			if (material.GetTexture(textureType, 0, &texturePath) != aiReturn_SUCCESS)
			{
				return result;
			}

			result.RawPath = texturePath.C_Str();
			if (result.RawPath.empty())
			{
				return result;
			}

			if (result.RawPath[0] == '*')
			{
				result.IsEmbeddedReference = true;
				return result;
			}

			const std::filesystem::path importedPath(result.RawPath);
			const std::filesystem::path modelDirectory = sourcePath.parent_path();
			if (importedPath.is_absolute())
			{
				AddCandidateWithPreferredVariants(result.Candidates, importedPath);
			}
			else
			{
				AddCandidateWithPreferredVariants(result.Candidates, modelDirectory / importedPath);
				AddCandidateWithPreferredVariants(result.Candidates, modelDirectory / importedPath.filename());
				AddCandidateWithPreferredVariants(result.Candidates, modelDirectory / "textures" / importedPath);
				AddCandidateWithPreferredVariants(result.Candidates, modelDirectory / "textures" / importedPath.filename());
				AddRecursiveTextureMatches(result.Candidates, modelDirectory, importedPath.filename());
			}

			for (const std::filesystem::path& candidate : result.Candidates)
			{
				if (IsReadableTextureFile(candidate))
				{
					result.SelectedPath = candidate;
					break;
				}
			}

			return result;
		}

		void LogTextureResolveResult(
			uint32_t materialIndex,
			std::string_view materialName,
			aiTextureType textureType,
			const TextureResolveResult& result)
		{
			if (result.RawPath.empty())
			{
				return;
			}

			std::string message = "Material texture resolve - MaterialIndex=";
			message.append(std::to_string(materialIndex));
			message.append(" Name=");
			message.append(materialName.empty() ? "<unnamed>" : std::string(materialName));
			message.append(" Type=");
			message.append(TextureTypeName(textureType));
			message.append(" RawPath=");
			message.append(result.RawPath);
			message.append(" SelectedPath=");
			if (!result.SelectedPath.empty())
			{
				message.append(result.SelectedPath.string());
			}
			else if (result.IsEmbeddedReference)
			{
				message.append("<embedded>");
			}
			else
			{
				message.append("<unresolved>");
			}

			message.append(" Candidates=[");
			for (size_t i = 0; i < result.Candidates.size(); ++i)
			{
				if (i > 0)
				{
					message.append("; ");
				}
				message.append(result.Candidates[i].string());
			}
			message.push_back(']');
			LogAssimpMessage(message);
		}

		[[nodiscard]] LoadedTextureImage LoadEmbeddedTextureImage(const aiScene& scene, const aiMaterial& material, aiTextureType textureType)
		{
			LoadedTextureImage image = {};
			aiString texturePath;
			if (material.GetTexture(textureType, 0, &texturePath) != aiReturn_SUCCESS || texturePath.length == 0 || texturePath.C_Str()[0] != '*')
			{
				return image;
			}

			const aiTexture* embeddedTexture = scene.GetEmbeddedTexture(texturePath.C_Str());
			if (!embeddedTexture)
			{
				return image;
			}

			if (embeddedTexture->mHeight == 0)
			{
				int width = 0;
				int height = 0;
				int channels = 0;
				const auto* encodedBytes = reinterpret_cast<const stbi_uc*>(embeddedTexture->pcData);
				stbi_uc* pixels = stbi_load_from_memory(encodedBytes, static_cast<int>(embeddedTexture->mWidth), &width, &height, &channels, STBI_rgb_alpha);
				if (!pixels)
				{
					return image;
				}

				image.Width = width;
				image.Height = height;
				image.Channels = 4;
				image.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
				stbi_image_free(pixels);
				return image;
			}

			image.Width = static_cast<int>(embeddedTexture->mWidth);
			image.Height = static_cast<int>(embeddedTexture->mHeight);
			image.Channels = 4;
			image.Pixels.resize(static_cast<size_t>(image.Width) * static_cast<size_t>(image.Height) * 4);
			for (int y = 0; y < image.Height; ++y)
			{
				for (int x = 0; x < image.Width; ++x)
				{
					const aiTexel& texel = embeddedTexture->pcData[static_cast<size_t>(y) * static_cast<size_t>(image.Width) + static_cast<size_t>(x)];
					const size_t pixelIndex = (static_cast<size_t>(y) * static_cast<size_t>(image.Width) + static_cast<size_t>(x)) * 4;
					image.Pixels[pixelIndex + 0] = texel.r;
					image.Pixels[pixelIndex + 1] = texel.g;
					image.Pixels[pixelIndex + 2] = texel.b;
					image.Pixels[pixelIndex + 3] = texel.a;
				}
			}

			return image;
		}

		void StoreEmbeddedDiffuseTexture(StaticMeshMaterial& material, LoadedTextureImage&& image)
		{
			material.EmbeddedDiffuseTexturePixels = std::move(image.Pixels);
			material.EmbeddedDiffuseTextureWidth = image.Width;
			material.EmbeddedDiffuseTextureHeight = image.Height;
		}

		[[nodiscard]] bool TryAssignDiffuseTexture(
			StaticMeshMaterial& material,
			const aiScene& scene,
			const aiMaterial& aiMaterial,
			uint32_t materialIndex,
			aiTextureType textureType,
			const std::filesystem::path& sourcePath)
		{
			const TextureResolveResult result = ResolveTexturePath(aiMaterial, textureType, sourcePath);
			LogTextureResolveResult(materialIndex, material.Name, textureType, result);

			if (!result.SelectedPath.empty())
			{
				material.DiffuseTexturePath = result.SelectedPath;
				return true;
			}

			LoadedTextureImage embeddedTexture = LoadEmbeddedTextureImage(scene, aiMaterial, textureType);
			if (embeddedTexture.IsValid())
			{
				StoreEmbeddedDiffuseTexture(material, std::move(embeddedTexture));

				std::string message = "Material embedded diffuse selected - MaterialIndex=";
				message.append(std::to_string(materialIndex));
				message.append(" Name=");
				message.append(material.Name.empty() ? "<unnamed>" : material.Name);
				message.append(" Type=");
				message.append(TextureTypeName(textureType));
				LogAssimpMessage(message);
				return true;
			}

			if (result.IsEmbeddedReference)
			{
				std::string message = "Material embedded diffuse missing - MaterialIndex=";
				message.append(std::to_string(materialIndex));
				message.append(" Name=");
				message.append(material.Name.empty() ? "<unnamed>" : material.Name);
				message.append(" Type=");
				message.append(TextureTypeName(textureType));
				message.append(" RawPath=");
				message.append(result.RawPath);
				LogAssimpMessage(message);
			}

			return false;
		}

		[[nodiscard]] std::filesystem::path ResolveMaterialTexturePath(
			const aiMaterial& aiMaterial,
			uint32_t materialIndex,
			std::string_view materialName,
			aiTextureType textureType,
			const std::filesystem::path& sourcePath)
		{
			const TextureResolveResult result = ResolveTexturePath(aiMaterial, textureType, sourcePath);
			LogTextureResolveResult(materialIndex, materialName, textureType, result);
			return result.SelectedPath;
		}

		[[nodiscard]] StaticMeshMaterial BuildMaterial(
			const aiScene& scene,
			uint32_t materialIndex,
			const std::filesystem::path& sourcePath)
		{
			StaticMeshMaterial material = {};
			const aiMaterial* aiMaterialPtr = scene.mMaterials[materialIndex];
			if (!aiMaterialPtr)
			{
				return material;
			}

			aiString materialName;
			if (aiMaterialPtr->Get(AI_MATKEY_NAME, materialName) == aiReturn_SUCCESS)
			{
				material.Name = materialName.C_Str();
			}

			if (!TryAssignDiffuseTexture(material, scene, *aiMaterialPtr, materialIndex, aiTextureType_BASE_COLOR, sourcePath))
			{
				(void)TryAssignDiffuseTexture(material, scene, *aiMaterialPtr, materialIndex, aiTextureType_DIFFUSE, sourcePath);
			}

			material.NormalTexturePath = ResolveMaterialTexturePath(*aiMaterialPtr, materialIndex, material.Name, aiTextureType_NORMAL_CAMERA, sourcePath);
			if (material.NormalTexturePath.empty())
			{
				material.NormalTexturePath = ResolveMaterialTexturePath(*aiMaterialPtr, materialIndex, material.Name, aiTextureType_NORMALS, sourcePath);
			}

			material.MetallicRoughnessTexturePath = ResolveMaterialTexturePath(*aiMaterialPtr, materialIndex, material.Name, aiTextureType_UNKNOWN, sourcePath);
			if (!material.NormalTexturePath.empty())
			{
				std::string message = "Material normal texture discovered but renderer does not sample it yet - MaterialIndex=";
				message.append(std::to_string(materialIndex));
				message.append(" Path=");
				message.append(material.NormalTexturePath.string());
				LogAssimpMessage(message);
			}
			if (!material.MetallicRoughnessTexturePath.empty())
			{
				std::string message = "Material metallic/roughness texture discovered but renderer does not sample it yet - MaterialIndex=";
				message.append(std::to_string(materialIndex));
				message.append(" Path=");
				message.append(material.MetallicRoughnessTexturePath.string());
				LogAssimpMessage(message);
			}

			return material;
		}

		[[nodiscard]] bool HasDiffuseTextureSource(const StaticMeshMaterial& material) noexcept
		{
			return !material.DiffuseTexturePath.empty()
				|| (!material.EmbeddedDiffuseTexturePixels.empty()
					&& material.EmbeddedDiffuseTextureWidth > 0
					&& material.EmbeddedDiffuseTextureHeight > 0);
		}

		[[nodiscard]] LoadedTextureImage LoadTextureImage(const std::filesystem::path& texturePath)
		{
			LoadedTextureImage image = {};
			if (texturePath.empty() || !std::filesystem::exists(texturePath))
			{
				return image;
			}

			int width = 0;
			int height = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load(texturePath.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels)
			{
				std::string errorMessage = "Failed to load texture image '";
				errorMessage.append(texturePath.string());
				errorMessage.append("'.");
				LogAssimpMessage(errorMessage);
				return image;
			}

			image.Width = width;
			image.Height = height;
			image.Channels = 4;
			image.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			stbi_image_free(pixels);
			return image;
		}

		[[nodiscard]] DirectX::XMFLOAT4 SampleTextureColor(const LoadedTextureImage& image, const DirectX::XMFLOAT2& uv)
		{
			if (!image.IsValid())
			{
				return { 1.0f, 1.0f, 1.0f, 1.0f };
			}

			const float wrappedU = uv.x - std::floor(uv.x);
			const float wrappedV = uv.y - std::floor(uv.y);
			const int texelX = (std::min)(static_cast<int>(wrappedU * static_cast<float>(image.Width)), image.Width - 1);
			const int texelY = (std::min)(static_cast<int>(wrappedV * static_cast<float>(image.Height)), image.Height - 1);
			const size_t pixelIndex = (static_cast<size_t>(texelY) * static_cast<size_t>(image.Width) + static_cast<size_t>(texelX)) * 4;

			return {
				image.Pixels[pixelIndex + 0] / 255.0f,
				image.Pixels[pixelIndex + 1] / 255.0f,
				image.Pixels[pixelIndex + 2] / 255.0f,
				image.Pixels[pixelIndex + 3] / 255.0f };
		}

		void NormalizeBoneWeights(StaticMeshVertex& vertex)
		{
			const float totalWeight = vertex.BoneWeights[0] + vertex.BoneWeights[1] + vertex.BoneWeights[2] + vertex.BoneWeights[3];
			if (totalWeight <= 0.0f)
			{
				return;
			}

			for (float& weight : vertex.BoneWeights)
			{
				weight /= totalWeight;
			}
		}

		void AddBoneInfluence(StaticMeshVertex& vertex, uint32_t boneIndex, float weight)
		{
			for (size_t influenceIndex = 0; influenceIndex < vertex.BoneWeights.size(); ++influenceIndex)
			{
				if (vertex.BoneWeights[influenceIndex] == 0.0f)
				{
					vertex.BoneIndices[influenceIndex] = boneIndex;
					vertex.BoneWeights[influenceIndex] = weight;
					return;
				}
			}

			auto smallestWeightIt = std::min_element(vertex.BoneWeights.begin(), vertex.BoneWeights.end());
			if (smallestWeightIt != vertex.BoneWeights.end() && weight > *smallestWeightIt)
			{
				const size_t replaceIndex = static_cast<size_t>(std::distance(vertex.BoneWeights.begin(), smallestWeightIt));
				vertex.BoneIndices[replaceIndex] = boneIndex;
				vertex.BoneWeights[replaceIndex] = weight;
			}
		}

		uint32_t BuildNodeHierarchy(const aiNode& node, int32_t parentIndex, StaticMeshAsset& meshAsset, std::vector<uint32_t>& meshNodeIndices)
		{
			const uint32_t nodeIndex = static_cast<uint32_t>(meshAsset.Nodes.size());
			SkeletonNode runtimeNode = {};
			runtimeNode.Name = node.mName.C_Str();
			runtimeNode.ParentIndex = parentIndex;
			runtimeNode.LocalBindTransform = ToMatrix(node.mTransformation);
			runtimeNode.LocalBindPose = Math::Transform::FromMatrix(runtimeNode.LocalBindTransform);
			meshAsset.NodeIndices[runtimeNode.Name] = nodeIndex;
			meshAsset.Nodes.push_back(std::move(runtimeNode));

			for (uint32_t meshIndex = 0; meshIndex < node.mNumMeshes; ++meshIndex)
			{
				meshNodeIndices[node.mMeshes[meshIndex]] = nodeIndex;
			}

			for (uint32_t childIndex = 0; childIndex < node.mNumChildren; ++childIndex)
			{
				const uint32_t childNodeIndex = BuildNodeHierarchy(*node.mChildren[childIndex], static_cast<int32_t>(nodeIndex), meshAsset, meshNodeIndices);
				meshAsset.Nodes[nodeIndex].Children.push_back(childNodeIndex);
			}

			return nodeIndex;
		}

		uint32_t GetOrCreateBoneIndex(const aiBone& bone, StaticMeshAsset& meshAsset)
		{
			const std::string boneName = bone.mName.C_Str();
			auto existingBone = meshAsset.BoneIndices.find(boneName);
			if (existingBone != meshAsset.BoneIndices.end())
			{
				return existingBone->second;
			}

			const uint32_t boneIndex = static_cast<uint32_t>(meshAsset.Bones.size());
			SkeletonBone runtimeBone = {};
			runtimeBone.Name = boneName;
			runtimeBone.OffsetMatrix = ToMatrix(bone.mOffsetMatrix);
			auto nodeIt = meshAsset.NodeIndices.find(boneName);
			runtimeBone.NodeIndex = nodeIt != meshAsset.NodeIndices.end() ? nodeIt->second : 0;
			meshAsset.BoneIndices[boneName] = boneIndex;
			meshAsset.Bones.push_back(std::move(runtimeBone));
			return boneIndex;
		}

		void PopulateAnimationClips(const aiScene& scene, StaticMeshAsset& meshAsset)
		{
			meshAsset.Animations.reserve(scene.mNumAnimations);
			for (uint32_t animationIndex = 0; animationIndex < scene.mNumAnimations; ++animationIndex)
			{
				const aiAnimation* animation = scene.mAnimations[animationIndex];
				if (!animation)
				{
					continue;
				}

				AnimationClip clip = {};
				clip.Name = animation->mName.C_Str();
				if (clip.Name.empty())
				{
					clip.Name = "Animation_" + std::to_string(animationIndex);
				}
				clip.DurationTicks = animation->mDuration;
				clip.TicksPerSecond = animation->mTicksPerSecond > 0.0 ? animation->mTicksPerSecond : 25.0;
				clip.Channels.reserve(animation->mNumChannels);

				for (uint32_t channelIndex = 0; channelIndex < animation->mNumChannels; ++channelIndex)
				{
					const aiNodeAnim* nodeAnimation = animation->mChannels[channelIndex];
					if (!nodeAnimation)
					{
						continue;
					}

					AnimationChannel channel = {};
					channel.NodeName = nodeAnimation->mNodeName.C_Str();
					channel.PositionKeys.reserve(nodeAnimation->mNumPositionKeys);
					channel.RotationKeys.reserve(nodeAnimation->mNumRotationKeys);
					channel.ScalingKeys.reserve(nodeAnimation->mNumScalingKeys);

					for (uint32_t positionKeyIndex = 0; positionKeyIndex < nodeAnimation->mNumPositionKeys; ++positionKeyIndex)
					{
						channel.PositionKeys.push_back({
							nodeAnimation->mPositionKeys[positionKeyIndex].mTime,
							ToFloat3(nodeAnimation->mPositionKeys[positionKeyIndex].mValue) });
					}

					for (uint32_t rotationKeyIndex = 0; rotationKeyIndex < nodeAnimation->mNumRotationKeys; ++rotationKeyIndex)
					{
						channel.RotationKeys.push_back({
							nodeAnimation->mRotationKeys[rotationKeyIndex].mTime,
							ToFloat4(nodeAnimation->mRotationKeys[rotationKeyIndex].mValue) });
					}

					for (uint32_t scalingKeyIndex = 0; scalingKeyIndex < nodeAnimation->mNumScalingKeys; ++scalingKeyIndex)
					{
						channel.ScalingKeys.push_back({
							nodeAnimation->mScalingKeys[scalingKeyIndex].mTime,
							ToFloat3(nodeAnimation->mScalingKeys[scalingKeyIndex].mValue) });
					}

					clip.ChannelIndices[channel.NodeName] = static_cast<uint32_t>(clip.Channels.size());
					clip.Channels.push_back(std::move(channel));
				}

				meshAsset.Animations.push_back(std::move(clip));
			}
		}

		[[nodiscard]] std::unique_ptr<StaticMeshAsset> BuildMeshAsset(const aiScene& scene, std::string_view filePath, bool isAnimated)
		{
			auto meshAsset = std::make_unique<StaticMeshAsset>();
			meshAsset->SourcePath = std::filesystem::path(filePath);
			meshAsset->IsAnimated = isAnimated;
			meshAsset->AnimationCount = scene.mNumAnimations;
			meshAsset->RootInverseTransform = ToMatrix(scene.mRootNode->mTransformation.Inverse());

			std::vector<uint32_t> meshNodeIndices(scene.mNumMeshes, 0);
			BuildNodeHierarchy(*scene.mRootNode, -1, *meshAsset, meshNodeIndices);

			meshAsset->Materials.reserve(scene.mNumMaterials);
			for (uint32_t materialIndex = 0; materialIndex < scene.mNumMaterials; ++materialIndex)
			{
				meshAsset->Materials.push_back(BuildMaterial(scene, materialIndex, meshAsset->SourcePath));
			}

			meshAsset->Submeshes.reserve(scene.mNumMeshes);
			for (uint32_t meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex)
			{
				const aiMesh* mesh = scene.mMeshes[meshIndex];
				if (!mesh || mesh->mNumVertices == 0)
				{
					continue;
				}

				StaticMeshSubmesh submesh = {};
				submesh.VertexOffset = static_cast<uint32_t>(meshAsset->Vertices.size());
				submesh.IndexOffset = static_cast<uint32_t>(meshAsset->Indices.size());
				submesh.MaterialIndex = mesh->mMaterialIndex;
				submesh.Name = mesh->mName.C_Str();
				submesh.NodeIndex = meshIndex < meshNodeIndices.size() ? meshNodeIndices[meshIndex] : 0;

				meshAsset->Vertices.reserve(meshAsset->Vertices.size() + mesh->mNumVertices);
				for (uint32_t vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
				{
					StaticMeshVertex vertex;
					vertex.Position = ToFloat3(mesh->mVertices[vertexIndex]);

					if (mesh->HasNormals())
					{
						vertex.Normal = ToFloat3(mesh->mNormals[vertexIndex]);
					}

					if (mesh->HasTextureCoords(0))
					{
						vertex.TexCoord = {
							mesh->mTextureCoords[0][vertexIndex].x,
							mesh->mTextureCoords[0][vertexIndex].y };
					}

					if (mesh->HasTangentsAndBitangents())
					{
						vertex.Tangent = ToFloat3(mesh->mTangents[vertexIndex]);
					}

					if (mesh->HasVertexColors(0))
					{
						const aiColor4D& color = mesh->mColors[0][vertexIndex];
						vertex.Color = { color.r, color.g, color.b, color.a };
					}
					else if (submesh.MaterialIndex < meshAsset->Materials.size() && HasDiffuseTextureSource(meshAsset->Materials[submesh.MaterialIndex]))
					{
						vertex.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
					}
					else
					{
						vertex.Color = DirectX::XMFLOAT4(
							vertex.Normal.x * 0.5f + 0.5f,
							vertex.Normal.y * 0.5f + 0.5f,
							vertex.Normal.z * 0.5f + 0.5f,
							1.0f);
					}

					meshAsset->Vertices.push_back(vertex);
				}

				if (isAnimated)
				{
					for (uint32_t boneArrayIndex = 0; boneArrayIndex < mesh->mNumBones; ++boneArrayIndex)
					{
						const aiBone* bone = mesh->mBones[boneArrayIndex];
						if (!bone)
						{
							continue;
						}

						const uint32_t boneIndex = GetOrCreateBoneIndex(*bone, *meshAsset);
						for (uint32_t weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex)
						{
							const aiVertexWeight& weight = bone->mWeights[weightIndex];
							AddBoneInfluence(meshAsset->Vertices[submesh.VertexOffset + weight.mVertexId], boneIndex, weight.mWeight);
						}
					}
				}

				for (uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
				{
					const aiFace& face = mesh->mFaces[faceIndex];
					for (uint32_t index = 0; index < face.mNumIndices; ++index)
					{
						meshAsset->Indices.push_back(submesh.VertexOffset + face.mIndices[index]);
					}
				}

				submesh.VertexCount = mesh->mNumVertices;
				submesh.IndexCount = static_cast<uint32_t>(meshAsset->Indices.size()) - submesh.IndexOffset;
				meshAsset->Submeshes.push_back(std::move(submesh));
			}

			if (isAnimated)
			{
				for (auto& vertex : meshAsset->Vertices)
				{
					NormalizeBoneWeights(vertex);
				}
				meshAsset->BoneCount = static_cast<uint32_t>(meshAsset->Bones.size());
				PopulateAnimationClips(scene, *meshAsset);
			}
			else
			{
				meshAsset->BoneCount = 0;
			}

			meshAsset->BindPoseVertices = meshAsset->Vertices;
			return meshAsset;
		}

		void PopulateInspectionSummary(ModelInspectionSummary& summary, const aiScene* scene, std::string_view assimpError)
		{
			summary.AssimpError = assimpError;
			if (!scene)
			{
				return;
			}

			summary.HasScene = true;
			summary.HasRootNode = scene->mRootNode != nullptr;
			summary.IsIncomplete = (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0;
			summary.HasAnimations = scene->HasAnimations();
			summary.MeshCount = scene->mNumMeshes;
			summary.MaterialCount = scene->mNumMaterials;
			summary.AnimationCount = scene->mNumAnimations;

			for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
			{
				const aiMesh* mesh = scene->mMeshes[meshIndex];
				if (!mesh)
				{
					continue;
				}

				summary.VertexCount += mesh->mNumVertices;
				summary.FaceCount += mesh->mNumFaces;

				bool hasTriangleFace = false;
				for (uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
				{
					const aiFace& face = mesh->mFaces[faceIndex];
					summary.IndexCount += face.mNumIndices;
					hasTriangleFace = hasTriangleFace || face.mNumIndices >= 3;
				}

				if (mesh->mNumVertices > 0 && hasTriangleFace)
				{
					++summary.RenderableMeshCount;
				}
			}
		}
	}

	ModelInspectionSummary AssimpModelLoader::InspectModel(std::string_view filePath) const
	{
		ModelInspectionSummary summary = {};
		try
		{
			Assimp::Importer importer;
			const std::string path(filePath);
			const aiScene* scene = importer.ReadFile(
				path.c_str(),
				aiProcess_Triangulate);

			PopulateInspectionSummary(summary, scene, importer.GetErrorString());
		}
		catch (const std::exception& exception)
		{
			summary.AssimpError = exception.what();
		}

		return summary;
	}

	std::unique_ptr<StaticMeshAsset> AssimpModelLoader::LoadStaticMesh(std::string_view filePath) const
	{
		try
		{
			Assimp::Importer importer;
			const aiScene* scene = importer.ReadFile(
				filePath.data(),
				aiProcess_Triangulate |
				aiProcess_JoinIdenticalVertices |
				aiProcess_GenSmoothNormals |
				aiProcess_PreTransformVertices |
				aiProcess_ImproveCacheLocality |
				aiProcess_SortByPType |
				aiProcess_FlipUVs);

			if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
			{
				std::string errorMessage = "Failed to load static mesh '";
				errorMessage.append(filePath);
				errorMessage.append("': ");
				errorMessage.append(importer.GetErrorString());
				LogAssimpMessage(errorMessage);
				return nullptr;
			}

			if (scene->HasAnimations())
			{
				std::string errorMessage = "Rejected animated asset in static loader: '";
				errorMessage.append(filePath);
				errorMessage.push_back('\'');
				LogAssimpMessage(errorMessage);
				return nullptr;
			}

			return BuildMeshAsset(*scene, filePath, false);
		}
		catch (const std::exception& exception)
		{
			std::string errorMessage = "Exception while loading static mesh '";
			errorMessage.append(filePath);
			errorMessage.append("': ");
			errorMessage.append(exception.what());
			LogAssimpMessage(errorMessage);
			return nullptr;
		}
	}

	std::unique_ptr<StaticMeshAsset> AssimpModelLoader::LoadAnimatedMesh(std::string_view filePath) const
	{
		try
		{
			Assimp::Importer importer;
			const aiScene* scene = importer.ReadFile(
				filePath.data(),
				aiProcess_Triangulate |
				aiProcess_JoinIdenticalVertices |
				aiProcess_GenSmoothNormals |
				aiProcess_LimitBoneWeights |
				aiProcess_ImproveCacheLocality |
				aiProcess_SortByPType |
				aiProcess_FlipUVs);

			if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
			{
				std::string errorMessage = "Failed to load animated mesh '";
				errorMessage.append(filePath);
				errorMessage.append("': ");
				errorMessage.append(importer.GetErrorString());
				LogAssimpMessage(errorMessage);
				return nullptr;
			}

			if (!scene->HasAnimations())
			{
				std::string errorMessage = "Rejected static asset in animated loader: '";
				errorMessage.append(filePath);
				errorMessage.push_back('\'');
				LogAssimpMessage(errorMessage);
				return nullptr;
			}

			return BuildMeshAsset(*scene, filePath, true);
		}
		catch (const std::exception& exception)
		{
			std::string errorMessage = "Exception while loading animated mesh '";
			errorMessage.append(filePath);
			errorMessage.append("': ");
			errorMessage.append(exception.what());
			LogAssimpMessage(errorMessage);
			return nullptr;
		}
	}

	bool AssimpModelLoader::HasAnimation(std::string_view filePath) const
	{
		const ModelInspectionSummary inspection = InspectModel(filePath);
		if (!inspection.HasScene || !inspection.HasRootNode || inspection.IsIncomplete)
		{
			std::string errorMessage = "Failed to inspect animation data for '";
			errorMessage.append(filePath);
			errorMessage.append("': ");
			errorMessage.append(inspection.AssimpError);
			LogAssimpMessage(errorMessage);
			return true;
		}

		return inspection.HasAnimations;
	}
}
