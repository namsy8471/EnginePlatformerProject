#pragma once

#include "Memory/MemorySystem.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Memory
{
	template <typename T, MemoryTag TagValue>
	class StdAllocator
	{
	public:
		using value_type = T;
		using propagate_on_container_move_assignment = std::true_type;
		using is_always_equal = std::true_type;

		constexpr StdAllocator() noexcept = default;

		template <typename U>
		constexpr StdAllocator(const StdAllocator<U, TagValue>&) noexcept
		{
		}

		[[nodiscard]] T* allocate(size_t count)
		{
			if (count > (std::numeric_limits<size_t>::max)() / sizeof(T))
			{
				throw std::bad_array_new_length();
			}

			return static_cast<T*>(Memory::Allocate(sizeof(T) * count, alignof(T), TagValue));
		}

		void deallocate(T* pointer, size_t) noexcept
		{
			Memory::Free(pointer);
		}

		template <typename U>
		struct rebind
		{
			using other = StdAllocator<U, TagValue>;
		};
	};

	template <typename T, typename U, MemoryTag TagValue>
	[[nodiscard]] constexpr bool operator==(const StdAllocator<T, TagValue>&, const StdAllocator<U, TagValue>&) noexcept
	{
		return true;
	}

	template <typename T, typename U, MemoryTag TagValue>
	[[nodiscard]] constexpr bool operator!=(const StdAllocator<T, TagValue>& lhs, const StdAllocator<U, TagValue>& rhs) noexcept
	{
		return !(lhs == rhs);
	}

	template <typename T, MemoryTag TagValue>
	using Vector = std::vector<T, StdAllocator<T, TagValue>>;

	template <typename T, MemoryTag TagValue>
	using UnorderedSet = std::unordered_set<T, std::hash<T>, std::equal_to<T>, StdAllocator<T, TagValue>>;

	template <typename Key, typename Value, MemoryTag TagValue>
	using UnorderedMap = std::unordered_map<
		Key,
		Value,
		std::hash<Key>,
		std::equal_to<Key>,
		StdAllocator<std::pair<const Key, Value>, TagValue>>;
}
