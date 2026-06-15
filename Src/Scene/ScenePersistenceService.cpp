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
		std::string& errorMessage)
	{
		(void)renderState;

		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

		writer.StartObject();
		writer.Key("fileVersion");
		writer.Uint(kSceneFileVersion);
		writer.Key("name");
		writer.String(scenePath.stem().string().c_str());
		writer.Key("entities");
		writer.StartArray();

		for (const SceneEntity& entity : scene.GetEntities())
		{
			writer.StartObject();
			writer.Key("name");
			const std::string* entityName = scene.GetEntityName(entity.Id);
			writer.String((entityName && !entityName->empty() ? *entityName : std::string("Entity")).c_str());

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

			result.Entities.push_back(std::move(entity));
		}

		result.Success = true;
		return result;
	}
}
