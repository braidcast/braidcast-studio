#pragma once

#include "StreamProfile.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Owns the global list of stream profiles for the new (non-Qt) frontend,
// persisted to the SAME standalone streams.json the legacy frontend uses
// (<config>/braidcast/basic/streams.json). De-Qt'd port of the legacy
// StreamProfileManager; FilePath() resolves the config dir via libobs
// os_get_config_path instead of OBSApp.
//
// When any profiles exist, exactly one is isPrimary == true (re-pointed on Load
// and on primary removal).
class StreamProfileStore {
public:
	void Load(); // read streams.json (replaces contents; re-points primary if missing)
	void Save() const;

	// The whole model as JSON, in the SAME shape streams.json holds (the single
	// serializer; Load/Save route through it). FromJson replaces contents and
	// re-points the primary if missing, mirroring Load(). Used by settings.snapshot/
	// settings.restore for the transactional Settings footer.
	nlohmann::json ToJson() const;
	void FromJson(const nlohmann::json &j);

	const std::vector<StreamProfile> &Profiles() const { return profiles; }
	std::vector<StreamProfile> &AllMutable() { return profiles; } // in-place edits (e.g. unlink an account)
	bool Empty() const { return profiles.empty(); }
	StreamProfile *Primary(); // the isPrimary profile, or nullptr if none yet
	StreamProfile *Find(const std::string &uuid);

	// True if any profile links the given OAuth account (providerId:userId). An empty
	// accountId matches nothing (key/RTMP/WHIP profiles carry no account). The single
	// "is this account still owned?" query -- used by the disconnect/profile-delete
	// cleanup and the boot orphan reconcile.
	bool ReferencesAccount(const std::string &accountId) const;
	StreamProfile &Add(StreamProfile p);  // assigns uuid if empty; first add becomes primary
	void Remove(const std::string &uuid); // re-points primary if the primary was removed
	bool SetPrimary(const std::string &uuid); // marks uuid primary, clears the rest; false if not found

	// Drop all profiles (releasing their obs_data) for teardown leak measurement.
	void Clear() { profiles.clear(); }

	// <config>/braidcast/basic/streams.json -- identical to the legacy path.
	static std::string FilePath();

private:
	std::vector<StreamProfile> profiles;
};
