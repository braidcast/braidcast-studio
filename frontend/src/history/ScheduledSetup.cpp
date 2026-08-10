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

bool ScheduledSetup::Flip(const std::string &uuid, bool enabled)
{
	if (!routing.write(uuid, enabled)) {
		return false;
	}
	bindings_.push_back(TouchedBinding{uuid, !enabled, enabled});
	return true;
}

bool ScheduledSetup::Apply(const std::vector<ScheduleDestination> &destinations, std::string &reason)
{
	if (isStreaming && isStreaming()) {
		reason = kAlreadyStreamingReason;
		return false;
	}
	Revert(); // two applications must never stack

	if (routing.read && routing.write) {
		const std::vector<RoutingBinding> before = routing.read();

		// The bindings the entry names, deduped: a profile can be bound on
		// several canvases and only one of them may be enabled, so the first is
		// the one a scheduled entry routes through.
		std::vector<std::string> wanted;
		for (const ScheduleDestination &d : destinations) {
			if (d.profileId.empty()) {
				continue;
			}
			for (const RoutingBinding &b : before) {
				if (b.profileId == d.profileId) {
					if (!Contains(wanted, b.uuid)) {
						wanted.push_back(b.uuid);
					}
					break;
				}
			}
		}

		// The enabled set becomes exactly what the entry names. Disabling runs
		// first: enabling a profile bound on another canvas would be refused by
		// the single-live-stream rule while the old binding still holds it.
		//
		// A refusal abandons the whole application rather than going live on a
		// half-applied routing, which would broadcast to destinations nobody
		// scheduled -- the outcome the exact-set rule exists to prevent.
		for (const RoutingBinding &b : before) {
			if (b.enabled && !Contains(wanted, b.uuid) && !Flip(b.uuid, false)) {
				reason = "a destination could not be taken off the air for this entry";
				Revert();
				return false;
			}
		}
		for (const std::string &uuid : wanted) {
			const RoutingBinding *b = Find(before, uuid);
			if (b && !b->enabled && !Flip(uuid, true)) {
				reason = "a destination could not be put on the air for this entry";
				Revert();
				return false;
			}
		}
	}

	// One capture per profile, not per destination: two destinations naming the
	// same profile would otherwise have the second capture record what the first
	// just wrote, and the user's own value would never come back.
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
		nlohmann::json existing = metadata.read ? metadata.read(d.profileId) : nlohmann::json::object();
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
		if (metadata.write) {
			metadata.write(d.profileId, merged);
		}
		overrides_.push_back(TouchedOverride{d.profileId, existing, merged, !existing.empty()});
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
		// Disables before enables, mirroring Apply: putting a binding back on
		// while the entry's binding still holds its profile is refused by the
		// single-live-stream rule, and that refusal would end with both off.
		for (const bool enabling : {false, true}) {
			const std::vector<RoutingBinding> current = routing.read();
			for (const TouchedBinding &touched : bindings_) {
				if (touched.before != enabling) {
					continue;
				}
				const RoutingBinding *now = Find(current, touched.uuid);
				// Per binding rather than over the set as a whole: an
				// unrelated change elsewhere must not strand every other
				// restore.
				if (!now || now->enabled != touched.after) {
					continue;
				}
				if (!routing.write(touched.uuid, touched.before)) {
					refused.push_back(touched);
				}
			}
		}
	}

	bool restoredAny = false;
	for (const TouchedOverride &touched : overrides_) {
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
