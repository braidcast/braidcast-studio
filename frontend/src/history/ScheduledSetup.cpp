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
	if (!destination.category.empty() || !destination.categoryId.empty()) {
		fields["category"] = nlohmann::json{{"id", destination.categoryId}, {"name", destination.category}};
	}
	const nlohmann::json tags = JsonUtil::ParseJson(destination.tags);
	if (tags.is_array() && !tags.empty()) {
		fields["tags"] = tags;
	}
	return fields;
}

void ScheduledSetup::Flip(const std::string &uuid, bool enabled)
{
	bindings_.push_back(TouchedBinding{uuid, !enabled, enabled});
	routing.write(uuid, enabled);
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
		for (const RoutingBinding &b : before) {
			if (b.enabled && !Contains(wanted, b.uuid)) {
				Flip(b.uuid, false);
			}
		}
		for (const std::string &uuid : wanted) {
			const RoutingBinding *b = Find(before, uuid);
			if (b && !b->enabled) {
				Flip(uuid, true);
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
		nlohmann::json merged = existing;
		for (auto it = fields.begin(); it != fields.end(); ++it) {
			merged[it.key()] = it.value();
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
	if (!applied_) {
		return;
	}
	if (isStreaming && isStreaming()) {
		return;
	}

	if (routing.read && routing.write) {
		const std::vector<RoutingBinding> current = routing.read();
		for (const TouchedBinding &touched : bindings_) {
			const RoutingBinding *now = Find(current, touched.uuid);
			// Per binding rather than over the set as a whole: an unrelated
			// change elsewhere must not strand every other restore.
			if (now && now->enabled == touched.after) {
				routing.write(touched.uuid, touched.before);
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

	if (log) {
		log("[schedule] put back the routing and metadata the scheduled entry changed");
	}
	applied_ = false;
	bindings_.clear();
	overrides_.clear();
}

} // namespace History
