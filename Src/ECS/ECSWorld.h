#pragma once

#include "ECS/ComponentPool.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ECS
{
	class ECSWorld
	{
	public:
		[[nodiscard]] Entity CreateEntity()
		{
			uint32_t index = 0;
			if (!m_FreeList.empty())
			{
				index = m_FreeList.back();
				m_FreeList.pop_back();
				m_Alive[index] = true;
			}
			else
			{
				index = static_cast<uint32_t>(m_Generations.size());
				m_Generations.push_back(1);
				m_Alive.push_back(true);
			}

			return Entity{ index, m_Generations[index] };
		}

		bool DestroyEntity(Entity entity)
		{
			if (!IsAlive(entity))
			{
				return false;
			}

			for (auto& [type, pool] : m_Pools)
			{
				(void)type;
				pool->Remove(entity);
			}

			m_Alive[entity.Index] = false;
			++m_Generations[entity.Index];
			if (m_Generations[entity.Index] == 0)
			{
				++m_Generations[entity.Index];
			}
			m_FreeList.push_back(entity.Index);
			return true;
		}

		void Clear()
		{
			for (auto& [type, pool] : m_Pools)
			{
				(void)type;
				pool->Clear();
			}

			m_Pools.clear();
			m_Generations.clear();
			m_Alive.clear();
			m_FreeList.clear();
		}

		[[nodiscard]] bool IsAlive(Entity entity) const noexcept
		{
			return entity.IsValid()
				&& entity.Index < m_Generations.size()
				&& m_Alive[entity.Index]
				&& m_Generations[entity.Index] == entity.Generation;
		}

		template <typename ComponentType, typename... Args>
		ComponentType& AddComponent(Entity entity, Args&&... args)
		{
			if (!IsAlive(entity))
			{
				throw std::invalid_argument("Cannot add a component to a dead ECS entity.");
			}

			return GetOrCreatePool<ComponentType>().Add(entity, std::forward<Args>(args)...);
		}

		template <typename ComponentType>
		void RemoveComponent(Entity entity)
		{
			if (ComponentPool<ComponentType>* pool = GetPool<ComponentType>())
			{
				pool->Remove(entity);
			}
		}

		template <typename ComponentType>
		[[nodiscard]] ComponentType* GetComponent(Entity entity)
		{
			if (!IsAlive(entity))
			{
				return nullptr;
			}

			ComponentPool<ComponentType>* pool = GetPool<ComponentType>();
			return pool ? pool->Get(entity) : nullptr;
		}

		template <typename ComponentType>
		[[nodiscard]] const ComponentType* GetComponent(Entity entity) const
		{
			if (!IsAlive(entity))
			{
				return nullptr;
			}

			const ComponentPool<ComponentType>* pool = GetPool<ComponentType>();
			return pool ? pool->Get(entity) : nullptr;
		}

		template <typename ComponentType>
		[[nodiscard]] bool HasComponent(Entity entity) const
		{
			return GetComponent<ComponentType>(entity) != nullptr;
		}

		template <typename... ComponentTypes, typename FunctionType>
		void ForEach(FunctionType&& function)
		{
			static_assert(sizeof...(ComponentTypes) > 0, "ECSWorld::ForEach requires at least one component type.");

			auto componentPools = std::tuple<ComponentPool<ComponentTypes>*...>{ GetPool<ComponentTypes>()... };
			if (!std::apply([](auto*... pools) { return ((pools != nullptr) && ...); }, componentPools))
			{
				return;
			}

			auto* firstPool = std::get<0>(componentPools);
			const std::vector<Entity>& entities = firstPool->Entities();
			for (Entity entity : entities)
			{
				if (!IsAlive(entity))
				{
					continue;
				}

				auto componentPointers = std::apply(
					[entity](auto*... pools)
					{
						return std::tuple<ComponentTypes*...>{ pools->Get(entity)... };
					},
					componentPools);
				if (!std::apply([](auto*... components) { return ((components != nullptr) && ...); }, componentPointers))
				{
					continue;
				}

				std::apply(
					[&](auto*... components)
					{
						function(entity, *components...);
					},
					componentPointers);
			}
		}

		template <typename ComponentType>
		[[nodiscard]] ComponentPool<ComponentType>* GetPool()
		{
			const auto poolIt = m_Pools.find(std::type_index(typeid(ComponentType)));
			if (poolIt == m_Pools.end())
			{
				return nullptr;
			}

			return static_cast<ComponentPool<ComponentType>*>(poolIt->second.get());
		}

		template <typename ComponentType>
		[[nodiscard]] const ComponentPool<ComponentType>* GetPool() const
		{
			const auto poolIt = m_Pools.find(std::type_index(typeid(ComponentType)));
			if (poolIt == m_Pools.end())
			{
				return nullptr;
			}

			return static_cast<const ComponentPool<ComponentType>*>(poolIt->second.get());
		}

		[[nodiscard]] size_t EntityCapacity() const noexcept
		{
			return m_Generations.size();
		}

	private:
		template <typename ComponentType>
		ComponentPool<ComponentType>& GetOrCreatePool()
		{
			const std::type_index typeIndex(typeid(ComponentType));
			auto poolIt = m_Pools.find(typeIndex);
			if (poolIt == m_Pools.end())
			{
				poolIt = m_Pools.emplace(typeIndex, std::make_unique<ComponentPool<ComponentType>>()).first;
			}

			return *static_cast<ComponentPool<ComponentType>*>(poolIt->second.get());
		}

		std::vector<uint32_t> m_Generations;
		std::vector<bool> m_Alive;
		std::vector<uint32_t> m_FreeList;
		std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_Pools;
	};
}
