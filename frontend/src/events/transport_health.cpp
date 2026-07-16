#include "transport_health.hpp"

#include <chrono>

namespace Transports {

const char *TransportHealth::StateName(State state)
{
	switch (state) {
	case State::Connecting:
		return "connecting";
	case State::Connected:
		return "connected";
	case State::Reconnecting:
		return "reconnecting";
	case State::Failed:
		return "failed";
	case State::Disconnected:
	default:
		return "disconnected";
	}
}

namespace {

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace

void TransportHealth::Report(const std::string &id, State state, const std::string &error)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Entry &e = entries_[id];
		// Coalesce: an unchanged (state, error) report does not re-notify -- transports
		// report at every seam, but only real transitions push to the UI.
		if (e.id == id && e.state == state && e.lastError == error) {
			return;
		}
		e.id = id;
		e.state = state;
		e.lastError = error;
		e.updatedAtMs = NowMs();
	}
	// Notify outside the lock: onChanged marshals to the UI thread and reads Snapshot()
	// back, which re-locks mutex_ -- holding it across the callback would deadlock.
	NotifyChanged();
}

std::vector<TransportHealth::Entry> TransportHealth::Snapshot() const
{
	std::vector<Entry> out;
	std::lock_guard<std::mutex> lock(mutex_);
	out.reserve(entries_.size());
	for (const auto &kv : entries_) {
		out.push_back(kv.second);
	}
	return out;
}

void TransportHealth::Clear()
{
	bool had = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		had = !entries_.empty();
		entries_.clear();
	}
	if (had) {
		NotifyChanged();
	}
}

void TransportHealth::NotifyChanged()
{
	if (onChanged) {
		onChanged();
	}
}

TransportHealth &Health()
{
	static TransportHealth health;
	return health;
}

} // namespace Transports
