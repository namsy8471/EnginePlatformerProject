#pragma once

#include <cstdint>
#include <functional>
#include <limits>

namespace ECS
{
	struct Entity
	{
		static constexpr uint32_t InvalidIndex = (std::numeric_limits<uint32_t>::max)();

		uint32_t Index = InvalidIndex;
		uint32_t Generation = 0;

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return Index != InvalidIndex;
		}
	};

	[[nodiscard]] constexpr bool operator==(Entity lhs, Entity rhs) noexcept
	{
		return lhs.Index == rhs.Index && lhs.Generation == rhs.Generation;
	}

	[[nodiscard]] constexpr bool operator!=(Entity lhs, Entity rhs) noexcept
	{
		return !(lhs == rhs);
	}
}

template <>
struct std::hash<ECS::Entity>
{
	[[nodiscard]] size_t operator()(ECS::Entity entity) const noexcept
	{
		const uint64_t packed = (static_cast<uint64_t>(entity.Generation) << 32) | entity.Index;
		return std::hash<uint64_t>{}(packed);
	}
};
