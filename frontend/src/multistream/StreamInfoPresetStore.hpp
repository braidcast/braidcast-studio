#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The saved "stream info sheets" a streamer re-applies each broadcast: a capped,
// de-duplicated history of the metadata bag sets a go-live has run under, persisted to
// stream_info_presets.json.
//
// A SEPARATE store from StreamMetaStore rather than another map in stream_meta.json:
// that file holds the per-channel/per-stream defaults the dialog falls back to, and a
// corrupt presets file must not be able to cost the user those.
//
// A preset's identity is DERIVED from its bags, never stored -- OAuth::MetadataIdentity
// over the shared bag plus each provider's, providers in ascending id order. So going
// live twice with the same field set bumps one preset, while changing the title makes a
// second (both are wanted: title variants are experimented with on purpose and each must
// stay separately selectable). Fields outside that comparison -- thumbnail, latency, dvr
// -- do not fork a preset; they simply take their latest value on the one they matched.
// Nor does a value at rest (an empty list, a flag that is off) against its absence: a sheet
// saved before a field travelled in presets must stay the same preset as one saved after.
//
// UI-thread-only and unguarded, like the sibling multistream stores. Trivial ctor; Load()
// runs explicitly from the bootstrap, after obs_startup and after portable config is
// applied, so the on-disk path resolves correctly. Mutators do NOT persist -- callers
// Save() when they want it on disk.
class StreamInfoPresetStore {
public:
	// The most presets kept. Past it the least recently used row is dropped: this is a
	// handful of reusable sheets, not an archive of every broadcast.
	static constexpr size_t kMaxPresets = 20;

	StreamInfoPresetStore() = default;

	StreamInfoPresetStore(const StreamInfoPresetStore &) = delete;
	StreamInfoPresetStore &operator=(const StreamInfoPresetStore &) = delete;

	// Read stream_info_presets.json (if present) into memory. Call from Start().
	void Load();

	// Persist every preset via SaveJsonAtomic. Returns false on write failure (already
	// logged).
	bool Save() const;

	// Every preset as JSON, most recently used first.
	nlohmann::json List() const;

	// Per provider, the fields a row written before the preset format marker could never
	// hold: the channel-scoped ones, which did not travel in presets then.
	using LegacyUnheldFields = std::map<std::string, std::set<std::string>>;

	// Read from the provider descriptors, so only once the registry is booted -- which every
	// Remember caller is and Load() is not.
	static LegacyUnheldFields FieldsLegacyRowsNeverHeld();

	// Upsert `shared`/`byProvider`. A bag set whose identity matches one already held
	// keeps that preset's id, its creation stamp and the name the user gave it, takes the
	// incoming payload, and is marked used now; anything else becomes a new, unnamed
	// preset. Against a row written before the format marker, the row and the incoming sheet
	// are both compared without the `legacyUnheld` fields, which such a row could not have
	// held -- so re-saving it with the same visible values upgrades it rather than forking
	// it. An exact match anywhere wins over such a legacy match. `created` reports
	// which happened, and is answered by the store rather than by the attempt -- a new
	// preset the cap did not keep reports false. Returns the preset's id, or an empty string
	// when no row was kept to name.
	std::string Remember(const nlohmann::json &shared, const nlohmann::json &byProvider,
			     const LegacyUnheldFields &legacyUnheld, bool &created);

	// Mark `id` used now, for applying a preset outside a go-live. False when unknown.
	bool Touch(const std::string &id);

	bool Remove(const std::string &id);

	// Set `id`'s user-facing name. An empty name is valid -- it returns the row to the
	// UI's title fallback. False when unknown.
	bool Rename(const std::string &id, const std::string &name);

	// Headless smoke check of the identity rule, on throwaway stores that are never loaded
	// or saved: a sheet saved before channel fields travelled in presets and one saved after,
	// with the same visible values, must upsert one row -- for YouTube's made for kids,
	// Facebook's privacy and Twitch's language, read from the live descriptors -- as must one
	// that adds a provider bag holding nothing identifying, while a real difference forks one;
	// a legacy row holding a channel field is compared under the sheet's projection, and a
	// save equal to a current row lands on it rather than on a look-alike legacy row.
	// Logs "[selftest] stream info preset identity OK" or a FAILED line naming the case.
	static void RunIdentitySelfTest();

private:
	struct Preset {
		std::string id;
		std::string name;
		int64_t createdAtMs = 0;
		int64_t lastUsedAtMs = 0;
		nlohmann::json shared = nlohmann::json::object();
		nlohmann::json byProvider = nlohmann::json::object();
	};

	// The row holding `id`, or presets_.end(). The ONE lookup every mutator routes
	// through, so "which preset is that" is decided in a single place.
	std::vector<Preset>::iterator Find(const std::string &id);

	// Restore the invariant presets_ holds: ordered by last use, most recent first, and
	// never longer than kMaxPresets (MruRows::Normalize).
	void Normalize();

	// Drop rows a file written under an older identity rule split in two. Load-time only:
	// Remember compares each incoming sheet as it arrives, so a running store cannot grow
	// a duplicate. Runs AFTER Normalize, whose ordering decides which of a pair survives.
	void MergeDuplicates();

	// Ordered most recently used first (see Normalize), which is also the order List()
	// and the on-disk file carry.
	std::vector<Preset> presets_;
};
