#pragma once

#include "StorePaths.hpp"

#include <cstddef>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

// Persists remembered stream metadata between sessions to stream_meta.json.
// Two maps of opaque field bags (the same shape the streamMeta.set `fields`
// object uses -- the store never interprets them):
//   - per-channel defaults keyed by accountId  ("channels")
//   - per-stream overrides keyed by profileUuid ("streams")
// Owned by the bootstrap. Like the other stores it has a trivial ctor and loads
// explicitly via Load() inside Start() -- after obs_startup and after portable
// config is applied -- so the on-disk path resolves correctly. Saved via
// SaveJsonAtomic.
class StreamMetaStore {
public:
	StreamMetaStore() = default;

	StreamMetaStore(const StreamMetaStore &) = delete;
	StreamMetaStore &operator=(const StreamMetaStore &) = delete;

	// Read stream_meta.json (if present) into the two maps. Call from Start().
	void Load();

	// The remembered field bag for a channel/stream, or an empty object if none.
	// Never throws.
	nlohmann::json ChannelDefaults(const std::string &accountId) const;
	nlohmann::json StreamOverride(const std::string &profileUuid) const;

	// Remember `fields` for a channel/stream (replaces any prior bag). Does NOT
	// persist -- callers Save() when they want it on disk.
	void PutChannelDefaults(const std::string &accountId, const nlohmann::json &fields);
	void PutStreamOverride(const std::string &profileUuid, const nlohmann::json &fields);

	// Forget a stream's override so it inherits the channel default again. Used
	// when a remembered override is toggled off on save; a no-op if none exists.
	void RemoveStreamOverride(const std::string &profileUuid);

	// Forget every stream override whose profile `isProfile` does not recognize, i.e. one
	// left behind by a deleted destination. Returns how many went. Does NOT persist.
	size_t PruneStreamOverrides(const std::function<bool(const std::string &profileUuid)> &isProfile);

	// Persist both maps to stream_meta.json via SaveJsonAtomic. Returns false on write
	// failure (already logged).
	bool Save() const;

	// Did the last Load find stream_meta.json on disk but unusable (kept aside)? The target
	// claims were in it, so a pass that would re-derive them (the boot target reconcile) waits
	// on this for the session; a save of the user's own change does not lift it.
	bool LoadedUnusable() const { return hold_.LoadedUnusable(); }

	// Whether saves still skip the untouched fallback: the file is unusable and the user has
	// not changed stream meta yet this session.
	bool HoldArmed() const { return hold_.Armed(); }

private:
	// Both maps as one string, for UnusableStoreHold.
	std::string Serialize() const;

	nlohmann::json channels_; // object of accountId  -> fields
	nlohmann::json streams_;  // object of profileUuid -> fields
	// Save is const, but the hold records whether a save has happened.
	mutable UnusableStoreHold hold_;
};
