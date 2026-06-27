#pragma once

#include "StreamProfile.hpp"

#include <string>
#include <vector>

/* Owns the global list of stream profiles, persisted to a standalone
 * streams.json (parallel to canvases.json). Loaded once at startup; survives
 * profile and scene-collection switches. Always contains exactly one
 * isPrimary == true profile (seeded on first run from the existing single
 * Stream1 service — see SeedFromLegacyService). */
class StreamProfileManager {
public:
	void Load(); // read streams.json (replaces contents; does NOT seed primary)
	void Save() const;

	const std::vector<StreamProfile> &Profiles() const { return profiles; }
	bool Empty() const { return profiles.empty(); }
	StreamProfile *Primary(); // the isPrimary profile, or nullptr if none yet
	StreamProfile *Find(const std::string &uuid);
	StreamProfile &Add(StreamProfile p);  // assigns uuid if empty; first add becomes primary
	void Remove(const std::string &uuid); // re-points primary if the primary was removed

private:
	static std::string FilePath(); // <userProfilesLocation>/obs-multistream/basic/streams.json
	std::vector<StreamProfile> profiles;
};
