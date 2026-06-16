#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Resources
{
	enum class ResourceKind : uint8_t
	{
		Unknown,
		Mesh,
		Texture,
		Material,
		Skeleton,
		Animation,
		Scene,
		Shader,
		Script,
		Audio,
		Other
	};

	enum class ResourceLoadState : uint8_t
	{
		Undefined,
		Declared,
		Preparing,
		Prepared,
		Loaded,
		Failed,
		Unloaded
	};

	enum class ResourcePriority : uint8_t
	{
		Low,
		Normal,
		High
	};

	struct ResourceHandle
	{
		uint64_t Id = 0;
		uint32_t Generation = 0;
		ResourceKind Kind = ResourceKind::Unknown;

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return Id != 0;
		}

		[[nodiscard]] friend bool operator==(ResourceHandle lhs, ResourceHandle rhs) noexcept
		{
			return lhs.Id == rhs.Id && lhs.Generation == rhs.Generation && lhs.Kind == rhs.Kind;
		}
	};

	struct ResourceLocation
	{
		std::filesystem::path Path;
		std::string Type = "FileSystem";
		bool Recursive = true;
	};

	struct ResourceDeclaration
	{
		std::string Name;
		std::filesystem::path SourcePath;
		ResourceKind Kind = ResourceKind::Unknown;
		std::string GroupName = "General";
		ResourcePriority Priority = ResourcePriority::Normal;
		uintmax_t SizeBytes = 0;
		bool Streamable = true;
	};

	struct ResourceRecord
	{
		ResourceHandle Handle;
		std::string Name;
		std::filesystem::path SourcePath;
		ResourceKind Kind = ResourceKind::Unknown;
		std::string GroupName = "General";
		ResourceLoadState State = ResourceLoadState::Declared;
		ResourcePriority Priority = ResourcePriority::Normal;
		uintmax_t SizeBytes = 0;
		uint32_t RefCount = 0;
		uint64_t LastAccessFrame = 0;
		bool Streamable = true;
		std::string ErrorMessage;
	};

	struct ResourceGroup
	{
		std::string Name = "General";
		std::vector<ResourceLocation> Locations;
		std::vector<ResourceHandle> Resources;
		bool Initialized = false;
	};

	struct ResourceManagerStats
	{
		size_t GroupCount = 0;
		size_t ResourceCount = 0;
		size_t DeclaredCount = 0;
		size_t PreparedCount = 0;
		size_t LoadedCount = 0;
		size_t FailedCount = 0;
		uintmax_t DeclaredBytes = 0;
		uintmax_t LoadedBytes = 0;
	};

	[[nodiscard]] const char* ToString(ResourceKind kind) noexcept;
	[[nodiscard]] const char* ToString(ResourceLoadState state) noexcept;
}
