#pragma once

#include "ECS/Entity.h"
#include "Memory/StdAllocator.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ECS
{
	class IComponentPool
	{
	public:
		virtual ~IComponentPool() = default;

		virtual void Remove(Entity entity) = 0;
		virtual void Clear() = 0;
		[[nodiscard]] virtual bool Has(Entity entity) const = 0;
		[[nodiscard]] virtual size_t Size() const noexcept = 0;
	};

	template <typename ComponentType>
	class ComponentPool final : public IComponentPool
	{
	public:
		template <typename... Args>
		ComponentType& Add(Entity entity, Args&&... args)
		{
			if (ComponentType* existing = Get(entity))
			{
				*existing = ComponentType{ std::forward<Args>(args)... };
				return *existing;
			}

			const size_t denseIndex = m_Components.size();
			EnsureSparseCapacity(entity.Index);
			m_Sparse[entity.Index] = denseIndex;
			m_Entities.push_back(entity);
			m_Components.emplace_back(std::forward<Args>(args)...);
			return m_Components.back();
		}

		void Remove(Entity entity) override
		{
			if (entity.Index >= m_Sparse.size())
			{
				return;
			}

			const size_t denseIndex = m_Sparse[entity.Index];
			if (denseIndex == InvalidDenseIndex)
			{
				return;
			}

			if (denseIndex >= m_Entities.size() || m_Entities[denseIndex] != entity)
			{
				return;
			}

			const size_t lastIndex = m_Components.size() - 1;
			if (denseIndex != lastIndex)
			{
				m_Components[denseIndex] = std::move(m_Components[lastIndex]);
				m_Entities[denseIndex] = m_Entities[lastIndex];
				m_Sparse[m_Entities[denseIndex].Index] = denseIndex;
			}

			m_Components.pop_back();
			m_Entities.pop_back();
			m_Sparse[entity.Index] = InvalidDenseIndex;
		}

		void Clear() override
		{
			m_Components.clear();
			m_Entities.clear();
			m_Sparse.clear();
		}

		[[nodiscard]] bool Has(Entity entity) const override
		{
			return Get(entity) != nullptr;
		}

		[[nodiscard]] ComponentType* Get(Entity entity)
		{
			if (entity.Index >= m_Sparse.size())
			{
				return nullptr;
			}

			const size_t denseIndex = m_Sparse[entity.Index];
			if (denseIndex == InvalidDenseIndex)
			{
				return nullptr;
			}

			if (denseIndex >= m_Components.size() || m_Entities[denseIndex] != entity)
			{
				return nullptr;
			}

			return &m_Components[denseIndex];
		}

		[[nodiscard]] const ComponentType* Get(Entity entity) const
		{
			if (entity.Index >= m_Sparse.size())
			{
				return nullptr;
			}

			const size_t denseIndex = m_Sparse[entity.Index];
			if (denseIndex == InvalidDenseIndex)
			{
				return nullptr;
			}

			if (denseIndex >= m_Components.size() || m_Entities[denseIndex] != entity)
			{
				return nullptr;
			}

			return &m_Components[denseIndex];
		}

		[[nodiscard]] size_t Size() const noexcept override
		{
			return m_Components.size();
		}

		[[nodiscard]] const Memory::Vector<Entity, Memory::MemoryTag::ECS>& Entities() const noexcept
		{
			return m_Entities;
		}

		[[nodiscard]] Memory::Vector<ComponentType, Memory::MemoryTag::ECS>& Components() noexcept
		{
			return m_Components;
		}

		[[nodiscard]] const Memory::Vector<ComponentType, Memory::MemoryTag::ECS>& Components() const noexcept
		{
			return m_Components;
		}

	private:
		static constexpr size_t InvalidDenseIndex = (std::numeric_limits<size_t>::max)();

		void EnsureSparseCapacity(uint32_t entityIndex)
		{
			if (entityIndex >= m_Sparse.size())
			{
				m_Sparse.resize(static_cast<size_t>(entityIndex) + 1, InvalidDenseIndex);
			}
		}

		Memory::Vector<ComponentType, Memory::MemoryTag::ECS> m_Components;
		Memory::Vector<Entity, Memory::MemoryTag::ECS> m_Entities;
		Memory::Vector<size_t, Memory::MemoryTag::ECS> m_Sparse;
	};
}
