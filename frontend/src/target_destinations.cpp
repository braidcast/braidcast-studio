#include "target_destinations.hpp"

#include "event_names.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <util/dstr.h>

#include "util/async_task.hpp"
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
	bag[fieldKey] = json{{"id", target.id}, {"name", target.name}};
	meta.PutStreamOverride(profileUuid, bag);
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
	for (int n = 1;; n++) {
		probe.label = n == 1 ? base : base + " " + std::to_string(n);
		const std::string display = probe.DisplayName();
		bool taken = false;
		for (const StreamProfile &other : ObsBootstrap::StreamProfiles().Profiles()) {
			if (astrcmpi(display.c_str(), other.DisplayName().c_str()) == 0) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			return probe.label;
		}
	}
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
	// One target is the same shape as none for this: there is nothing to choose between,
	// so the account stays the single destination it already was.
	if (targets.fieldKey.empty() || targets.targets.size() < 2) {
		return;
	}

	AsyncTask::PostToUi(
		[accountId, originProfileUuid, targets] { Reconcile(accountId, originProfileUuid, targets); });
}
