#include "UndoManager.hpp"

#include <cstddef>
#include <utility>

namespace {
// Exception-safe suppression of recording around an in-flight apply: a throwing
// callback can never leave disableRefs stuck > 0.
struct DisableGuard {
	UndoManager *m;
	explicit DisableGuard(UndoManager *m_) : m(m_) { m->PushDisabled(); }
	~DisableGuard() { m->PopDisabled(); }
};
} // namespace

void UndoManager::AddAction(const std::string &name, const Cb &undo, const Cb &redo, const std::string &undoData,
			    const std::string &redoData)
{
	// Don't record sub-actions while a mutation (or an in-flight undo/redo) has
	// suppressed recording.
	if (!IsEnabled()) {
		return;
	}

	undoItems.push_back(Item{name, undoData, redoData, undo, redo});
	// Cap the history like stock OBS: an unbounded stack would grow the payload
	// snapshots without limit over a long session. Drop the oldest past the cap.
	constexpr size_t kMaxUndo = 5000;
	while (undoItems.size() > kMaxUndo) {
		undoItems.pop_front();
	}
	// A fresh action invalidates the redo branch.
	redoItems.clear();
	notify();
}

void UndoManager::Undo()
{
	// Refuse to re-enter while an apply is already in flight (recording disabled).
	if (disableRefs != 0) {
		return;
	}
	if (undoItems.empty()) {
		return;
	}

	Item item = std::move(undoItems.back());
	undoItems.pop_back();

	if (item.undo) {
		// Re-applying a snapshot must not record a new action; the guard
		// restores the ref count even if the callback throws.
		DisableGuard guard(this);
		item.undo(item.undoData);
	}

	redoItems.push_back(std::move(item));
	notify();
}

void UndoManager::Redo()
{
	if (disableRefs != 0) {
		return;
	}
	if (redoItems.empty()) {
		return;
	}

	Item item = std::move(redoItems.back());
	redoItems.pop_back();

	if (item.redo) {
		DisableGuard guard(this);
		item.redo(item.redoData);
	}

	undoItems.push_back(std::move(item));
	notify();
}

void UndoManager::Clear()
{
	undoItems.clear();
	redoItems.clear();
	notify();
}

void UndoManager::PushDisabled()
{
	disableRefs++;
}

void UndoManager::PopDisabled()
{
	if (disableRefs > 0) {
		disableRefs--;
	}
}

UndoManager::State UndoManager::GetState() const
{
	State s;
	s.canUndo = !undoItems.empty();
	s.canRedo = !redoItems.empty();
	s.undoName = undoItems.empty() ? std::string() : undoItems.back().name;
	s.redoName = redoItems.empty() ? std::string() : redoItems.back().name;
	return s;
}
