#include "ScenePersistenceService.h"

#include "Assets/PrimitiveMeshFactory.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ScenePersistence
{
	namespace
	{
		inline constexpr uint32_t kSceneFileVersion = 1;

		[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path, std::string& errorMessage)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
			{
				errorMessage = "Scene file could not be opened: " + path.string();
				return {};
			}

			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path, std::string_view text, std::string& errorMessage)
		{
			std::error_code errorCode;
			std::filesystem::create_directories(path.parent_path(), errorCode);
			if (errorCode)
			{
				errorMessage = "Scene directory could not be created: " + path.parent_path().string() + " | " + errorCode.message();
				return false;
			}

			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				errorMessage = "Scene file could not be written: " + path.string();
				return false;
			}

			stream.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!stream.good())
			{
				errorMessage = "Scene file write failed: " + path.string();
				return false;
			}
			return true;
		}

		[[nodiscard]] std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code errorCode;
			const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, errorCode);
			return (errorCode ? path : canonicalPath).lexically_normal();
		}

		[[nodiscard]] bool IsRelativeInsideRoot(const std::filesystem::path& relativePath)
		{
			if (relativePath.empty() || relativePath.is_absolute())
			{
				return false;
			}

			for (const auto& part : relativePath)
			{
				if (part == "..")
				{
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::string ToStoredAssetPath(const std::filesystem::path& sourcePath, const Projects::ProjectDescriptor& project)
		{
			if (sourcePath.empty())
			{
				return {};
			}

			const std::filesystem::path assetRoot = NormalizePath(project.RootPath / project.AssetRoot);
			const std::filesystem::path normalizedSource = NormalizePath(sourcePath);
			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(normalizedSource, assetRoot, errorCode);
			if (!errorCode && IsRelativeInsideRoot(relativePath))
			{
				return relativePath.generic_string();
			}

			return normalizedSource.generic_string();
		}

		[[nodiscard]] std::filesystem::path ResolveStoredAssetPath(const std::filesystem::path& storedPath, const Projects::ProjectDescriptor& project)
		{
			if (storedPath.empty() || storedPath.is_absolute())
			{
				return storedPath;
			}

			return NormalizePath(project.RootPath / project.AssetRoot / storedPath);
		}

		[[nodiscard]] std::string ToStoredProjectPath(const std::filesystem::path& sourcePath, const Projects::ProjectDescriptor& project)
		{
			if (sourcePath.empty())
			{
				return {};
			}

			const std::filesystem::path projectRoot = NormalizePath(project.RootPath);
			const std::filesystem::path normalizedSource = NormalizePath(sourcePath);
			std::error_code errorCode;
			const std::filesystem::path relativePath = std::filesystem::relative(normalizedSource, projectRoot, errorCode);
			if (!errorCode && IsRelativeInsideRoot(relativePath))
			{
				return relativePath.generic_string();
			}

			return normalizedSource.generic_string();
		}

		[[nodiscard]] std::filesystem::path ResolveStoredProjectPath(const std::filesystem::path& storedPath, const Projects::ProjectDescriptor& project)
		{
			if (storedPath.empty() || storedPath.is_absolute())
			{
				return storedPath;
			}

			return NormalizePath(project.RootPath / storedPath);
		}

		template <typename Writer>
		void WriteFloat2(Writer& writer, const char* key, const DirectX::XMFLOAT2& value)
		{
			writer.Key(key);
			writer.StartArray();
			writer.Double(value.x);
			writer.Double(value.y);
			writer.EndArray();
		}

		template <typename Writer>
		void WriteFloat3(Writer& writer, const char* key, const DirectX::XMFLOAT3& value)
		{
			writer.Key(key);
			writer.StartArray();
			writer.Double(value.x);
			writer.Double(value.y);
			writer.Double(value.z);
			writer.EndArray();
		}

		template <typename Writer>
		void WriteFloat4(Writer& writer, const char* key, const DirectX::XMFLOAT4& value)
		{
			writer.Key(key);
			writer.StartArray();
			writer.Double(value.x);
			writer.Double(value.y);
			writer.Double(value.z);
			writer.Double(value.w);
			writer.EndArray();
		}

		template <typename Writer>
		void WriteTransform(Writer& writer, const Math::Transform& transform)
		{
			writer.Key("transform");
			writer.StartObject();
			WriteFloat3(writer, "position", transform.Translation);
			WriteFloat4(writer, "rotation", transform.Rotation);
			WriteFloat3(writer, "scale", transform.Scale);
			writer.EndObject();
		}

		template <typename Writer>
		void WriteMaterial(Writer& writer, const Asset::StaticMeshMaterial& material, const Projects::ProjectDescriptor& project)
		{
			writer.StartObject();
			writer.Key("name");
			writer.String(material.Name.c_str());
			writer.Key("shadingModel");
			const std::string shadingModelName(Asset::MaterialShadingModelName(material.ShadingModel));
			writer.String(shadingModelName.c_str());
			WriteFloat4(writer, "baseColor", material.DiffuseColor);
			WriteFloat4(writer, "importedDiffuseTint", material.ImportedDiffuseTint);
			WriteFloat3(writer, "specularColor", material.SpecularColor);
			WriteFloat3(writer, "emissiveColor", material.EmissiveColor);
			writer.Key("metallic");
			writer.Double(material.MetallicFactor);
			writer.Key("roughness");
			writer.Double(material.RoughnessFactor);
			writer.Key("shininess");
			writer.Double(material.Shininess);
			writer.Key("opacity");
			writer.Double(material.Opacity);
			writer.Key("useVertexColor");
			writer.Bool(material.UseVertexColor);
			writer.Key("normalYFlip");
			writer.Bool(material.NormalYFlip);

			writer.Key("textures");
			writer.StartObject();
			for (size_t slotIndex = 0; slotIndex < Asset::kMaterialTextureSlotCount; ++slotIndex)
			{
				const auto slot = static_cast<Asset::MaterialTextureSlot>(slotIndex);
				const std::filesystem::path texturePath = Asset::GetMaterialTexturePath(material, slot);
				if (texturePath.empty())
				{
					continue;
				}

				const std::string storedPath = ToStoredAssetPath(texturePath, project);
				if (storedPath.empty())
				{
					continue;
				}

				const std::string slotKey(Asset::MaterialTextureSlotKey(slot));
				writer.Key(slotKey.c_str());
				writer.String(storedPath.c_str());
			}
			writer.EndObject();
			writer.EndObject();
		}

		[[nodiscard]] bool ReadFloat3(const rapidjson::Value& object, const char* key, DirectX::XMFLOAT3& value)
		{
			if (!object.HasMember(key) || !object[key].IsArray() || object[key].Size() != 3)
			{
				return false;
			}

			const auto& array = object[key];
			if (!array[0].IsNumber() || !array[1].IsNumber() || !array[2].IsNumber())
			{
				return false;
			}

			value = {
				array[0].GetFloat(),
				array[1].GetFloat(),
				array[2].GetFloat()
			};
			return true;
		}

		[[nodiscard]] bool ReadFloat4(const rapidjson::Value& object, const char* key, DirectX::XMFLOAT4& value)
		{
			if (!object.HasMember(key) || !object[key].IsArray() || object[key].Size() != 4)
			{
				return false;
			}

			const auto& array = object[key];
			if (!array[0].IsNumber() || !array[1].IsNumber() || !array[2].IsNumber() || !array[3].IsNumber())
			{
				return false;
			}

			value = {
				array[0].GetFloat(),
				array[1].GetFloat(),
				array[2].GetFloat(),
				array[3].GetFloat()
			};
			return true;
		}

		[[nodiscard]] bool ReadTransform(const rapidjson::Value& entityObject, Math::Transform& transform)
		{
			if (!entityObject.HasMember("transform") || !entityObject["transform"].IsObject())
			{
				return false;
			}

			const auto& transformObject = entityObject["transform"];
			static_cast<void>(ReadFloat3(transformObject, "position", transform.Translation));
			static_cast<void>(ReadFloat4(transformObject, "rotation", transform.Rotation));
			static_cast<void>(ReadFloat3(transformObject, "scale", transform.Scale));
			transform.Rotation = Math::NormalizeQuaternionOrIdentity(transform.Rotation);
			return true;
		}

		[[nodiscard]] std::string StringMemberOrDefault(const rapidjson::Value& object, const char* key, std::string_view fallback);
		[[nodiscard]] float FloatMemberOrDefault(const rapidjson::Value& object, const char* key, float fallback);
		[[nodiscard]] bool BoolMemberOrDefault(const rapidjson::Value& object, const char* key, bool fallback);

		[[nodiscard]] Asset::StaticMeshMaterial ReadMaterial(const rapidjson::Value& materialObject, const Projects::ProjectDescriptor& project)
		{
			Asset::StaticMeshMaterial material;
			material.Name = StringMemberOrDefault(materialObject, "name", "");
			material.ShadingModel = Asset::MaterialShadingModelFromName(StringMemberOrDefault(materialObject, "shadingModel", "Phong"));
			static_cast<void>(ReadFloat4(materialObject, "baseColor", material.DiffuseColor));
			material.ImportedDiffuseTint = material.DiffuseColor;
			static_cast<void>(ReadFloat4(materialObject, "importedDiffuseTint", material.ImportedDiffuseTint));
			static_cast<void>(ReadFloat3(materialObject, "specularColor", material.SpecularColor));
			static_cast<void>(ReadFloat3(materialObject, "emissiveColor", material.EmissiveColor));
			material.MetallicFactor = FloatMemberOrDefault(materialObject, "metallic", material.MetallicFactor);
			material.RoughnessFactor = FloatMemberOrDefault(materialObject, "roughness", material.RoughnessFactor);
			material.Shininess = FloatMemberOrDefault(materialObject, "shininess", material.Shininess);
			material.Opacity = FloatMemberOrDefault(materialObject, "opacity", material.Opacity);
			material.UseVertexColor = BoolMemberOrDefault(materialObject, "useVertexColor", material.UseVertexColor);
			material.NormalYFlip = BoolMemberOrDefault(materialObject, "normalYFlip", material.NormalYFlip);

			if (materialObject.HasMember("textures") && materialObject["textures"].IsObject())
			{
				const auto& texturesObject = materialObject["textures"];
				for (auto textureIt = texturesObject.MemberBegin(); textureIt != texturesObject.MemberEnd(); ++textureIt)
				{
					if (!textureIt->name.IsString() || !textureIt->value.IsString())
					{
						continue;
					}

					const Asset::MaterialTextureSlot slot = Asset::MaterialTextureSlotFromKey(textureIt->name.GetString());
					if (slot == Asset::MaterialTextureSlot::Count)
					{
						continue;
					}

					const std::filesystem::path resolvedPath = ResolveStoredAssetPath(textureIt->value.GetString(), project);
					Asset::SetMaterialTexturePath(material, slot, resolvedPath, true);
				}
			}

			return material;
		}

		[[nodiscard]] bool ReadFloat2(const rapidjson::Value& object, const char* key, DirectX::XMFLOAT2& value)
		{
			if (!object.HasMember(key) || !object[key].IsArray() || object[key].Size() != 2)
			{
				return false;
			}

			const auto& array = object[key];
			if (!array[0].IsNumber() || !array[1].IsNumber())
			{
				return false;
			}

			value = {
				array[0].GetFloat(),
				array[1].GetFloat()
			};
			return true;
		}

		[[nodiscard]] std::string StringMemberOrDefault(const rapidjson::Value& object, const char* key, std::string_view fallback)
		{
			if (!object.HasMember(key) || !object[key].IsString())
			{
				return std::string(fallback);
			}
			return object[key].GetString();
		}

		[[nodiscard]] float FloatMemberOrDefault(const rapidjson::Value& object, const char* key, float fallback)
		{
			if (!object.HasMember(key) || !object[key].IsNumber())
			{
				return fallback;
			}
			return object[key].GetFloat();
		}

		[[nodiscard]] uint32_t UintMemberOrDefault(const rapidjson::Value& object, const char* key, uint32_t fallback)
		{
			if (!object.HasMember(key) || !object[key].IsUint())
			{
				return fallback;
			}
			return object[key].GetUint();
		}

		[[nodiscard]] int IntMemberOrDefault(const rapidjson::Value& object, const char* key, int fallback)
		{
			if (!object.HasMember(key) || !object[key].IsInt())
			{
				return fallback;
			}
			return object[key].GetInt();
		}

		[[nodiscard]] uint64_t Uint64MemberOrDefault(const rapidjson::Value& object, const char* key, uint64_t fallback)
		{
			if (!object.HasMember(key) || !object[key].IsUint64())
			{
				return fallback;
			}
			return object[key].GetUint64();
		}

		[[nodiscard]] bool BoolMemberOrDefault(const rapidjson::Value& object, const char* key, bool fallback)
		{
			if (!object.HasMember(key) || !object[key].IsBool())
			{
				return fallback;
			}
			return object[key].GetBool();
		}
	}

	bool ScenePersistenceService::SaveScene(
		const Scene& scene,
		const SceneRenderState& renderState,
		const Projects::ProjectDescriptor& project,
		const std::filesystem::path& scenePath,
		const DirectX::XMFLOAT3& ambientColor,
		float ambientIntensity,
		float exposure,
		const Rendering::SkyboxSettings& skybox,
		std::string& errorMessage,
		const std::unordered_set<EntityId>* excludedEntities)
	{
		(void)renderState;

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(kSceneFileVersion);
		writer.Key("name");
		writer.String(scenePath.stem().string().c_str());
		writer.Key("lighting");
		writer.StartObject();
		WriteFloat3(writer, "ambientColor", ambientColor);
		writer.Key("ambientIntensity");
		writer.Double(std::clamp(ambientIntensity, 0.0f, 2.0f));
		writer.Key("exposure");
		writer.Double(std::clamp(exposure, 0.05f, 8.0f));
		const Rendering::SkyboxSettings clampedSkybox = Rendering::ClampSkyboxSettings(skybox);
		writer.Key("skybox");
		writer.StartObject();
		writer.Key("enabled");
		writer.Bool(clampedSkybox.Enabled);
		WriteFloat3(writer, "zenithColor", clampedSkybox.ZenithColor);
		WriteFloat3(writer, "horizonColor", clampedSkybox.HorizonColor);
		WriteFloat3(writer, "groundColor", clampedSkybox.GroundColor);
		WriteFloat3(writer, "sunColor", clampedSkybox.SunColor);
		WriteFloat3(writer, "sunDirection", clampedSkybox.SunDirection);
		writer.Key("intensity");
		writer.Double(clampedSkybox.Intensity);
		writer.Key("horizonHeight");
		writer.Double(clampedSkybox.HorizonHeight);
		writer.Key("horizonBlend");
		writer.Double(clampedSkybox.HorizonBlend);
		writer.Key("sunSize");
		writer.Double(clampedSkybox.SunSize);
		writer.Key("sunIntensity");
		writer.Double(clampedSkybox.SunIntensity);
		writer.EndObject();
		writer.EndObject();
		writer.Key("entities");
		writer.StartArray();

		const auto shouldSkipEntity = [excludedEntities](EntityId entityId) noexcept
		{
			return excludedEntities && excludedEntities->contains(entityId);
		};

		std::unordered_map<EntityId, size_t> entityIndexById;
		const std::vector<SceneEntity>& sceneEntities = scene.GetEntities();
		size_t persistedEntityIndex = 0;
		for (const SceneEntity& entity : sceneEntities)
		{
			if (shouldSkipEntity(entity.Id))
			{
				continue;
			}
			entityIndexById.emplace(entity.Id, persistedEntityIndex++);
		}

		for (const SceneEntity& entity : scene.GetEntities())
		{
			if (shouldSkipEntity(entity.Id))
			{
				continue;
			}

			writer.StartObject();
			writer.Key("name");
			const std::string* entityName = scene.GetEntityName(entity.Id);
			writer.String((entityName && !entityName->empty() ? *entityName : std::string("Entity")).c_str());

			if (const EditorStateComponent* editorState = scene.GetEditorStateComponent(entity.Id))
			{
				writer.Key("editorState");
				writer.StartObject();
				writer.Key("visibleInScene");
				writer.Bool(editorState->VisibleInScene);
				writer.Key("pickableInScene");
				writer.Bool(editorState->PickableInScene);
				writer.EndObject();
			}

			if (const SceneHierarchyComponent* hierarchy = scene.GetHierarchyComponent(entity.Id))
			{
				writer.Key("hierarchy");
				writer.StartObject();
				const auto parentIndexIt = entityIndexById.find(hierarchy->Parent);
				if (parentIndexIt != entityIndexById.end())
				{
					writer.Key("parentIndex");
					writer.Uint64(static_cast<uint64_t>(parentIndexIt->second));
				}
				writer.Key("expanded");
				writer.Bool(hierarchy->Expanded);
				writer.EndObject();
			}

			if (const TransformComponent* transform = scene.GetTransformComponent(entity.Id))
			{
				WriteTransform(writer, transform->LocalTransform);
			}

			if (const MeshComponent* mesh = scene.GetMeshComponent(entity.Id))
			{
				writer.Key("mesh");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsMeshEnabled(entity.Id));
				if (mesh->Asset && !mesh->Asset->SourcePath.empty())
				{
					const std::string assetPath = ToStoredAssetPath(mesh->Asset->SourcePath, project);
					if (!assetPath.empty())
					{
						writer.Key("assetPath");
						writer.String(assetPath.c_str());
					}
				}
				else if (mesh->Asset && mesh->Asset->PrimitiveKind != Asset::PrimitiveMeshKind::None)
				{
					const std::string primitiveName(Asset::PrimitiveMeshKindToString(mesh->Asset->PrimitiveKind));
					writer.Key("primitive");
					writer.String(primitiveName.c_str());
				}
				if (mesh->Asset && !mesh->Asset->Materials.empty())
				{
					writer.Key("materials");
					writer.StartArray();
					for (const Asset::StaticMeshMaterial& material : mesh->Asset->Materials)
					{
						WriteMaterial(writer, material, project);
					}
					writer.EndArray();
				}
				writer.EndObject();
			}

			if (const CameraComponent* camera = scene.GetCameraComponent(entity.Id))
			{
				writer.Key("camera");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsCameraEnabled(entity.Id));
				writer.Key("fovY");
				writer.Double(camera->FovY);
				writer.Key("nearZ");
				writer.Double(camera->NearZ);
				writer.Key("farZ");
				writer.Double(camera->FarZ);
				writer.Key("isGameCamera");
				writer.Bool(camera->IsGameCamera);
				writer.EndObject();
			}

			if (const LightComponent* light = scene.GetLightComponent(entity.Id))
			{
				writer.Key("light");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsLightEnabled(entity.Id));
				writer.Key("type");
				writer.Uint(static_cast<uint32_t>(light->Type));
				WriteFloat3(writer, "color", light->Color);
				writer.Key("intensity");
				writer.Double(light->Intensity);
				writer.Key("range");
				writer.Double(light->Range);
				writer.Key("spotAngle");
				writer.Double(light->SpotAngle);
				writer.Key("emitsLight");
				writer.Bool(light->Enabled);
				writer.Key("castShadows");
				writer.Bool(light->CastShadows);
				writer.Key("shadowBias");
				writer.Double(light->ShadowBias);
				writer.Key("shadowNormalBias");
				writer.Double(light->ShadowNormalBias);
				writer.Key("shadowStrength");
				writer.Double(light->ShadowStrength);
				writer.EndObject();
			}

			if (const AnimatorComponent* animator = scene.GetAnimatorComponent(entity.Id))
			{
				writer.Key("animator");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsAnimatorEnabled(entity.Id));
				writer.Key("currentClipIndex");
				writer.Uint(animator->CurrentClipIndex);
				writer.Key("timeSeconds");
				writer.Double(animator->TimeSeconds);
				writer.Key("speed");
				writer.Double(animator->Speed);
				writer.Key("playing");
				writer.Bool(animator->Playing);
				writer.Key("loop");
				writer.Bool(animator->Loop);
				writer.EndObject();
			}

			if (const RigidBodyComponent* rigidBody = scene.GetRigidBodyComponent(entity.Id))
			{
				writer.Key("rigidBody");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsRigidBodyEnabled(entity.Id));
				writer.Key("type");
				writer.String(Physics::ToString(rigidBody->Type).data());
				writer.Key("mass");
				writer.Double(rigidBody->Mass);
				writer.Key("linearDamping");
				writer.Double(rigidBody->LinearDamping);
				writer.Key("angularDamping");
				writer.Double(rigidBody->AngularDamping);
				writer.Key("useGravity");
				writer.Bool(rigidBody->UseGravity);
				WriteFloat3(writer, "linearVelocity", rigidBody->LinearVelocity);
				WriteFloat3(writer, "angularVelocity", rigidBody->AngularVelocity);
				writer.EndObject();
			}

			if (const ColliderComponent* collider = scene.GetColliderComponent(entity.Id))
			{
				writer.Key("collider");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsColliderEnabled(entity.Id));
				writer.Key("shape");
				writer.String(Physics::ToString(collider->Shape).data());
				WriteFloat3(writer, "size", collider->Size);
				writer.Key("radius");
				writer.Double(collider->Radius);
				writer.Key("height");
				writer.Double(collider->Height);
				WriteFloat3(writer, "offset", collider->Offset);
				writer.Key("isTrigger");
				writer.Bool(collider->IsTrigger);
				writer.EndObject();
			}

			if (const PhysicsMaterialComponent* physicsMaterial = scene.GetPhysicsMaterialComponent(entity.Id))
			{
				writer.Key("physicsMaterial");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsPhysicsMaterialEnabled(entity.Id));
				writer.Key("staticFriction");
				writer.Double(physicsMaterial->StaticFriction);
				writer.Key("dynamicFriction");
				writer.Double(physicsMaterial->DynamicFriction);
				writer.Key("restitution");
				writer.Double(physicsMaterial->Restitution);
				writer.EndObject();
			}

			if (const PrefabInstanceComponent* prefab = scene.GetPrefabInstanceComponent(entity.Id))
			{
				writer.Key("prefabInstance");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<PrefabInstanceComponent>(entity.Id));
				writer.Key("prefabPath");
				writer.String(ToStoredProjectPath(prefab->PrefabPath, project).c_str());
				writer.Key("sourceName");
				writer.String(prefab->SourceName.c_str());
				writer.Key("trackPrefabOverrides");
				writer.Bool(prefab->TrackPrefabOverrides);
				writer.EndObject();
			}

			if (const SceneReferenceComponent* sceneReference = scene.GetSceneReferenceComponent(entity.Id))
			{
				writer.Key("sceneReference");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<SceneReferenceComponent>(entity.Id));
				writer.Key("scenePath");
				writer.String(ToStoredProjectPath(sceneReference->ScenePath, project).c_str());
				writer.Key("loadAdditively");
				writer.Bool(sceneReference->LoadAdditively);
				writer.Key("autoLoad");
				writer.Bool(sceneReference->AutoLoad);
				writer.EndObject();
			}

			if (const ScriptComponent* script = scene.GetScriptComponent(entity.Id))
			{
				writer.Key("script");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<ScriptComponent>(entity.Id));
				writer.Key("scriptPath");
				writer.String(ToStoredProjectPath(script->ScriptPath, project).c_str());
				writer.Key("className");
				writer.String(script->ClassName.c_str());
				writer.Key("language");
				writer.Uint(static_cast<uint32_t>(script->Language));
				writer.Key("runInEditor");
				writer.Bool(script->RunInEditor);
				writer.EndObject();
			}

			if (const Sprite2DComponent* sprite = scene.GetSprite2DComponent(entity.Id))
			{
				writer.Key("sprite2D");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<Sprite2DComponent>(entity.Id));
				writer.Key("texturePath");
				writer.String(ToStoredAssetPath(sprite->TexturePath, project).c_str());
				WriteFloat4(writer, "color", sprite->Color);
				WriteFloat2(writer, "size", sprite->Size);
				WriteFloat2(writer, "pivot", sprite->Pivot);
				writer.Key("sortingLayer");
				writer.Int(sprite->SortingLayer);
				writer.Key("orderInLayer");
				writer.Int(sprite->OrderInLayer);
				writer.EndObject();
			}

			if (const UiElementComponent* ui = scene.GetUiElementComponent(entity.Id))
			{
				writer.Key("uiElement");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<UiElementComponent>(entity.Id));
				writer.Key("kind");
				writer.Uint(static_cast<uint32_t>(ui->Kind));
				writer.Key("text");
				writer.String(ui->Text.c_str());
				WriteFloat2(writer, "anchorMin", ui->AnchorMin);
				WriteFloat2(writer, "anchorMax", ui->AnchorMax);
				WriteFloat2(writer, "position", ui->Position);
				WriteFloat2(writer, "size", ui->Size);
				WriteFloat4(writer, "color", ui->Color);
				writer.EndObject();
			}

			if (const AudioSourceComponent* audio = scene.GetAudioSourceComponent(entity.Id))
			{
				writer.Key("audioSource");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<AudioSourceComponent>(entity.Id));
				writer.Key("clipPath");
				writer.String(ToStoredAssetPath(audio->ClipPath, project).c_str());
				writer.Key("volume");
				writer.Double(audio->Volume);
				writer.Key("pitch");
				writer.Double(audio->Pitch);
				writer.Key("loop");
				writer.Bool(audio->Loop);
				writer.Key("playOnStart");
				writer.Bool(audio->PlayOnStart);
				writer.Key("spatialize");
				writer.Bool(audio->Spatialize);
				writer.Key("minDistance");
				writer.Double(audio->MinDistance);
				writer.Key("maxDistance");
				writer.Double(audio->MaxDistance);
				writer.EndObject();
			}

			if (const NavigationAgentComponent* navigation = scene.GetNavigationAgentComponent(entity.Id))
			{
				writer.Key("navigationAgent");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<NavigationAgentComponent>(entity.Id));
				writer.Key("radius");
				writer.Double(navigation->Radius);
				writer.Key("height");
				writer.Double(navigation->Height);
				writer.Key("speed");
				writer.Double(navigation->Speed);
				writer.Key("acceleration");
				writer.Double(navigation->Acceleration);
				WriteFloat3(writer, "target", navigation->Target);
				writer.Key("hasTarget");
				writer.Bool(navigation->HasTarget);
				writer.EndObject();
			}

			if (const NetworkIdentityComponent* network = scene.GetNetworkIdentityComponent(entity.Id))
			{
				writer.Key("networkIdentity");
				writer.StartObject();
				writer.Key("enabled");
				writer.Bool(scene.IsComponentEnabled<NetworkIdentityComponent>(entity.Id));
				writer.Key("networkId");
				writer.Uint64(network->NetworkId);
				writer.Key("prefabKey");
				writer.String(network->PrefabKey.c_str());
				writer.Key("replicateTransform");
				writer.Bool(network->ReplicateTransform);
				writer.Key("serverAuthoritative");
				writer.Bool(network->ServerAuthoritative);
				writer.EndObject();
			}

			writer.EndObject();
		}

		writer.EndArray();
		writer.EndObject();

		return WriteTextFile(scenePath, std::string_view(buffer.GetString(), buffer.GetSize()), errorMessage);
	}

	LoadSceneResult ScenePersistenceService::LoadScene(
		const std::filesystem::path& scenePath,
		const Projects::ProjectDescriptor& project)
	{
		LoadSceneResult result;
		std::string errorMessage;
		const std::string text = ReadTextFile(scenePath, errorMessage);
		if (!errorMessage.empty())
		{
			result.ErrorMessage = errorMessage;
			return result;
		}

		rapidjson::Document document;
		document.Parse(text.c_str(), text.size());
		if (document.HasParseError() || !document.IsObject())
		{
			result.ErrorMessage = "Scene JSON parse failed: ";
			result.ErrorMessage.append(rapidjson::GetParseError_En(document.GetParseError()));
			return result;
		}

		result.Name = StringMemberOrDefault(document, "name", scenePath.stem().string());
		if (document.HasMember("lighting") && document["lighting"].IsObject())
		{
			const auto& lightingObject = document["lighting"];
			static_cast<void>(ReadFloat3(lightingObject, "ambientColor", result.AmbientColor));
			result.AmbientColor.x = std::clamp(result.AmbientColor.x, 0.0f, 4.0f);
			result.AmbientColor.y = std::clamp(result.AmbientColor.y, 0.0f, 4.0f);
			result.AmbientColor.z = std::clamp(result.AmbientColor.z, 0.0f, 4.0f);
			result.AmbientIntensity = std::clamp(
				FloatMemberOrDefault(lightingObject, "ambientIntensity", result.AmbientIntensity),
				0.0f,
				2.0f);
			result.Exposure = std::clamp(
				FloatMemberOrDefault(lightingObject, "exposure", result.Exposure),
				0.05f,
				8.0f);
			if (lightingObject.HasMember("skybox") && lightingObject["skybox"].IsObject())
			{
				const auto& skyboxObject = lightingObject["skybox"];
				Rendering::SkyboxSettings skybox = result.Skybox;
				skybox.Enabled = BoolMemberOrDefault(skyboxObject, "enabled", skybox.Enabled);
				static_cast<void>(ReadFloat3(skyboxObject, "zenithColor", skybox.ZenithColor));
				static_cast<void>(ReadFloat3(skyboxObject, "horizonColor", skybox.HorizonColor));
				static_cast<void>(ReadFloat3(skyboxObject, "groundColor", skybox.GroundColor));
				static_cast<void>(ReadFloat3(skyboxObject, "sunColor", skybox.SunColor));
				static_cast<void>(ReadFloat3(skyboxObject, "sunDirection", skybox.SunDirection));
				skybox.Intensity = FloatMemberOrDefault(skyboxObject, "intensity", skybox.Intensity);
				skybox.HorizonHeight = FloatMemberOrDefault(skyboxObject, "horizonHeight", skybox.HorizonHeight);
				skybox.HorizonBlend = FloatMemberOrDefault(skyboxObject, "horizonBlend", skybox.HorizonBlend);
				skybox.SunSize = FloatMemberOrDefault(skyboxObject, "sunSize", skybox.SunSize);
				skybox.SunIntensity = FloatMemberOrDefault(skyboxObject, "sunIntensity", skybox.SunIntensity);
				result.Skybox = Rendering::ClampSkyboxSettings(skybox);
			}
		}
		if (!document.HasMember("entities") || !document["entities"].IsArray())
		{
			result.Success = true;
			return result;
		}

		for (const auto& entityObject : document["entities"].GetArray())
		{
			if (!entityObject.IsObject())
			{
				continue;
			}

			LoadedSceneEntity entity;
			entity.Name = StringMemberOrDefault(entityObject, "name", "Entity");
			if (entityObject.HasMember("editorState") && entityObject["editorState"].IsObject())
			{
				const auto& editorStateObject = entityObject["editorState"];
				entity.HasEditorState = true;
				entity.EditorState.VisibleInScene = BoolMemberOrDefault(editorStateObject, "visibleInScene", entity.EditorState.VisibleInScene);
				entity.EditorState.PickableInScene = BoolMemberOrDefault(editorStateObject, "pickableInScene", entity.EditorState.PickableInScene);
			}
			if (entityObject.HasMember("hierarchy") && entityObject["hierarchy"].IsObject())
			{
				const auto& hierarchyObject = entityObject["hierarchy"];
				entity.HasHierarchy = true;
				entity.ParentIndex = static_cast<size_t>(Uint64MemberOrDefault(hierarchyObject, "parentIndex", static_cast<uint64_t>(entity.ParentIndex)));
				entity.HierarchyExpanded = BoolMemberOrDefault(hierarchyObject, "expanded", entity.HierarchyExpanded);
			}
			entity.HasTransform = ReadTransform(entityObject, entity.Transform);

			if (entityObject.HasMember("mesh") && entityObject["mesh"].IsObject())
			{
				const auto& meshObject = entityObject["mesh"];
				entity.HasMesh = true;
				entity.MeshEnabled = BoolMemberOrDefault(meshObject, "enabled", entity.MeshEnabled);
				const std::string primitiveName = StringMemberOrDefault(meshObject, "primitive", "");
				entity.PrimitiveKind = Asset::PrimitiveMeshKindFromString(primitiveName);

				const std::string assetPath = StringMemberOrDefault(meshObject, "assetPath", "");
				if (!assetPath.empty())
				{
					entity.MeshAssetPath = ResolveStoredAssetPath(std::filesystem::path(assetPath), project);
				}

				if (meshObject.HasMember("materials") && meshObject["materials"].IsArray())
				{
					for (const auto& materialObject : meshObject["materials"].GetArray())
					{
						if (materialObject.IsObject())
						{
							entity.MaterialOverrides.push_back(ReadMaterial(materialObject, project));
						}
					}
				}
			}

			if (entityObject.HasMember("camera") && entityObject["camera"].IsObject())
			{
				const auto& cameraObject = entityObject["camera"];
				entity.HasCamera = true;
				entity.CameraEnabled = BoolMemberOrDefault(cameraObject, "enabled", entity.CameraEnabled);
				entity.Camera.FovY = FloatMemberOrDefault(cameraObject, "fovY", entity.Camera.FovY);
				entity.Camera.NearZ = FloatMemberOrDefault(cameraObject, "nearZ", entity.Camera.NearZ);
				entity.Camera.FarZ = FloatMemberOrDefault(cameraObject, "farZ", entity.Camera.FarZ);
				entity.Camera.IsGameCamera = BoolMemberOrDefault(cameraObject, "isGameCamera", entity.Camera.IsGameCamera);
			}

			if (entityObject.HasMember("light") && entityObject["light"].IsObject())
			{
				const auto& lightObject = entityObject["light"];
				entity.HasLight = true;
				entity.LightEnabled = BoolMemberOrDefault(lightObject, "enabled", entity.LightEnabled);
				entity.Light.Type = static_cast<LightType>((std::min)(UintMemberOrDefault(lightObject, "type", 0), 2u));
				static_cast<void>(ReadFloat3(lightObject, "color", entity.Light.Color));
				entity.Light.Intensity = FloatMemberOrDefault(lightObject, "intensity", entity.Light.Intensity);
				entity.Light.Range = FloatMemberOrDefault(lightObject, "range", entity.Light.Range);
				entity.Light.SpotAngle = FloatMemberOrDefault(lightObject, "spotAngle", entity.Light.SpotAngle);
				entity.Light.Enabled = BoolMemberOrDefault(
					lightObject,
					"emitsLight",
					BoolMemberOrDefault(lightObject, "enabled", entity.Light.Enabled));
				entity.Light.CastShadows = BoolMemberOrDefault(lightObject, "castShadows", entity.Light.CastShadows);
				entity.Light.ShadowBias = FloatMemberOrDefault(lightObject, "shadowBias", entity.Light.ShadowBias);
				entity.Light.ShadowNormalBias = FloatMemberOrDefault(lightObject, "shadowNormalBias", entity.Light.ShadowNormalBias);
				entity.Light.ShadowStrength = FloatMemberOrDefault(lightObject, "shadowStrength", entity.Light.ShadowStrength);
			}

			if (entityObject.HasMember("animator") && entityObject["animator"].IsObject())
			{
				const auto& animatorObject = entityObject["animator"];
				entity.HasAnimator = true;
				entity.AnimatorEnabled = BoolMemberOrDefault(animatorObject, "enabled", entity.AnimatorEnabled);
				entity.Animator.CurrentClipIndex = UintMemberOrDefault(animatorObject, "currentClipIndex", entity.Animator.CurrentClipIndex);
				entity.Animator.TimeSeconds = FloatMemberOrDefault(animatorObject, "timeSeconds", entity.Animator.TimeSeconds);
				entity.Animator.Speed = FloatMemberOrDefault(animatorObject, "speed", entity.Animator.Speed);
				entity.Animator.Playing = BoolMemberOrDefault(animatorObject, "playing", entity.Animator.Playing);
				entity.Animator.Loop = BoolMemberOrDefault(animatorObject, "loop", entity.Animator.Loop);
			}

			if (entityObject.HasMember("rigidBody") && entityObject["rigidBody"].IsObject())
			{
				const auto& rigidBodyObject = entityObject["rigidBody"];
				entity.HasRigidBody = true;
				entity.RigidBodyEnabled = BoolMemberOrDefault(rigidBodyObject, "enabled", entity.RigidBodyEnabled);
				entity.RigidBody.Type = Physics::RigidBodyTypeFromString(StringMemberOrDefault(rigidBodyObject, "type", "Dynamic"));
				entity.RigidBody.Mass = FloatMemberOrDefault(rigidBodyObject, "mass", entity.RigidBody.Mass);
				entity.RigidBody.LinearDamping = FloatMemberOrDefault(rigidBodyObject, "linearDamping", entity.RigidBody.LinearDamping);
				entity.RigidBody.AngularDamping = FloatMemberOrDefault(rigidBodyObject, "angularDamping", entity.RigidBody.AngularDamping);
				entity.RigidBody.UseGravity = BoolMemberOrDefault(rigidBodyObject, "useGravity", entity.RigidBody.UseGravity);
				static_cast<void>(ReadFloat3(rigidBodyObject, "linearVelocity", entity.RigidBody.LinearVelocity));
				static_cast<void>(ReadFloat3(rigidBodyObject, "angularVelocity", entity.RigidBody.AngularVelocity));
			}

			if (entityObject.HasMember("collider") && entityObject["collider"].IsObject())
			{
				const auto& colliderObject = entityObject["collider"];
				entity.HasCollider = true;
				entity.ColliderEnabled = BoolMemberOrDefault(colliderObject, "enabled", entity.ColliderEnabled);
				entity.Collider.Shape = Physics::ColliderShapeFromString(StringMemberOrDefault(colliderObject, "shape", "Box"));
				static_cast<void>(ReadFloat3(colliderObject, "size", entity.Collider.Size));
				entity.Collider.Radius = FloatMemberOrDefault(colliderObject, "radius", entity.Collider.Radius);
				entity.Collider.Height = FloatMemberOrDefault(colliderObject, "height", entity.Collider.Height);
				static_cast<void>(ReadFloat3(colliderObject, "offset", entity.Collider.Offset));
				entity.Collider.IsTrigger = BoolMemberOrDefault(colliderObject, "isTrigger", entity.Collider.IsTrigger);
			}

			if (entityObject.HasMember("physicsMaterial") && entityObject["physicsMaterial"].IsObject())
			{
				const auto& materialObject = entityObject["physicsMaterial"];
				entity.HasPhysicsMaterial = true;
				entity.PhysicsMaterialEnabled = BoolMemberOrDefault(materialObject, "enabled", entity.PhysicsMaterialEnabled);
				entity.PhysicsMaterial.StaticFriction = FloatMemberOrDefault(materialObject, "staticFriction", entity.PhysicsMaterial.StaticFriction);
				entity.PhysicsMaterial.DynamicFriction = FloatMemberOrDefault(materialObject, "dynamicFriction", entity.PhysicsMaterial.DynamicFriction);
				entity.PhysicsMaterial.Restitution = FloatMemberOrDefault(materialObject, "restitution", entity.PhysicsMaterial.Restitution);
			}

			if (entityObject.HasMember("prefabInstance") && entityObject["prefabInstance"].IsObject())
			{
				const auto& prefabObject = entityObject["prefabInstance"];
				entity.HasPrefabInstance = true;
				entity.PrefabInstanceEnabled = BoolMemberOrDefault(prefabObject, "enabled", entity.PrefabInstanceEnabled);
				entity.PrefabInstance.PrefabPath = ResolveStoredProjectPath(StringMemberOrDefault(prefabObject, "prefabPath", ""), project);
				entity.PrefabInstance.SourceName = StringMemberOrDefault(prefabObject, "sourceName", entity.PrefabInstance.SourceName);
				entity.PrefabInstance.TrackPrefabOverrides = BoolMemberOrDefault(prefabObject, "trackPrefabOverrides", entity.PrefabInstance.TrackPrefabOverrides);
			}

			if (entityObject.HasMember("sceneReference") && entityObject["sceneReference"].IsObject())
			{
				const auto& referenceObject = entityObject["sceneReference"];
				entity.HasSceneReference = true;
				entity.SceneReferenceEnabled = BoolMemberOrDefault(referenceObject, "enabled", entity.SceneReferenceEnabled);
				entity.SceneReference.ScenePath = ResolveStoredProjectPath(StringMemberOrDefault(referenceObject, "scenePath", ""), project);
				entity.SceneReference.LoadAdditively = BoolMemberOrDefault(referenceObject, "loadAdditively", entity.SceneReference.LoadAdditively);
				entity.SceneReference.AutoLoad = BoolMemberOrDefault(referenceObject, "autoLoad", entity.SceneReference.AutoLoad);
			}

			if (entityObject.HasMember("script") && entityObject["script"].IsObject())
			{
				const auto& scriptObject = entityObject["script"];
				entity.HasScript = true;
				entity.ScriptEnabled = BoolMemberOrDefault(scriptObject, "enabled", entity.ScriptEnabled);
				entity.Script.ScriptPath = ResolveStoredProjectPath(StringMemberOrDefault(scriptObject, "scriptPath", ""), project);
				entity.Script.ClassName = StringMemberOrDefault(scriptObject, "className", entity.Script.ClassName);
				entity.Script.Language = static_cast<ScriptLanguage>((std::min)(UintMemberOrDefault(scriptObject, "language", 0), 3u));
				entity.Script.RunInEditor = BoolMemberOrDefault(scriptObject, "runInEditor", entity.Script.RunInEditor);
			}

			if (entityObject.HasMember("sprite2D") && entityObject["sprite2D"].IsObject())
			{
				const auto& spriteObject = entityObject["sprite2D"];
				entity.HasSprite2D = true;
				entity.Sprite2DEnabled = BoolMemberOrDefault(spriteObject, "enabled", entity.Sprite2DEnabled);
				entity.Sprite2D.TexturePath = ResolveStoredAssetPath(StringMemberOrDefault(spriteObject, "texturePath", ""), project);
				static_cast<void>(ReadFloat4(spriteObject, "color", entity.Sprite2D.Color));
				static_cast<void>(ReadFloat2(spriteObject, "size", entity.Sprite2D.Size));
				static_cast<void>(ReadFloat2(spriteObject, "pivot", entity.Sprite2D.Pivot));
				entity.Sprite2D.SortingLayer = IntMemberOrDefault(spriteObject, "sortingLayer", entity.Sprite2D.SortingLayer);
				entity.Sprite2D.OrderInLayer = IntMemberOrDefault(spriteObject, "orderInLayer", entity.Sprite2D.OrderInLayer);
			}

			if (entityObject.HasMember("uiElement") && entityObject["uiElement"].IsObject())
			{
				const auto& uiObject = entityObject["uiElement"];
				entity.HasUiElement = true;
				entity.UiElementEnabled = BoolMemberOrDefault(uiObject, "enabled", entity.UiElementEnabled);
				entity.UiElement.Kind = static_cast<UiElementKind>((std::min)(UintMemberOrDefault(uiObject, "kind", 0), 3u));
				entity.UiElement.Text = StringMemberOrDefault(uiObject, "text", entity.UiElement.Text);
				static_cast<void>(ReadFloat2(uiObject, "anchorMin", entity.UiElement.AnchorMin));
				static_cast<void>(ReadFloat2(uiObject, "anchorMax", entity.UiElement.AnchorMax));
				static_cast<void>(ReadFloat2(uiObject, "position", entity.UiElement.Position));
				static_cast<void>(ReadFloat2(uiObject, "size", entity.UiElement.Size));
				static_cast<void>(ReadFloat4(uiObject, "color", entity.UiElement.Color));
			}

			if (entityObject.HasMember("audioSource") && entityObject["audioSource"].IsObject())
			{
				const auto& audioObject = entityObject["audioSource"];
				entity.HasAudioSource = true;
				entity.AudioSourceEnabled = BoolMemberOrDefault(audioObject, "enabled", entity.AudioSourceEnabled);
				entity.AudioSource.ClipPath = ResolveStoredAssetPath(StringMemberOrDefault(audioObject, "clipPath", ""), project);
				entity.AudioSource.Volume = FloatMemberOrDefault(audioObject, "volume", entity.AudioSource.Volume);
				entity.AudioSource.Pitch = FloatMemberOrDefault(audioObject, "pitch", entity.AudioSource.Pitch);
				entity.AudioSource.Loop = BoolMemberOrDefault(audioObject, "loop", entity.AudioSource.Loop);
				entity.AudioSource.PlayOnStart = BoolMemberOrDefault(audioObject, "playOnStart", entity.AudioSource.PlayOnStart);
				entity.AudioSource.Spatialize = BoolMemberOrDefault(audioObject, "spatialize", entity.AudioSource.Spatialize);
				entity.AudioSource.MinDistance = FloatMemberOrDefault(audioObject, "minDistance", entity.AudioSource.MinDistance);
				entity.AudioSource.MaxDistance = FloatMemberOrDefault(audioObject, "maxDistance", entity.AudioSource.MaxDistance);
			}

			if (entityObject.HasMember("navigationAgent") && entityObject["navigationAgent"].IsObject())
			{
				const auto& navigationObject = entityObject["navigationAgent"];
				entity.HasNavigationAgent = true;
				entity.NavigationAgentEnabled = BoolMemberOrDefault(navigationObject, "enabled", entity.NavigationAgentEnabled);
				entity.NavigationAgent.Radius = FloatMemberOrDefault(navigationObject, "radius", entity.NavigationAgent.Radius);
				entity.NavigationAgent.Height = FloatMemberOrDefault(navigationObject, "height", entity.NavigationAgent.Height);
				entity.NavigationAgent.Speed = FloatMemberOrDefault(navigationObject, "speed", entity.NavigationAgent.Speed);
				entity.NavigationAgent.Acceleration = FloatMemberOrDefault(navigationObject, "acceleration", entity.NavigationAgent.Acceleration);
				static_cast<void>(ReadFloat3(navigationObject, "target", entity.NavigationAgent.Target));
				entity.NavigationAgent.HasTarget = BoolMemberOrDefault(navigationObject, "hasTarget", entity.NavigationAgent.HasTarget);
			}

			if (entityObject.HasMember("networkIdentity") && entityObject["networkIdentity"].IsObject())
			{
				const auto& networkObject = entityObject["networkIdentity"];
				entity.HasNetworkIdentity = true;
				entity.NetworkIdentityEnabled = BoolMemberOrDefault(networkObject, "enabled", entity.NetworkIdentityEnabled);
				entity.NetworkIdentity.NetworkId = Uint64MemberOrDefault(networkObject, "networkId", entity.NetworkIdentity.NetworkId);
				entity.NetworkIdentity.PrefabKey = StringMemberOrDefault(networkObject, "prefabKey", entity.NetworkIdentity.PrefabKey);
				entity.NetworkIdentity.ReplicateTransform = BoolMemberOrDefault(networkObject, "replicateTransform", entity.NetworkIdentity.ReplicateTransform);
				entity.NetworkIdentity.ServerAuthoritative = BoolMemberOrDefault(networkObject, "serverAuthoritative", entity.NetworkIdentity.ServerAuthoritative);
			}

			result.Entities.push_back(std::move(entity));
		}

		result.Success = true;
		return result;
	}
}
