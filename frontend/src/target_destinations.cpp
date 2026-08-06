#include "target_destinations.hpp"

#include "event_names.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <util/dstr.h>

#include "util/async_task.hpp"
#include "util/env_config.hpp"
#include "util/string_util.hpp"
#include "bridge.hpp"
#include "log.hpp"
#include "multistream/CanvasRuntime.hpp"
#include "multistream/OutputBindingStore.hpp"
#include "multistream/StreamMetaStore.hpp"
#include "multistream/StreamProfileStore.hpp"
#include "oauth/account_store.hpp"
#include "oauth/registry.hpp"
#include "obs_bootstrap.hpp"

using json = nlohmann::json;

namespace {

// The target a profile currently claims, read out of the remembered per-stream bag --
// the same layer the Go Live modal edits and streamMeta.set pushes, so a destination's
// target has exactly one home rather than a second copy kept beside it.
std::string ClaimedTarget(const std::string &profileUuid, const std::string &fieldKey)
{
	const json bag = ObsBootstrap::StreamMeta().StreamOverride(profileUuid);
	if (!bag.is_object()) {
		return std::string();
	}
	const auto field = bag.find(fieldKey);
	if (field == bag.end() || !field->is_object()) {
		return std::string();
	}
	const auto id = field->find("id");
	return id != field->end() && id->is_string() ? id->get<std::string>() : std::string();
}

// Record which target `profileUuid` streams to, merging into whatever else that stream
// already remembers rather than replacing the bag -- a title the user typed once is not
// this function's to discard.
void ClaimTarget(const std::string &profileUuid, const std::string &fieldKey, const OAuth::StreamTarget &target)
{
	StreamMetaStore &meta = ObsBootstrap::StreamMeta();
	json bag = meta.StreamOverride(profileUuid);
	if (!bag.is_object()) {
		bag = json::object();
	}
	// name and avatar ride along with the id because they are what the destination shows
	// itself as, and re-reading them would mean a platform call on every render of every
	// row. They are a cache of the target's identity at claim time; a Page renamed later
	// refreshes on the next reconcile, which runs at connect and at boot.
	bag[fieldKey] = json{{"id", target.id}, {"name", target.name}, {"avatarUrl", target.avatarUrl}};
	meta.PutStreamOverride(profileUuid, bag);
}

// The field key addressing a target for whichever provider this destination's account
// belongs to; empty when there is no account, no provider, or the provider has no targets.
// One resolver so the claim's reader and its writers cannot disagree about where it lives.
std::string FieldKeyForProfile(const std::string &profileUuid, std::string *accountIdOut)
{
	const StreamProfile *p = ObsBootstrap::StreamProfiles().Find(profileUuid);
	if (!p || p->accountId.empty()) {
		return std::string();
	}
	if (accountIdOut) {
		*accountIdOut = p->accountId;
	}
	const std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(p->accountId);
	if (!acct) {
		return std::string();
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(acct->providerId);
	return provider ? provider->targetFieldKey() : std::string();
}

// A label no existing profile's DisplayName already takes, suffixing " 2", " 3", ... --
// the generative side of the duplicate-name rule the create/update methods enforce, so
// two Pages sharing a name cannot produce two identically-named destinations.
std::string UniqueLabel(const StreamProfile &candidate, const std::string &base)
{
	StreamProfile probe;
	probe.serviceId = candidate.serviceId;
	if (candidate.settings) {
		probe.settings = obs_data_create();
		obs_data_apply(probe.settings, candidate.settings);
	}
	return StringUtil::UniqueName(base, StringUtil::BareName::Try, [&probe](const std::string &label) {
		probe.label = label;
		const std::string display = probe.DisplayName();
		for (const StreamProfile &other : ObsBootstrap::StreamProfiles().Profiles()) {
			if (astrcmpi(display.c_str(), other.DisplayName().c_str()) == 0) {
				return true;
			}
		}
		return false;
	});
}

// One canvas routing edge of the origin profile, snapshotted before anything is added
// (OutputBindings::Add invalidates references into the vector it appends to).
struct OriginRoute {
	std::string canvasUuid;
	bool enabled = false;
};

std::vector<OriginRoute> OriginRoutes(const std::string &originProfileUuid)
{
	std::vector<OriginRoute> routes;
	for (const OutputBinding &b : ObsBootstrap::OutputBindings().Bindings().bindings) {
		if (b.profileUuid == originProfileUuid) {
			routes.push_back(OriginRoute{b.canvasUuid, b.enabled});
		}
	}
	return routes;
}

// Route a freshly created profile exactly where the origin is routed, armed exactly as
// the origin is armed. The user pointed the connect at a destination they had already
// placed on a canvas; its siblings belong on the same canvas, or they exist without
// ever going live. Bindings are per scene-collection, so this is the ACTIVE collection
// only -- another collection routing the origin gets the new profiles as unbound
// destinations it can bind by hand.
void RouteLikeOrigin(const std::string &profileUuid, const std::vector<OriginRoute> &routes)
{
	OutputBindings &bindings = ObsBootstrap::OutputBindings().Bindings();
	for (const OriginRoute &route : routes) {
		if (bindings.HasPair(profileUuid, route.canvasUuid)) {
			continue;
		}
		OutputBinding &created = bindings.Add(route.canvasUuid);
		created.profileUuid = profileUuid;
		// Never enabled beyond what the origin already is: the single-live-stream rule
		// allows one enabled binding per profile, and the origin cannot hold two.
		created.enabled = route.enabled;
	}
}

// The service shape every destination of this account shares, taken from the origin
// once. Held as a standalone copy rather than a pointer into the profile store, whose
// vector reallocates on the first Add.
struct ProfileModel {
	std::string serviceId;
	OBSDataAutoRelease settings;
};

// Build the destination for `target` from that model.
StreamProfile NewProfileFor(const ProfileModel &model, const std::string &accountId, const OAuth::StreamTarget &target)
{
	StreamProfile p;
	p.serviceId = model.serviceId;
	p.settings = obs_data_create();
	if (model.settings) {
		obs_data_apply(p.settings, model.settings);
	}
	// The ingest endpoint is created per target at go-live, so carrying the origin's
	// credential over would point two destinations at one ingest until the first go-live
	// overwrote it -- and would trip the duplicate-key guard in the meantime.
	obs_data_unset_user_value(p.settings, p.KeyField());
	p.accountId = accountId;
	p.label = UniqueLabel(p, target.name);
	return p;
}

// The reconcile, on the UI thread. Additive by construction:
//
//   - a target some profile of this account already claims is left completely alone, so
//     reconnecting never duplicates a destination the user has since renamed, re-routed
//     or given its own title;
//   - a target with no claimant is given to an unclaimed profile of this account if
//     there is one -- which is how the profile the user connected FROM becomes the first
//     Page rather than having a sibling materialized beside it -- and otherwise gets a
//     new profile;
//   - a profile claiming a target the account no longer administers is LEFT STANDING. It
//     goes visibly stale (its go-live fails with the provider's own "no longer available"
//     sentence) rather than vanishing with whatever the user had configured on it.
//     Deleting is the only irreversible option here, so it is the one not taken.
void Reconcile(const std::string &accountId, const std::string &originProfileUuid, const OAuth::TargetList &targets)
{
	StreamProfileStore &profiles = ObsBootstrap::StreamProfiles();

	// The origin leads: it is matched by uuid rather than by its account link having
	// already landed, since the connect flow posts that link to this same UI queue just
	// ahead of us. Without it there is no model to copy a new destination from, so a
	// profile deleted since the grant began ends the reconcile rather than inventing one.
	const StreamProfile *origin = profiles.Find(originProfileUuid);
	if (!origin || (!origin->accountId.empty() && origin->accountId != accountId)) {
		return;
	}
	ProfileModel model;
	model.serviceId = origin->serviceId;
	if (origin->settings) {
		model.settings = obs_data_create();
		obs_data_apply(model.settings, origin->settings);
	}
	origin = nullptr; // the store's vector reallocates on the first Add below

	std::vector<std::string> owned{originProfileUuid};
	for (const StreamProfile &p : profiles.Profiles()) {
		if (p.accountId == accountId && p.uuid != originProfileUuid) {
			owned.push_back(p.uuid);
		}
	}

	std::vector<std::string> claimed;
	std::vector<std::string> unclaimed;
	for (const std::string &uuid : owned) {
		const std::string target = ClaimedTarget(uuid, targets.fieldKey);
		if (target.empty()) {
			// A profile of this account with no target recorded was ambiguous the moment
			// the account gained a second target -- its go-live could only ask the user to
			// choose one. Adopting a target makes it addressable instead.
			unclaimed.push_back(uuid);
		} else {
			claimed.push_back(target);
		}
	}

	// Snapshotted before the first Add, and reused for every created profile.
	const std::vector<OriginRoute> routes = OriginRoutes(originProfileUuid);

	size_t nextUnclaimed = 0;
	int adopted = 0;
	int created = 0;
	for (const OAuth::StreamTarget &target : targets.targets) {
		bool alreadyClaimed = false;
		for (const std::string &id : claimed) {
			if (id == target.id) {
				alreadyClaimed = true;
				break;
			}
		}
		if (alreadyClaimed) {
			continue;
		}
		if (nextUnclaimed < unclaimed.size()) {
			ClaimTarget(unclaimed[nextUnclaimed++], targets.fieldKey, target);
			adopted++;
			continue;
		}
		StreamProfile p = NewProfileFor(model, accountId, target);
		const std::string uuid = profiles.Add(std::move(p)).uuid;
		ClaimTarget(uuid, targets.fieldKey, target);
		RouteLikeOrigin(uuid, routes);
		created++;
	}

	// Whatever this pass settled on, the account's claims are now final for this reconcile.
	PublishTargetClaims(accountId);

	if (adopted == 0 && created == 0) {
		return;
	}

	ObsBootstrap::StreamMeta().Save();
	Bridge::EmitEvent(EventNames::kStreamMetaChanged, json{{"profileUuid", std::string()}});
	if (created > 0) {
		profiles.Save();
		ObsBootstrap::OutputBindings().Save();
		// A newly enabled binding activates its canvas; mirrors outputBinding.create.
		ObsBootstrap::CanvasRuntime().ReconcileAll();
		Bridge::EmitEvent(EventNames::kStreamProfileChanged, json::object());
		Bridge::EmitEvent(EventNames::kOutputBindingChanged, json::object());
	}
	HostLog("[oauth] " + accountId + ": " + std::to_string(targets.targets.size()) + " targets, " +
		std::to_string(adopted) + " adopted, " + std::to_string(created) + " destination(s) created");
}

// The profile the boot pass reconciles from. Store order is streams.json order, so the
// account's first profile is the one its connect ran from -- every sibling this reconcile
// creates is appended after it, and neither a rename nor a re-route moves it. So the boot
// pass copies its model from the same profile the connect path passed, and picks the same
// one on every launch. Empty when no profile is linked to the account any more: there is
// then no service shape or canvas routing to copy, and inventing one is not this pass's
// call.
std::string BootOriginProfile(const std::string &accountId)
{
	for (const StreamProfile &p : ObsBootstrap::StreamProfiles().Profiles()) {
		if (p.accountId == accountId) {
			return p.uuid;
		}
	}
	return std::string();
}

// One account's boot reconcile, snapshotted on the UI thread for the worker to replay.
struct BootReconcile {
	std::string accountId;
	std::string originProfileUuid;
};

} // namespace

void MaterializeTargetDestinations(const std::string &accountId, const std::string &originProfileUuid)
{
	if (accountId.empty() || originProfileUuid.empty()) {
		return;
	}
	const std::optional<OAuth::OAuthAccount> stored = OAuth::Accounts().Get(accountId);
	if (!stored) {
		return;
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(stored->providerId);
	if (!provider) {
		return;
	}

	OAuth::OAuthAccount acct = *stored;
	OAuth::TargetList targets;
	std::string err;
	if (!provider->enumerateTargets(acct, targets, err)) {
		// Never fatal to a connect that has already succeeded: the account is stored and
		// usable, and the worst outcome here is the destinations the user would otherwise
		// have to add by hand.
		HostLog("[oauth] target discovery failed for " + accountId + ": " + err);
		return;
	}
	// A single target still runs the reconcile, and that is the point rather than an
	// oversight. It creates no second profile -- the loop only ever adds one for a target
	// nothing claims -- but it does claim that one target on the origin profile, which is
	// what gives the destination its own name and picture. Skipping it here was why a
	// one-Page account had to be papered over by renaming the account itself, and why
	// that paper tore the moment a second Page appeared. Claim at one, claim at five.
	if (targets.fieldKey.empty() || targets.targets.empty()) {
		return;
	}

	AsyncTask::PostToUi(
		[accountId, originProfileUuid, targets] { Reconcile(accountId, originProfileUuid, targets); });
}

void MaterializeTargetDestinationsAtBoot()
{
	// A smoke/self-test run drives the app against the user's REAL config directory (the
	// dev rundir's config is a junction to it), so a pass that creates stream profiles
	// must not run there -- it would rewrite the user's streams.json on every such launch.
	if (Env::IsSelfTestRun()) {
		return;
	}

	std::vector<BootReconcile> pending;
	for (const auto &entry : OAuth::Accounts().All()) {
		// The shared connection gate, which also excludes an account the broker has
		// rejected as dead: spending a boot request on a credential that cannot answer
		// only costs the launch a timeout.
		if (!OAuth::IsAccountConnected(entry.second)) {
			continue;
		}
		const std::string origin = BootOriginProfile(entry.first);
		if (origin.empty()) {
			continue;
		}
		pending.push_back(BootReconcile{entry.first, origin});
	}
	if (pending.empty()) {
		return;
	}

	// enumerateTargets is a blocking platform read, so the accounts are walked off the UI
	// thread and boot returns without waiting. Routed through RunAsync rather than a raw
	// detached thread so teardown's drain counts this worker before the stores its
	// PostToUi reaches through are freed.
	AsyncTask::RunAsync([pending = std::move(pending)] {
		for (const BootReconcile &r : pending) {
			MaterializeTargetDestinations(r.accountId, r.originProfileUuid);
		}
	});
}

void PublishTargetClaims(const std::string &accountId)
{
	if (accountId.empty()) {
		return;
	}
	const std::optional<OAuth::OAuthAccount> acct = OAuth::Accounts().Get(accountId);
	if (!acct) {
		return;
	}
	OAuth::StreamProvider *provider = OAuth::Registry().Get(acct->providerId);
	if (!provider) {
		return;
	}
	const std::string fieldKey = provider->targetFieldKey();
	if (fieldKey.empty()) {
		return;
	}

	std::map<std::string, std::string> claims;
	for (const StreamProfile &p : ObsBootstrap::StreamProfiles().Profiles()) {
		if (p.accountId != accountId) {
			continue;
		}
		const std::string target = ClaimedTarget(p.uuid, fieldKey);
		if (!target.empty()) {
			claims[p.uuid] = target;
		}
	}
	provider->noteTargetClaims(accountId, std::move(claims));
}

void PublishAllTargetClaims()
{
	for (const auto &entry : OAuth::Accounts().All()) {
		PublishTargetClaims(entry.first);
	}
}

json ClaimedTargetOf(const std::string &profileUuid)
{
	json out = json{{"id", ""}, {"name", ""}, {"avatarUrl", ""}};
	const std::string fieldKey = FieldKeyForProfile(profileUuid, nullptr);
	if (fieldKey.empty()) {
		return out;
	}
	const json bag = ObsBootstrap::StreamMeta().StreamOverride(profileUuid);
	if (!bag.is_object()) {
		return out;
	}
	const auto field = bag.find(fieldKey);
	if (field == bag.end() || !field->is_object()) {
		return out;
	}
	for (const char *key : {"id", "name", "avatarUrl"}) {
		const auto value = field->find(key);
		if (value != field->end() && value->is_string()) {
			out[key] = value->get<std::string>();
		}
	}
	return out;
}

bool ClaimTargetForProfile(const std::string &profileUuid, const std::string &targetId, const std::string &name,
			   const std::string &avatarUrl, std::string &err)
{
	std::string accountId;
	const std::string fieldKey = FieldKeyForProfile(profileUuid, &accountId);
	if (fieldKey.empty()) {
		err = "this destination has no connected account with targets to choose from";
		return false;
	}
	if (targetId.empty()) {
		err = "choose a target";
		return false;
	}

	// A target belongs to exactly one destination. Two profiles claiming one Page would
	// each create a live video on it and contend for a single ingest endpoint, so the
	// second claim is refused here rather than left to collide at go-live -- where it
	// would surface as an opaque platform error mid-broadcast.
	for (const StreamProfile &other : ObsBootstrap::StreamProfiles().Profiles()) {
		if (other.uuid == profileUuid || other.accountId != accountId) {
			continue;
		}
		const json claim = ClaimedTargetOf(other.uuid);
		if (claim.value("id", std::string()) == targetId) {
			err = "\"" + name + "\" already belongs to the destination \"" + other.DisplayName() +
			      "\" -- change that one first";
			return false;
		}
	}

	ClaimTarget(profileUuid, fieldKey, OAuth::StreamTarget{targetId, name, avatarUrl});
	ObsBootstrap::StreamMeta().Save();
	PublishTargetClaims(accountId);
	// Both: the claim lives in the stream-meta bag, and it is also what a profile row
	// renders as its name and picture.
	Bridge::EmitEvent(EventNames::kStreamMetaChanged, json{{"profileUuid", profileUuid}});
	Bridge::EmitEvent(EventNames::kStreamProfileChanged, json::object());
	return true;
}
