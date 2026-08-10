#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

#include "Schema.hpp"

namespace History {

// Why a scheduled start is refused while a broadcast is already running. Spelled
// once: the runner shows it on the chip through the armed window, and the setup
// refuses again at the moment it would otherwise touch the routing.
inline constexpr const char *kAlreadyStreamingReason = "something is already streaming";

// One routing edge, reduced to what applying an entry needs to decide.
struct RoutingBinding {
	std::string uuid;
	std::string profileId;
	bool enabled = false;
};

struct RoutingSeam {
	std::function<std::vector<RoutingBinding>()> read;
	// Called only for a binding whose flag actually differs, so the caller's own
	// persist-and-notify tail runs once per real change and not at all otherwise.
	std::function<void(const std::string &uuid, bool enabled)> write;
};

// The remembered per-destination metadata, as opaque field bags. Nothing here
// interprets them beyond merging keys.
struct MetadataSeam {
	std::function<nlohmann::json(const std::string &profileId)> read;
	std::function<void(const std::string &profileId, const nlohmann::json &fields)> write;
	std::function<void(const std::string &profileId)> clear;
	std::function<void()> save;
};

// Loads a scheduled entry's destinations and metadata into the live configuration
// for the duration of one broadcast, and puts back exactly what it changed.
//
// This mutates configuration the user owns, so the window in which that is true is
// deliberately as short as it can be: applied at T-0 immediately before going
// live, not when the entry arms. Arming is a preparation and stays read-only.
//
// Every effect is an injected callable, so the capture-and-restore logic is
// testable without libobs, the bindings file or a broadcast -- which matters more
// here than anywhere else in this feature, since a restore that silently does the
// wrong thing leaves the user's destinations rewritten with nothing to show for it.
class ScheduledSetup {
public:
	RoutingSeam routing;
	MetadataSeam metadata;
	// Nothing here touches the routing while this answers true.
	std::function<bool()> isStreaming;
	std::function<void(const std::string &)> log;

	// False + `reason` when nothing was applied and the caller must not go live.
	bool Apply(const std::vector<ScheduleDestination> &destinations, std::string &reason);

	// Put back what Apply changed, per item: a binding or a bag someone else has
	// altered since is left alone rather than overwritten, and one such change does
	// not strand the rest of the restore.
	//
	// A no-op while streaming, keeping the snapshot for a later call, because
	// restoring the routing means flipping bindings and a binding flipped off stops
	// its output. Whatever is live is live to these destinations. The broadcast's
	// stop edge is what completes it.
	void Revert();

	bool IsApplied() const { return applied_; }

	// The entry's metadata in the shape providers read. `category` is an object
	// there and every provider keys on its id, while the name is what a prefill
	// shows -- so the two halves go to the two places. Empty values are left out,
	// and Apply merges rather than replaces, so a value the entry does not carry
	// keeps whatever the user had remembered.
	static nlohmann::json MetadataFields(const ScheduleDestination &destination);

private:
	struct TouchedBinding {
		std::string uuid;
		bool before = false;
		bool after = false;
	};
	struct TouchedOverride {
		std::string profileId;
		nlohmann::json before;
		nlohmann::json after;
		bool existed = false;
	};

	void Flip(const std::string &uuid, bool enabled);

	bool applied_ = false;
	std::vector<TouchedBinding> bindings_;
	std::vector<TouchedOverride> overrides_;
};

} // namespace History
