#include "StreamInfoPresetStore.hpp"

#include "MruRows.hpp"
#include "StorePaths.hpp"

#include "log.hpp"
#include "oauth/provider.hpp"
#include "oauth/registry.hpp"
#include "util/json_util.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"

#include <uuid_util.hpp>

#include <util/platform.h>

#include <algorithm>
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
// A bag with every value at rest dropped: an empty list, and a flag that is off.
// MetadataIdentity deliberately tells an ABSENT key from one carrying such a value: there, an
// empty list is the assertion "no tags" and `false` the assertion "not made for kids", while an
// absent key means the provider could not read the field at all, and the difference decides
// whether a go-live is reported as diverging. A SAVED SHEET has no such distinction to make --
// both say the streamer set nothing -- and keeping it forks one stream into two presets whose
// every visible field matches: one go-live sent no `tags` key and the next sent `tags: []`, and
// a sheet saved before made-for-kids travelled in presets holds no `madeForKids` where every
// later one holds the `false` the dialog seeds. Applied here rather than inside MetadataIdentity
// so the divergence check keeps the distinction it needs.
//
// Off is the resting value of EVERY flag the identity reads: made for kids declares false as
// its default (youtube_provider.cpp) and branded content declares none, which the dialog shows
// as off. A flag whose default is on would have to be read against its descriptor instead --
// and the descriptors cannot be consulted here, because Load() runs before the provider
// registry is populated (obs_bootstrap.cpp) and an identity must not change between the load
// that merges rows and the go-live that upserts one.
json WithoutRestingValues(const json &bag)
{
	if (!bag.is_object()) {
		return bag;
	}
	json out = json::object();
	for (const auto &entry : bag.items()) {
		const json &value = entry.value();
		if ((value.is_array() && value.empty()) || (value.is_boolean() && !value.get<bool>())) {
			continue;
		}
		out[entry.key()] = value;
	}
	return out;
}

// The provider-bag key the frontend writes its preset format under (PRESET_FORMAT_KEY in
// applyPreset.ts). A row none of whose bags carries it was written before channel-scoped
// fields travelled in presets.
constexpr const char *kPresetFormatKey = "__v";

bool IsLegacyRow(const json &byProvider)
{
	if (!byProvider.is_object()) {
		return true;
	}
	for (const auto &entry : byProvider.items()) {
		if (entry.value().is_object() && entry.value().contains(kPresetFormatKey)) {
			return false;
		}
	}
	return true;
}

// `byProvider` as a legacy row could have held it: every field such a row never carried taken
// out, so a value it could not have stated cannot make it look different.
json AsLegacyRowWouldHold(const json &byProvider, const StreamInfoPresetStore::LegacyUnheldFields &legacyUnheld)
{
	json out = byProvider;
	if (!out.is_object()) {
		return out;
	}
	for (auto &entry : out.items()) {
		const auto unheld = legacyUnheld.find(entry.key());
		if (unheld == legacyUnheld.end() || !entry.value().is_object()) {
			continue;
		}
		for (const std::string &key : unheld->second) {
			entry.value().erase(key);
		}
	}
	return out;
}

std::string PresetIdentity(const json &shared, const json &byProvider)
{
	std::string identity;
	StringUtil::AppendLengthPrefixed(identity, OAuth::MetadataIdentity(WithoutRestingValues(shared)));
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
		// A bag with nothing identifying left -- the format marker alone, or only values at
		// rest -- is a provider that was armed and said nothing, which must not fork the sheet.
		const std::string bagIdentity =
			OAuth::MetadataIdentity(WithoutRestingValues(byProvider.at(providerId)));
		if (bagIdentity.empty()) {
			continue;
		}
		StringUtil::AppendLengthPrefixed(identity, providerId);
		StringUtil::AppendLengthPrefixed(identity, bagIdentity);
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
		preset.createdAtMs = MruRows::ReadTimestamp(item, "createdAtMs", now);
		preset.lastUsedAtMs = MruRows::ReadTimestamp(item, "lastUsedAtMs", now);
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

auto StreamInfoPresetStore::FieldsLegacyRowsNeverHeld() -> LegacyUnheldFields
{
	LegacyUnheldFields out;
	for (OAuth::StreamProvider *provider : OAuth::Registry().All()) {
		try {
			const json cap = provider->capabilityJson();
			const json fields = cap.value("fields", json::array());
			for (const json &field : fields) {
				// Channel scope read exactly as the dialog reads it (fieldScope in
				// fieldValue.ts): anything but "all" or "provider", a missing scope included.
				const std::string scope = JsonUtil::Str(field, "scope");
				if (field.is_object() && scope != "all" && scope != "provider") {
					out[provider->id()].insert(JsonUtil::Str(field, "key"));
				}
			}
		} catch (const std::exception &e) {
			HostLog(std::string("[storage] stream info preset: capabilityJson failed: ") + e.what());
		}
	}
	return out;
}

std::string StreamInfoPresetStore::Remember(const json &shared, const json &byProvider,
					    const LegacyUnheldFields &legacyUnheld, bool &created)
{
	const std::string incoming = PresetIdentity(shared, byProvider);
	const std::string incomingAsLegacy = PresetIdentity(shared, AsLegacyRowWouldHold(byProvider, legacyUnheld));
	const int64_t usedNow = MruRows::UsedNowMs(presets_);

	// An exact match wins over a legacy one wherever the two sit in MRU order: otherwise a
	// save equal to a current row would overwrite an older legacy row that merely looks like
	// it, leaving two rows holding the same sheet. A legacy row is compared under the same
	// projection as the sheet, with the fields such a row never carried taken out of both.
	auto hit = std::find_if(presets_.begin(), presets_.end(), [&](const Preset &preset) {
		return PresetIdentity(preset.shared, preset.byProvider) == incoming;
	});
	if (hit == presets_.end()) {
		hit = std::find_if(presets_.begin(), presets_.end(), [&](const Preset &preset) {
			return IsLegacyRow(preset.byProvider) &&
			       PresetIdentity(preset.shared, AsLegacyRowWouldHold(preset.byProvider, legacyUnheld)) ==
				       incomingAsLegacy;
		});
	}
	if (hit != presets_.end()) {
		Preset &preset = *hit;
		// The identity fields already agree, so this overwrite can only move the fields
		// identity ignores -- thumbnail, latency, dvr, autoStop, projection, a value at rest
		// -- to their latest value, or give a legacy row the channel fields it never held.
		// That is the point: those must not fork a second sheet.
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
	it->lastUsedAtMs = MruRows::UsedNowMs(presets_);
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
	return MruRows::FindById(presets_, id);
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
		"omitted a value the other sent at rest");
}

void StreamInfoPresetStore::Normalize()
{
	MruRows::Normalize(presets_, kMaxPresets);
}

void StreamInfoPresetStore::RunIdentitySelfTest()
{
	// The live descriptors, as the bridge reads them for every save: the cases below depend on
	// them marking these fields channel-scoped, so a descriptor change fails here first.
	const LegacyUnheldFields legacyUnheld = FieldsLegacyRowsNeverHeld();
	const auto unheld = [&](const char *provider, const char *key) {
		const auto it = legacyUnheld.find(provider);
		return it != legacyUnheld.end() && it->second.count(key) != 0;
	};

	// The shape every preset saved before this change has: YouTube only, no made-for-kids key.
	const json shared = json{{"title", "Spider-Man 2"}, {"description", "Max graphics"}};
	const json oldYouTube = json{{"category", json{{"id", "20"}, {"name", "Gaming"}}},
				     {"privacy", "public"},
				     {"tags", json::array()},
				     {"thumbnail", "C:/thumb.png"}};
	// The same visible sheet saved now: the format marker, and the channel fields at rest.
	json newYouTube = oldYouTube;
	newYouTube[kPresetFormatKey] = 2;
	newYouTube["madeForKids"] = false;
	newYouTube["autoStop"] = true;
	newYouTube["latency"] = "normal";
	json kidsYouTube = newYouTube;
	kidsYouTube["madeForKids"] = true;
	// A provider that was armed and said nothing: the marker, and values at rest.
	const json silentTwitch = json{{kPresetFormatKey, 2}, {"tags", json::array()}, {"brandedContent", false}};

	StreamInfoPresetStore store;
	bool created = false;
	std::string failure;
	if (!unheld("youtube", "madeForKids") || !unheld("facebook", "privacy") || !unheld("twitch", "language")) {
		failure =
			"the descriptors do not mark made for kids, Facebook privacy and Twitch language channel-scoped";
	}
	if (failure.empty()) {
		store.Remember(shared, json{{"youtube", oldYouTube}}, legacyUnheld, created);
		if (!created) {
			failure = "the first sheet was not kept as a new preset";
		}
	}
	if (failure.empty()) {
		store.Remember(shared, json{{"youtube", newYouTube}}, legacyUnheld, created);
		if (created || store.presets_.size() != 1) {
			failure = "an old sheet and the same sheet saved now made two presets";
		}
	}
	if (failure.empty()) {
		store.Remember(shared, json{{"youtube", kidsYouTube}}, legacyUnheld, created);
		if (!created || store.presets_.size() != 2) {
			failure = "made for kids on did not make a preset of its own";
		}
	}
	if (failure.empty()) {
		store.Remember(shared, json{{"youtube", newYouTube}, {"twitch", silentTwitch}}, legacyUnheld, created);
		if (created || store.presets_.size() != 2) {
			failure = "a provider bag holding nothing identifying made a preset of its own";
		}
	}

	// Facebook armed at a legacy save held nothing of its own, so the row has no bag for it;
	// every save now carries its privacy, which rests at public.
	const json facebookNow = json{{kPresetFormatKey, 2}, {"privacy", "public"}};
	json facebookCategory = facebookNow;
	facebookCategory["category"] = json{{"id", "6003"}, {"name", "Video games"}};
	StreamInfoPresetStore facebookStore;
	if (failure.empty()) {
		facebookStore.Remember(shared, json{{"youtube", oldYouTube}}, legacyUnheld, created);
		facebookStore.Remember(shared, json{{"youtube", newYouTube}, {"facebook", facebookNow}}, legacyUnheld,
				       created);
		if (created || facebookStore.presets_.size() != 1) {
			failure = "a legacy sheet re-saved with Facebook's privacy made two presets";
		}
	}
	if (failure.empty()) {
		facebookStore.Remember(shared, json{{"youtube", newYouTube}, {"facebook", facebookCategory}},
				       legacyUnheld, created);
		if (!created || facebookStore.presets_.size() != 2) {
			failure = "a Facebook category did not make a preset of its own";
		}
	}

	// Twitch's language is prefilled from the channel, so every save now carries one.
	const json oldTwitch = json{{"category", json{{"id", "509658"}, {"name", "Just Chatting"}}}};
	json twitchNow = oldTwitch;
	twitchNow[kPresetFormatKey] = 2;
	twitchNow["language"] = "en";
	twitchNow["tags"] = json::array();
	twitchNow["brandedContent"] = false;
	json twitchOtherCategory = twitchNow;
	twitchOtherCategory["category"] = json{{"id", "33214"}, {"name", "Fortnite"}};
	StreamInfoPresetStore twitchStore;
	if (failure.empty()) {
		twitchStore.Remember(shared, json{{"twitch", oldTwitch}}, legacyUnheld, created);
		twitchStore.Remember(shared, json{{"twitch", twitchNow}}, legacyUnheld, created);
		if (created || twitchStore.presets_.size() != 1) {
			failure = "a legacy sheet re-saved with Twitch's language made two presets";
		}
	}
	// Against a legacy row too, not only the upgraded one: leaving the channel fields out of
	// the comparison must not let a real difference through.
	StreamInfoPresetStore twitchLegacyStore;
	if (failure.empty()) {
		twitchLegacyStore.Remember(shared, json{{"twitch", oldTwitch}}, legacyUnheld, created);
		twitchLegacyStore.Remember(shared, json{{"twitch", twitchOtherCategory}}, legacyUnheld, created);
		if (!created || twitchLegacyStore.presets_.size() != 2) {
			failure = "a different Twitch category merged into a legacy preset";
		}
	}

	// A legacy row holding a channel field anyway (hand-edited, or written by a foreign
	// build) is compared under the same projection as the sheet, so the field cannot keep
	// the two apart on one side only.
	json legacyWithKids = oldYouTube;
	legacyWithKids["madeForKids"] = true;
	StreamInfoPresetStore projectionStore;
	if (failure.empty()) {
		projectionStore.Remember(shared, json{{"youtube", legacyWithKids}}, legacyUnheld, created);
		projectionStore.Remember(shared, json{{"youtube", newYouTube}}, legacyUnheld, created);
		if (created || projectionStore.presets_.size() != 1) {
			failure = "a legacy row holding a channel field was compared with it on one side only";
		}
	}

	// A current row N and a more recently used legacy row L that look alike: a save equal to N
	// must land on N, not overwrite L into a second copy of it.
	StreamInfoPresetStore exactFirstStore;
	const json currentSheet = json{{"youtube", newYouTube}, {"facebook", facebookNow}};
	if (failure.empty()) {
		const std::string current = exactFirstStore.Remember(shared, currentSheet, legacyUnheld, created);
		exactFirstStore.Remember(shared, json{{"youtube", oldYouTube}}, legacyUnheld, created);
		const bool legacyAhead = exactFirstStore.presets_.size() == 2 &&
					 IsLegacyRow(exactFirstStore.presets_.front().byProvider);
		const std::string landed = exactFirstStore.Remember(shared, currentSheet, legacyUnheld, created);
		if (!legacyAhead || created || landed != current || exactFirstStore.presets_.size() != 2 ||
		    exactFirstStore.presets_.front().id != current ||
		    !IsLegacyRow(exactFirstStore.presets_.back().byProvider)) {
			failure = "a save equal to a current row overwrote a legacy row that looked like it";
		}
	}
	HostLog(failure.empty() ? std::string("[selftest] stream info preset identity OK")
				: "[selftest] stream info preset identity FAILED: " + failure);
}
