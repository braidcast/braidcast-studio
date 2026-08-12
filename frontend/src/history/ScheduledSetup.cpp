#include "ScheduledSetup.hpp"

#include <algorithm>

#include "util/json_util.hpp"

namespace History {

namespace {

bool Contains(const std::vector<std::string> &haystack, const std::string &needle)
{
	return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

const RoutingBinding *Find(const std::vector<RoutingBinding> &bindings, const std::string &uuid)
{
	for (const RoutingBinding &b : bindings) {
		if (b.uuid == uuid) {
			return &b;
		}
	}
	return nullptr;
}

} // namespace

nlohmann::json ScheduledSetup::MetadataFields(const ScheduleDestination &destination)
{
	nlohmann::json fields = nlohmann::json::object();
	if (!destination.title.empty()) {
		fields["title"] = destination.title;
	}
	if (!destination.categoryId.empty()) {
		fields["category"] = nlohmann::json{{"id", destination.categoryId}, {"name", destination.category}};
	}
	const nlohmann::json tags = JsonUtil::ParseJson(destination.tags);
	if (tags.is_array() && !tags.empty()) {
		fields["tags"] = tags;
	}
	return fields;
}

std::vector<ScheduledSetup::PlannedOverride>
ScheduledSetup::PlanMetadata(const std::vector<ScheduleDestination> &destinations,
			     const std::function<nlohmann::json(const std::string &)> &read)
{
	std::vector<PlannedOverride> plan;
	// One entry per profile, not per destination: two destinations naming the same
	// profile would otherwise have the second merge over what the first produced,
	// and the user's own value would never come back.
	std::vector<std::string> captured;
	for (const ScheduleDestination &d : destinations) {
		if (d.profileId.empty() || Contains(captured, d.profileId)) {
			continue;
		}
		const nlohmann::json fields = MetadataFields(d);
		if (fields.empty()) {
			continue;
		}
		captured.push_back(d.profileId);
		nlohmann::json existing = read ? read(d.profileId) : nlohmann::json::object();
		if (!existing.is_object()) {
			existing = nlohmann::json::object();
		}
		// Merged key by key rather than replaced: the entry states what it wants
		// changed, and a field it does not carry keeps what the user remembered.
		// An object field merges a level deeper for the same reason -- `category`
		// carries an id and a name, and replacing the object would take a sibling
		// key down with it.
		nlohmann::json merged = existing;
		for (auto it = fields.begin(); it != fields.end(); ++it) {
			nlohmann::json &target = merged[it.key()];
			if (!it.value().is_object() || !target.is_object()) {
				target = it.value();
				continue;
			}
			for (auto field = it.value().begin(); field != it.value().end(); ++field) {
				if (field.value().is_string() && field.value().get<std::string>().empty()) {
					continue;
				}
				target[field.key()] = field.value();
			}
		}
		plan.push_back(PlannedOverride{d.profileId, existing, merged, !existing.empty()});
	}
	return plan;
}

bool ScheduledSetup::WriteLanded(const std::string &uuid, bool enabled)
{
	if (routing.write(uuid, enabled)) {
		return true;
	}
	// A refusal is not proof that nothing changed: a seam can assign the flag, stop
	// the output and reconcile, and only then discover it cannot persist. So the
	// state is what answers, not the result -- treating a change that happened as
	// one that did not is how it goes unrecorded, and an unrecorded change is one
	// no restore can undo.
	const std::vector<RoutingBinding> current = routing.read ? routing.read() : std::vector<RoutingBinding>();
	const RoutingBinding *now = Find(current, uuid);
	return now && now->enabled == enabled;
}

bool ScheduledSetup::Flip(const std::string &uuid, bool enabled)
{
	if (!WriteLanded(uuid, enabled)) {
		return false;
	}
	// One record per binding, keeping the `before` from the first time this
	// application touched it: a restore the routing refused leaves its record
	// behind, and a second record for the same uuid would leave the restore
	// deciding between two answers. What has to come back is the value the user
	// had, not the one an unfinished restore left.
	for (TouchedBinding &touched : bindings_) {
		if (touched.uuid == uuid) {
			touched.after = enabled;
			return true;
		}
	}
	bindings_.push_back(TouchedBinding{uuid, !enabled, enabled});
	return true;
}

bool ScheduledSetup::NarrowRouting(const std::vector<ScheduleDestination> &destinations, std::string &reason)
{
	if (routing.read && routing.write) {
		const std::vector<RoutingBinding> before = routing.read();

		// The ENABLED bindings the entry names, deduped: a profile can be bound
		// on several canvases and only one of them may be enabled, so the
		// enabled one is the binding a scheduled entry routes through. A profile
		// with no enabled binding resolves to nothing, which is what leaves it
		// switched off below rather than being switched on for it.
		std::vector<std::string> wanted;
		for (const ScheduleDestination &d : destinations) {
			if (d.profileId.empty()) {
				continue;
			}
			for (const RoutingBinding &b : before) {
				if (b.profileId == d.profileId && b.enabled) {
					if (!Contains(wanted, b.uuid)) {
						wanted.push_back(b.uuid);
					}
					break;
				}
			}
		}

		// Narrowing to nothing is not narrowing. With nothing left to keep on the air
		// the pass below would switch every binding off and report success, and the
		// go-live would broadcast nowhere. Asked of the resolved set rather than of the
		// destination list, so it covers an entry that names none as well as one whose
		// destinations are all switched off -- the runner's armability gate refuses
		// both before this is reached, but an outcome this bad must not rest on a
		// caller remembering to check.
		if (wanted.empty()) {
			reason = destinations.empty() ? kNoDestinationsReason
						      : "none of this entry's destinations are switched on";
			return false;
		}

		// An entry narrows the enabled set; it never widens it. Switching a binding
		// on whose canvas has no other enabled binding wakes that canvas and starts a
		// whole extra encode -- an entry naming a destination the user had switched
		// off would put their machine under a load they deliberately turned off, at a
		// time they may not be watching. A destination the entry cannot reach is
		// reported against that destination instead, and whether the entry still goes
		// live without it is the user's own setting.
		//
		// A refusal abandons the whole application rather than going live on a
		// half-applied routing, which would broadcast to destinations nobody
		// scheduled.
		for (const RoutingBinding &b : before) {
			if (b.enabled && !Contains(wanted, b.uuid) && !Flip(b.uuid, false)) {
				reason = "a destination could not be taken off the air for this entry";
				Revert();
				return false;
			}
		}
	}
	return true;
}

bool ScheduledSetup::ApplyRouting(const std::vector<ScheduleDestination> &destinations, std::string &reason)
{
	if (isStreaming && isStreaming()) {
		reason = kAlreadyStreamingReason;
		return false;
	}
	Revert(); // two applications must never stack

	if (!NarrowRouting(destinations, reason)) {
		return false;
	}

	applied_ = true;
	if (log) {
		log("[schedule] applied " + std::to_string(bindings_.size()) + " routing change(s)");
	}
	return true;
}

bool ScheduledSetup::Apply(const std::vector<ScheduleDestination> &destinations, std::string &reason)
{
	if (isStreaming && isStreaming()) {
		reason = kAlreadyStreamingReason;
		return false;
	}
	Revert(); // two applications must never stack

	if (!NarrowRouting(destinations, reason)) {
		return false;
	}

	// Planned first, then written: the plan is the record Revert restores from, so
	// what a caller can preview through PlanMetadata is by construction what goes
	// out here. Assigned rather than appended because Revert above has already let
	// go of whatever the previous application was holding.
	overrides_ = PlanMetadata(destinations, metadata.read);
	for (const PlannedOverride &planned : overrides_) {
		if (metadata.write) {
			metadata.write(planned.profileId, planned.after);
		}
	}
	if (!overrides_.empty() && metadata.save) {
		metadata.save();
	}

	applied_ = true;
	if (log) {
		log("[schedule] applied " + std::to_string(bindings_.size()) + " routing change(s) and " +
		    std::to_string(overrides_.size()) + " metadata override(s)");
	}
	return true;
}

void ScheduledSetup::Revert()
{
	if (bindings_.empty() && overrides_.empty()) {
		applied_ = false;
		return;
	}
	if (isStreaming && isStreaming()) {
		return;
	}

	// What the routing would not take. Held rather than dropped, so the next call
	// -- the broadcast's stop edge, or the next entry's Apply -- tries again
	// instead of leaving the user's destinations rewritten with no record of it.
	std::vector<TouchedBinding> refused;
	if (routing.read && routing.write) {
		// One pass, in any order: Apply only ever takes bindings off the air, so
		// every record here is a disable to undo and switching one back on cannot
		// collide with another. Ordering mattered only while Apply could move a
		// profile from one canvas to another, which left a binding holding that
		// profile that had to come off before the user's own could go back on.
		const std::vector<RoutingBinding> current = routing.read();
		for (const TouchedBinding &touched : bindings_) {
			const RoutingBinding *now = Find(current, touched.uuid);
			// Per binding rather than over the set as a whole: an unrelated
			// change elsewhere must not strand every other restore.
			if (!now || now->enabled != touched.after) {
				continue;
			}
			if (!WriteLanded(touched.uuid, touched.before)) {
				refused.push_back(touched);
			}
		}
	}

	bool restoredAny = false;
	for (const PlannedOverride &touched : overrides_) {
		const nlohmann::json now = metadata.read ? metadata.read(touched.profileId) : nlohmann::json();
		if (now != touched.after) {
			continue; // edited since; that edit is the newer intent
		}
		if (touched.existed && metadata.write) {
			metadata.write(touched.profileId, touched.before);
		} else if (!touched.existed && metadata.clear) {
			metadata.clear(touched.profileId);
		}
		restoredAny = true;
	}
	if (restoredAny && metadata.save) {
		metadata.save();
	}

	overrides_.clear();
	bindings_ = refused;
	applied_ = !bindings_.empty();
	if (!log) {
		return;
	}
	if (bindings_.empty()) {
		log("[schedule] put back the routing and metadata the scheduled entry changed");
	} else {
		log("[schedule] could not put back " + std::to_string(bindings_.size()) +
		    " routing change(s); holding them to retry");
	}
}

} // namespace History
