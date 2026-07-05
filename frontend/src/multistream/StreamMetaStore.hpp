#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Persists remembered stream metadata between sessions to stream_meta.json.
// Two maps of opaque field bags (the same shape the streamMeta.set `fields`
// object uses -- the store never interprets them):
//   - per-channel defaults keyed by accountId  ("channels")
//   - per-stream overrides keyed by profileUuid ("streams")
// Loaded in the ctor, saved via SaveJsonAtomic. Owned by the bootstrap for a
// clear lifecycle owner; no Start/Stop hook needed (it loads on construction).
class StreamMetaStore {
public:
	StreamMetaStore(); // loads stream_meta.json if present

	StreamMetaStore(const StreamMetaStore &) = delete;
	StreamMetaStore &operator=(const StreamMetaStore &) = delete;

	// The remembered field bag for a channel/stream, or an empty object if none.
	// Never throws.
	nlohmann::json ChannelDefaults(const std::string &accountId) const;
	nlohmann::json StreamOverride(const std::string &profileUuid) const;

	// Remember `fields` for a channel/stream (replaces any prior bag). Does NOT
	// persist -- callers Save() when they want it on disk.
	void PutChannelDefaults(const std::string &accountId, const nlohmann::json &fields);
	void PutStreamOverride(const std::string &profileUuid, const nlohmann::json &fields);

	// Persist both maps to stream_meta.json via SaveJsonAtomic.
	void Save() const;

private:
	nlohmann::json channels_; // object of accountId  -> fields
	nlohmann::json streams_;  // object of profileUuid -> fields
};
