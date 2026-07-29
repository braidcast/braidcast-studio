#ifndef OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_
#define OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_

#include <string>

// Give every target an account can stream to (StreamProvider::enumerateTargets --
// Facebook's Pages) its own stream profile, so profile -> output stays 1:1 and each
// target keeps its own ingest endpoint. Called from the connect flow once the account
// is stored: the enumeration is a blocking platform read, so this must run on a worker
// thread; the store writes it then makes marshal themselves to TID_UI.
//
// `originProfileUuid` is the profile the connect ran from -- the model the new profiles
// copy their service settings and canvas routing from, and the first claimant of a
// target. Additive only: a target already claimed is left alone and a claim on a target
// no longer granted is left standing, so reconnecting an account whose grants changed
// never duplicates or deletes a destination. See the .cpp for the full reconcile.
//
// Does nothing at all when the provider reports one target or none, which is every
// provider that does not implement enumerateTargets.
void MaterializeTargetDestinations(const std::string &accountId, const std::string &originProfileUuid);

#endif // OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_
