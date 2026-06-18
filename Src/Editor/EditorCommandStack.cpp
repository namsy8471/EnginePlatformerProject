#include "EditorCommandStack.h"

#include <utility>

namespace Editor
{
	namespace
	{
		const std::string kEmptyLabel;
	}

	void EditorCommandStack::Clear()
	{
		m_UndoStack.clear();
		m_RedoStack.clear();
	}

	void EditorCommandStack::Execute(EditorCommand command)
	{
		if (command.Execute)
		{
			command.Execute();
		}
		m_UndoStack.push_back(std::move(command));
		m_RedoStack.clear();
	}

	bool EditorCommandStack::CanUndo() const noexcept
	{
		return !m_UndoStack.empty() && static_cast<bool>(m_UndoStack.back().Undo);
	}

	bool EditorCommandStack::CanRedo() const noexcept
	{
		return !m_RedoStack.empty() && static_cast<bool>(m_RedoStack.back().Execute);
	}

	void EditorCommandStack::Undo()
	{
		if (!CanUndo())
		{
			return;
		}

		EditorCommand command = std::move(m_UndoStack.back());
		m_UndoStack.pop_back();
		command.Undo();
		m_RedoStack.push_back(std::move(command));
	}

	void EditorCommandStack::Redo()
	{
		if (!CanRedo())
		{
			return;
		}

		EditorCommand command = std::move(m_RedoStack.back());
		m_RedoStack.pop_back();
		if (command.Execute)
		{
			command.Execute();
		}
		m_UndoStack.push_back(std::move(command));
	}

	const std::string& EditorCommandStack::GetUndoLabel() const noexcept
	{
		return m_UndoStack.empty() ? kEmptyLabel : m_UndoStack.back().Name;
	}

	const std::string& EditorCommandStack::GetRedoLabel() const noexcept
	{
		return m_RedoStack.empty() ? kEmptyLabel : m_RedoStack.back().Name;
	}

	size_t EditorCommandStack::GetUndoCount() const noexcept
	{
		return m_UndoStack.size();
	}

	size_t EditorCommandStack::GetRedoCount() const noexcept
	{
		return m_RedoStack.size();
	}
}
