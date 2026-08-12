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
	//
	// False means the binding does not reliably hold `enabled` afterwards -- the
	// single-live-stream rule refused it outright, or it landed but something past
	// the mutation went wrong. It does NOT mean nothing changed, so `read` is what
	// decides whether a change has to be recorded, never this result on its own. A
	// change that happened and went unrecorded is one the restore cannot undo,
	// which is how the user's routing goes missing for good.
	std::function<bool(const std::string &uuid, bool enabled)> write;
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
	//
	// Narrowing only: an entry may take destinations off the air, never put one on.
	// A destination it names that is switched off stays switched off, and an entry
	// that names nothing currently switched on is refused rather than applied onto an
	// empty enabled set. Switching a binding on whose canvas has no other enabled
	// binding wakes that canvas and starts an encode the user deliberately turned off,
	// which is not something a plan running unattended may decide to do.
	bool Apply(const std::vector<ScheduleDestination> &destinations, std::string &reason);

	// Put back what Apply changed, per item: a binding or a bag someone else has
	// altered since is left alone rather than overwritten, and one such change does
	// not strand the rest of the restore.
	//
	// Every routing record is a binding Apply took off the air, since an entry may
	// only narrow the enabled set, so the restore is one unordered pass of switching
	// those back on. A write that is refused anyway keeps its snapshot for a later
	// call rather than being dropped -- losing the user's routing silently is the
	// one outcome a restore must not have.
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
	//
	// That is why an entry naming a category with no id carries no category at all:
	// entries written before the id column existed have only the name, no provider
	// reads the name, and sending the pair would replace a remembered id with an
	// empty one -- going live under no category rather than the one the user last
	// used.
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

	// Ask the seam for `enabled` and answer whether the binding holds it
	// afterwards. The one place that reconciles a write result against the state,
	// so both the record-a-change and the restore-is-still-owed decisions read it
	// the same way.
	bool WriteLanded(const std::string &uuid, bool enabled);

	// False when the binding does not hold `enabled` afterwards, and nothing is
	// recorded then. A change that landed is recorded whatever the seam reported.
	bool Flip(const std::string &uuid, bool enabled);

	bool applied_ = false;
	std::vector<TouchedBinding> bindings_;
	std::vector<TouchedOverride> overrides_;
};

} // namespace History
