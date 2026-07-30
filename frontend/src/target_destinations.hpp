#ifndef OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_
#define OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_

#include <string>

#include <nlohmann/json.hpp>

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

// The same reconcile for every account already connected at launch. A target granted
// after the account was connected -- a Page added to the person's Facebook account since
// -- is otherwise invisible until they disconnect and reconnect, the connect flow being
// the only thing that runs the reconcile. Enumeration blocks on the platform, so the pass
// is handed to a worker and boot never waits on it.
//
// Call once from bootstrap on the UI thread, after the profile and account stores load
// and the provider registry is populated. Inert under a smoke/self-test run: those drive
// the app unattended against the user's real config directory, and this pass writes
// stream profiles.
void MaterializeTargetDestinationsAtBoot();

// The target one destination claims, as `{"id","name","avatarUrl"}` with empty strings
// for whatever is absent -- an unclaimed profile, an account with no provider, a provider
// with no targets. Absence is a real answer here, never an error.
//
// Exported so the bridge reports a destination's identity without a second reader of the
// same bag: this module owns the claim, writes it in the reconcile, and is where the field
// key is resolved. A local read, safe to call per row.
//
// UI thread only, like every StreamMetaStore access.
nlohmann::json ClaimedTargetOf(const std::string &profileUuid);

// Point one destination at `targetId`, refusing a target another destination of the same
// account already claims -- two profiles on one Page would create two live videos there
// and fight over one ingest. `name` and `avatarUrl` are cached alongside so the row can
// render without a platform call.
//
// False + `err` on: unknown profile, no linked account, a provider with no targets, or a
// target already taken. Saves and emits streamProfile.changed on success.
//
// UI thread only.
bool ClaimTargetForProfile(const std::string &profileUuid, const std::string &targetId, const std::string &name,
			   const std::string &avatarUrl, std::string &err);

// Hand one account's provider the set of targets its destinations claim, replacing whatever
// the provider held for that account (StreamProvider::noteTargetClaims). The claim lives in a
// store only the UI thread may touch, so a provider whose reads run on a poll worker -- the
// audience poller's Facebook Page totals -- cannot resolve a destination without this.
//
// Call after ANY change to what a destination claims: a claim written or cleared, a
// destination deleted. The set is replaced wholesale, so a removal needs no counterpart here.
// Reads only; safe under a smoke/self-test run, unlike the reconcile above.
//
// UI thread only.
void PublishTargetClaims(const std::string &accountId);

// The same for every connected account, once at boot. Not folded into
// MaterializeTargetDestinationsAtBoot: that pass only ever writes claims for targets NOTHING
// yet claims, so a profile that claimed its Page in an earlier session goes through it
// untouched -- and it is skipped entirely when the account is offline or its enumeration
// fails. Relying on it would leave the mirror cold across a relaunch.
//
// UI thread only. Must run before the audience poller starts.
void PublishAllTargetClaims();

#endif // OBS_MULTISTREAM_FRONTEND_TARGET_DESTINATIONS_HPP_
