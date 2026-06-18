#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Editor
{
	enum class EditorPlayState : uint8_t
	{
		Edit,
		EnteringPlay,
		Play,
		Paused,
		ExitingPlay
	};

	struct EditorCommand
	{
		std::string Name;
		std::function<void()> Execute;
		std::function<void()> Undo;
	};

	class EditorCommandStack
	{
	public:
		void Clear();
		void Execute(EditorCommand command);
		[[nodiscard]] bool CanUndo() const noexcept;
		[[nodiscard]] bool CanRedo() const noexcept;
		void Undo();
		void Redo();
		[[nodiscard]] const std::string& GetUndoLabel() const noexcept;
		[[nodiscard]] const std::string& GetRedoLabel() const noexcept;
		[[nodiscard]] size_t GetUndoCount() const noexcept;
		[[nodiscard]] size_t GetRedoCount() const noexcept;

	private:
		std::vector<EditorCommand> m_UndoStack;
		std::vector<EditorCommand> m_RedoStack;
	};
}
