#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <string>

// Ported from the legacy undo_stack (Qt stripped). Each action stores a name plus
// before/after opaque payload strings and the callbacks that re-apply them. Callers
// (bridge mutations) serialize state into undo_data/redo_data; undo() invokes the
// action's undo(undo_data), redo() invokes redo(redo_data).
//
// A callback returns whether it applied anything. One that cannot (its scene or item
// is gone) returns false, and that entry is dropped from both stacks rather than kept:
// undo()/redo() then go on to the next entry, so one keypress never spends itself on
// an entry that does nothing.
class UndoManager {
public:
	using Cb = std::function<bool(const std::string &data)>;

	struct State {
		bool canUndo = false;
		bool canRedo = false;
		std::string undoName; // name of the action undo() would revert ("" if none)
		std::string redoName; // name of the action redo() would re-apply ("" if none)
	};

	void AddAction(const std::string &name, const Cb &undo, const Cb &redo, const std::string &undoData,
		       const std::string &redoData);

	// Apply the newest entry that still applies, dropping any dead ones above it.
	// Returns how many dead entries were dropped.
	size_t Undo();
	size_t Redo();
	void Clear();

	// Ref-counted suppression so a mutation can avoid recording sub-actions.
	void PushDisabled();
	void PopDisabled();
	bool IsEnabled() const { return disableRefs == 0; }

	State GetState() const;

	// Owner sets this to be notified after any state-affecting op (add/undo/redo/
	// clear). Used to EmitEvent. Not fired by push/pop disabled.
	std::function<void()> onChanged;

private:
	struct Item {
		std::string name;
		std::string undoData;
		std::string redoData;
		Cb undo;
		Cb redo;
	};
	std::deque<Item> undoItems;
	std::deque<Item> redoItems;
	int disableRefs = 0;

	size_t Step(std::deque<Item> &from, std::deque<Item> &to, bool undo);

	void notify()
	{
		if (onChanged) {
			onChanged();
		}
	}
};
