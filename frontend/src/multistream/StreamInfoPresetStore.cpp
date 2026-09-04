#include "StreamInfoPresetStore.hpp"

#include "StorePaths.hpp"

#include "log.hpp"
#include "oauth/provider.hpp"
#include "util/json_util.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"

#include <uuid_util.hpp>

#include <util/platform.h>

#include <algorithm>
#include <limits>
#include <utility>

using json = nlohmann::json;

namespace {

// Bumped only when the on-disk shape changes in a way this build could misread. There is
// no v0, so nothing migrates today; the field exists so a future change has somewhere to
// stand.
constexpr int kStoreVersion = 1;

std::string FilePath()
{
	return MultistreamBasicPath("stream_info_presets.json");
}

// An epoch-ms field, falling back to `fallback` for a missing, non-numeric or non-positive
// value. Zero is refused along with garbage: a hand-edited row carrying one would be the
// next eviction victim regardless of how recently it was really used. The upper clamp is
// what keeps UsedNowMs()'s `front().lastUsedAtMs + 1` from overflowing on a document
// carrying INT64_MAX.
int64_t ReadTimestamp(const json &item, const char *key, int64_t fallback)
{
	constexpr int64_t kMaxStampMs = std::numeric_limits<int64_t>::max() / 2;
	const int64_t value = JsonUtil::NumLoose(item, key, fallback);
	return value > 0 ? std::min(value, kMaxStampMs) : fallback;
}

// A metadata bag as it was persisted: the stringified JSON object Save writes (see the note
// there), or a plain object from a hand-written document. An absent key is an empty bag, which
// is a real answer -- the row simply asserts nothing on that side.
//
// Returns false when the key IS there but cannot be read as a bag, which makes it the caller's
// job to drop the whole row. Substituting an empty bag would be worse than losing the preset:
// the surviving row would still apply, silently missing whatever the unreadable half held --
// including `privacy` and `madeForKids`, which decide who can see the broadcast.
bool ReadBag(const json &item, const char *key, json &out)
{
	const json &value = JsonUtil::Obj(item, key);
	if (value.is_null()) {
		out = json::object();
		return true;
	}
	if (value.is_object()) {
		out = value;
		return true;
	}
	if (!value.is_string()) {
		return false;
	}
	// Tolerant parse: a truncated or hand-mangled blob yields a discarded value rather than
	// throwing, so one bad row can never abort the load.
	const json parsed = JsonUtil::ParseJson(value.get<std::string>());
	if (!parsed.is_object()) {
		return false;
	}
	out = parsed;
	return true;
}

// What tells one preset from another: the shared bag's identity, then each provider's id and
// identity, with providers walked in ASCENDING id order. The order is stated rather than
// inherited from how the JSON type happens to iterate, because the identity of a saved sheet
// must not depend on that. Every part goes in length-prefixed via
// StringUtil::AppendLengthPrefixed -- the same construction MetadataIdentity uses inside one
// bag, and here for the same reason: a provider id and a bag identity are both free to hold
// any byte, so joining them on a delimiter would let one of them forge a provider boundary
// and make a one-provider sheet read alike to a two-provider one.
// A bag with every empty list dropped. MetadataIdentity deliberately tells an ABSENT key
// from one carrying an empty list: there, an empty list is the assertion "no tags" while an
// absent key means the provider could not read the field at all, and the difference decides
// whether a go-live is reported as diverging. A SAVED SHEET has no such distinction to make
// -- both say the streamer set no tags -- and keeping it forked one stream into two presets
// whose every visible field matched, because one go-live sent no `tags` key and the next
// sent `tags: []`. Applied here rather than inside MetadataIdentity so the divergence check
// keeps the distinction it needs.
json WithoutEmptyLists(const json &bag)
{
	if (!bag.is_object()) {
		return bag;
	}
	json out = json::object();
	for (const auto &entry : bag.items()) {
		if (entry.value().is_array() && entry.value().empty()) {
			continue;
		}
		out[entry.key()] = entry.value();
	}
	return out;
}

std::string PresetIdentity(const json &shared, const json &byProvider)
{
	std::string identity;
	StringUtil::AppendLengthPrefixed(identity, OAuth::MetadataIdentity(WithoutEmptyLists(shared)));
	if (!byProvider.is_object()) {
		return identity;
	}
	std::vector<std::string> providerIds;
	providerIds.reserve(byProvider.size());
	for (const auto &entry : byProvider.items()) {
		providerIds.push_back(entry.key());
	}
	std::sort(providerIds.begin(), providerIds.end());
	for (const std::string &providerId : providerIds) {
		StringUtil::AppendLengthPrefixed(identity, providerId);
		StringUtil::AppendLengthPrefixed(identity,
						 OAuth::MetadataIdentity(WithoutEmptyLists(byProvider.at(providerId))));
	}
	return identity;
}

} // namespace

void StreamInfoPresetStore::Load()
{
	presets_.clear();

	const std::string path = FilePath();
	const json root = LoadStoreJson(path);
	const json &stored = JsonUtil::Obj(root, "presets");
	if (!stored.is_array()) {
		// LoadStoreJson answers with an empty object for a file that is simply not there
		// (and falls back to the ".bak" copy for a truncated write), so only a file that
		// exists yet yielded no preset list has actually lost anything.
		if (os_file_exists(path.c_str())) {
			HostLog("[storage] stream_info_presets.json unreadable or malformed; the saved stream "
				"info presets it held are gone");
		}
		return;
	}

	const int version = static_cast<int>(JsonUtil::NumLoose(root, "version", kStoreVersion));
	if (version > kStoreVersion) {
		// Written by a newer build. Loading what parses keeps the presets this build can
		// still read, rather than starting a downgraded install empty.
		HostLog("[storage] stream_info_presets.json is version " + std::to_string(version) +
			" but this build reads v" + std::to_string(kStoreVersion) + "; loading what it can");
	}

	const int64_t now = TimeUtil::NowMs();
	for (const json &item : stored) {
		// One unusable row must not cost the other nineteen, so a bad entry is skipped
		// rather than discarding the file.
		if (!item.is_object()) {
			continue;
		}
		Preset preset;
		preset.id = JsonUtil::Str(item, "id");
		if (preset.id.empty()) {
			continue;
		}
		if (!ReadBag(item, "shared", preset.shared) || !ReadBag(item, "byProvider", preset.byProvider)) {
			continue;
		}
		preset.name = JsonUtil::Str(item, "name");
		preset.createdAtMs = ReadTimestamp(item, "createdAtMs", now);
		preset.lastUsedAtMs = ReadTimestamp(item, "lastUsedAtMs", now);
		presets_.push_back(std::move(preset));
	}
	// Neither the file's length nor its order is trusted: a hand-edited or newer-build
	// document can carry more rows than the cap, and in any order at all.
	Normalize();
	MergeDuplicates();
}

bool StreamInfoPresetStore::Save() const
{
	// The rows are what List() renders, with ONE difference: the two opaque bags are carried
	// as stringified JSON rather than as nested objects. A BAG CARRYING A LIST MUST NOT GO
	// THROUGH obs_data NESTED, and that is a hidden constraint of the save seam, not a style
	// choice here.
	//
	// SaveStoreJson routes through obs_data, and an obs_data array holds OBJECTS ONLY: every
	// scalar element of a JSON array is dropped on the way in -- obs_data_add_json_array skips
	// any element that is not a json object (libobs/obs-data.c:473-474). A metadata bag's
	// `tags` and `contentLabels` are arrays of strings, and both are among the nine fields a
	// preset's identity is built from, so a nested bag would come back with those lists emptied:
	// every preset differing only by its tags would collapse into one, and the tags a saved
	// sheet exists to carry would be gone. A string survives verbatim, which is the same reason
	// StreamMetaStore's file holds blobs.
	json rows = List();
	for (json &row : rows) {
		row["shared"] = row["shared"].dump();
		row["byProvider"] = row["byProvider"].dump();
	}
	return SaveStoreJson(json{{"version", kStoreVersion}, {"presets", std::move(rows)}}, FilePath());
}

json StreamInfoPresetStore::List() const
{
	json out = json::array();
	for (const Preset &preset : presets_) {
		out.push_back(json{{"id", preset.id},
				   {"name", preset.name},
				   {"createdAtMs", preset.createdAtMs},
				   {"lastUsedAtMs", preset.lastUsedAtMs},
				   {"shared", preset.shared},
				   {"byProvider", preset.byProvider}});
	}
	return out;
}

std::string StreamInfoPresetStore::Remember(const json &shared, const json &byProvider, bool &created)
{
	const std::string incoming = PresetIdentity(shared, byProvider);
	const int64_t usedNow = UsedNowMs();

	for (Preset &preset : presets_) {
		if (PresetIdentity(preset.shared, preset.byProvider) != incoming) {
			continue;
		}
		// The identity fields already agree, so this overwrite can only move the fields
		// identity ignores -- thumbnail, latency, dvr, autoStop, projection -- to their
		// latest value. That is the point: those must not fork a second sheet.
		preset.shared = shared;
		preset.byProvider = byProvider;
		preset.lastUsedAtMs = usedNow;
		created = false;
		const std::string id = preset.id;
		Normalize();
		return id;
	}

	Preset fresh;
	fresh.id = UuidUtil::New();
	// The creation date is the wall clock unclamped: nothing orders by it, so it has no
	// reason to trade accuracy for monotonicity the way lastUsedAtMs does.
	fresh.createdAtMs = TimeUtil::NowMs();
	fresh.lastUsedAtMs = usedNow;
	fresh.shared = shared;
	fresh.byProvider = byProvider;
	const std::string id = fresh.id;
	presets_.push_back(std::move(fresh));
	Normalize();
	// Normalize() decides what survives the cap, so `created` is read back out of the store
	// rather than assumed: an id the caller is told was created but that the store does not
	// hold would fail every later touch/rename with "no such preset", and the caller would
	// have reported a save that kept nothing.
	created = Find(id) != presets_.end();
	return created ? id : std::string();
}

bool StreamInfoPresetStore::Touch(const std::string &id)
{
	const auto it = Find(id);
	if (it == presets_.end()) {
		return false;
	}
	it->lastUsedAtMs = UsedNowMs();
	Normalize();
	return true;
}

bool StreamInfoPresetStore::Remove(const std::string &id)
{
	const auto it = Find(id);
	if (it == presets_.end()) {
		return false;
	}
	presets_.erase(it);
	return true;
}

bool StreamInfoPresetStore::Rename(const std::string &id, const std::string &name)
{
	const auto it = Find(id);
	if (it == presets_.end()) {
		return false;
	}
	// Deliberately does not bump lastUsedAtMs: naming a sheet is not using it, and letting
	// it count as use would let housekeeping reorder the list under the user.
	it->name = name;
	return true;
}

auto StreamInfoPresetStore::Find(const std::string &id) -> std::vector<Preset>::iterator
{
	return std::find_if(presets_.begin(), presets_.end(), [&id](const Preset &p) { return p.id == id; });
}

int64_t StreamInfoPresetStore::UsedNowMs() const
{
	const int64_t now = TimeUtil::NowMs();
	if (presets_.empty()) {
		return now;
	}
	// Normalize leaves presets_ ordered most recent first, and no mutator leaves it
	// otherwise: Remove only erases, which preserves the order, and Rename touches no
	// stamp. So front() holds the largest stamp in the store whenever a caller reaches
	// here.
	return std::max(now, presets_.front().lastUsedAtMs + 1);
}

void StreamInfoPresetStore::MergeDuplicates()
{
	// Rows the identity rule would refuse to create today, but that a file written before it
	// can still hold. The store's whole contract is a de-duplicated history, so restoring
	// that invariant on load is not a new behavior -- without it a user carries the split
	// pair forever, since Remember only ever compares what a go-live brings in.
	//
	// Normalize has already ordered by last use, so the row kept is the one used most
	// recently; it inherits nothing from the row it absorbs beyond staying where it is. A
	// name the user typed on the dropped row would be lost, so a NAMED row is never dropped.
	std::vector<std::string> seen;
	std::vector<Preset> kept;
	kept.reserve(presets_.size());
	size_t merged = 0;
	for (Preset &preset : presets_) {
		const std::string identity = PresetIdentity(preset.shared, preset.byProvider);
		if (!preset.name.empty() || std::find(seen.begin(), seen.end(), identity) == seen.end()) {
			seen.push_back(identity);
			kept.push_back(std::move(preset));
			continue;
		}
		merged++;
	}
	// Unconditional, and load-bearing rather than tidy: the loop above moved every row
	// out of presets_, so a return that skipped this would leave the store holding
	// moved-from Presets -- empty ids and null bags -- which List() then hands to the UI.
	presets_ = std::move(kept);
	if (merged == 0) {
		return;
	}
	HostLog("[storage] merged " + std::to_string(merged) +
		" duplicate stream info preset(s): same sheet, saved twice because one go-live "
		"omitted a list field the other sent empty");
}

void StreamInfoPresetStore::Normalize()
{
	// Stable, so two rows stamped in the same millisecond keep a deterministic order.
	std::stable_sort(presets_.begin(), presets_.end(),
			 [](const Preset &a, const Preset &b) { return a.lastUsedAtMs > b.lastUsedAtMs; });
	// Eviction takes the tail, which is the row used longest ago -- never the oldest by
	// creation. A sheet made months back but applied every broadcast has to survive a burst
	// of one-off experiments.
	if (presets_.size() > kMaxPresets) {
		presets_.resize(kMaxPresets);
	}
}
