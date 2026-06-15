#pragma once

#include "Scene/SceneTypes.h"

#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>

class SceneComponentStore
{
public:
	template <typename Component>
	using ComponentMap = std::unordered_map<EntityId, Component>;

	template <typename Component>
	[[nodiscard]] Component& AddComponent(EntityId entityId, Component component)
	{
		auto& components = EnsurePool<Component>().Components;
		auto [componentIt, inserted] = components.insert_or_assign(entityId, std::move(component));
		(void)inserted;
		EnsurePool<Component>().SetEnabled(entityId, true);
		return componentIt->second;
	}

	template <typename Component>
	[[nodiscard]] Component& EnsureComponent(EntityId entityId)
	{
		auto& components = EnsurePool<Component>().Components;
		auto [componentIt, inserted] = components.try_emplace(entityId);
		if (inserted)
		{
			EnsurePool<Component>().SetEnabled(entityId, true);
		}
		return componentIt->second;
	}

	template <typename Component>
	[[nodiscard]] Component* GetComponent(EntityId entityId)
	{
		auto* pool = FindPool<Component>();
		if (!pool)
		{
			return nullptr;
		}

		auto componentIt = pool->Components.find(entityId);
		return componentIt != pool->Components.end() ? &componentIt->second : nullptr;
	}

	template <typename Component>
	[[nodiscard]] const Component* GetComponent(EntityId entityId) const
	{
		const auto* pool = FindPool<Component>();
		if (!pool)
		{
			return nullptr;
		}

		auto componentIt = pool->Components.find(entityId);
		return componentIt != pool->Components.end() ? &componentIt->second : nullptr;
	}

	template <typename Component>
	[[nodiscard]] bool HasComponent(EntityId entityId) const
	{
		return GetComponent<Component>(entityId) != nullptr;
	}

	template <typename Component>
	bool RemoveComponent(EntityId entityId)
	{
		auto* pool = FindPool<Component>();
		return pool ? pool->Remove(entityId) : false;
	}

	template <typename Component>
	[[nodiscard]] bool IsComponentEnabled(EntityId entityId) const
	{
		const auto* pool = FindPool<Component>();
		return pool ? pool->IsEnabled(entityId) : false;
	}

	template <typename Component>
	bool SetComponentEnabled(EntityId entityId, bool enabled)
	{
		auto* pool = FindPool<Component>();
		if (!pool || !pool->Components.contains(entityId))
		{
			return false;
		}

		pool->SetEnabled(entityId, enabled);
		return true;
	}

	void RemoveEntity(EntityId entityId)
	{
		for (auto& [type, pool] : m_Pools)
		{
			(void)type;
			pool->Remove(entityId);
		}
	}

	void Clear() noexcept
	{
		m_Pools.clear();
	}

	template <typename Component>
	[[nodiscard]] ComponentMap<Component>& GetComponents()
	{
		return EnsurePool<Component>().Components;
	}

	template <typename Component>
	[[nodiscard]] const ComponentMap<Component>& GetComponents() const noexcept
	{
		const auto* pool = FindPool<Component>();
		if (pool)
		{
			return pool->Components;
		}

		static const ComponentMap<Component> emptyComponents;
		return emptyComponents;
	}

private:
	struct IComponentPool
	{
		virtual ~IComponentPool() = default;
		virtual bool Remove(EntityId entityId) = 0;
	};

	template <typename Component>
	struct ComponentPool final : IComponentPool
	{
		ComponentMap<Component> Components;
		std::unordered_set<EntityId> DisabledEntities;

		bool Remove(EntityId entityId) override
		{
			DisabledEntities.erase(entityId);
			return Components.erase(entityId) > 0;
		}

		[[nodiscard]] bool IsEnabled(EntityId entityId) const
		{
			return Components.contains(entityId) && !DisabledEntities.contains(entityId);
		}

		void SetEnabled(EntityId entityId, bool enabled)
		{
			if (enabled)
			{
				DisabledEntities.erase(entityId);
				return;
			}

			DisabledEntities.insert(entityId);
		}
	};

	template <typename Component>
	[[nodiscard]] ComponentPool<Component>& EnsurePool()
	{
		const std::type_index type = std::type_index(typeid(Component));
		auto poolIt = m_Pools.find(type);
		if (poolIt == m_Pools.end())
		{
			auto pool = std::make_unique<ComponentPool<Component>>();
			ComponentPool<Component>* rawPool = pool.get();
			m_Pools.emplace(type, std::move(pool));
			return *rawPool;
		}

		return static_cast<ComponentPool<Component>&>(*poolIt->second);
	}

	template <typename Component>
	[[nodiscard]] ComponentPool<Component>* FindPool()
	{
		const std::type_index type = std::type_index(typeid(Component));
		auto poolIt = m_Pools.find(type);
		return poolIt != m_Pools.end()
			? static_cast<ComponentPool<Component>*>(poolIt->second.get())
			: nullptr;
	}

	template <typename Component>
	[[nodiscard]] const ComponentPool<Component>* FindPool() const
	{
		const std::type_index type = std::type_index(typeid(Component));
		auto poolIt = m_Pools.find(type);
		return poolIt != m_Pools.end()
			? static_cast<const ComponentPool<Component>*>(poolIt->second.get())
			: nullptr;
	}

	std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_Pools;
};
