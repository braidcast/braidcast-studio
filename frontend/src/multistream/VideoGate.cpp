#include "VideoGate.hpp"

#include "util/env_config.hpp"

#include <obs.hpp>

#include <map>
#include <set>
#include <string>

namespace VideoGate {

namespace {

// uuid -> strong ref, held only for the duration of one Reconcile so the graph
// cannot change under the walk. Sets that outlive a call store uuids alone.
using SourceSet = std::map<std::string, OBSSource>;

bool GateEnabled()
{
	static const bool enabled = Env::Flag("BRAIDCAST_MAIN_IDLE_GATE", true);
	return enabled;
}

std::function<bool()> g_mainActiveFn;
std::function<void(const RootVisitor &)> g_canvasRootsFn;

// uuid -> outstanding IncShowing count. A source held here is being drawn by the
// frontend itself (a thumbnail render, a Multiview cell), so its tree renders
// regardless of what the Default canvas is doing.
std::map<std::string, int> g_showingRoots;

// The uuids gated by the last Reconcile. Needed to ungate a source that has
// since left the Default canvas's tree, which the next pass no longer visits.
std::set<std::string> g_gated;

std::string SourceUuid(obs_source_t *source)
{
	const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
	return uuid ? uuid : std::string();
}

void AddSource(obs_source_t *source, SourceSet &out)
{
	OBSSource ref(source); // null once the source has begun being destroyed
	if (!ref) {
		return;
	}
	const std::string uuid = SourceUuid(source);
	if (!uuid.empty()) {
		out.emplace(uuid, std::move(ref));
	}
}

// Add `root` and its active tree to `out`. obs_source_enum_active_tree visits
// descendants but not the root, hence the explicit add. Walking only the ACTIVE
// tree is deliberate: a hidden scene item's source is unreachable here and is
// already not showing, so it needs no gate.
void CollectActiveTree(obs_source_t *root, SourceSet &out)
{
	AddSource(root, out);
	obs_source_enum_active_tree(
		root,
		[](obs_source_t *, obs_source_t *child, void *param) {
			AddSource(child, *static_cast<SourceSet *>(param));
		},
		&out);
}

void UngateAll()
{
	for (const std::string &uuid : g_gated) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_video_gated(source, false);
		}
	}
	g_gated.clear();
}

} // namespace

void SetMainActivePredicate(std::function<bool()> fn)
{
	g_mainActiveFn = std::move(fn);
}

void SetCanvasRootEnumerator(std::function<void(const RootVisitor &)> fn)
{
	g_canvasRootsFn = std::move(fn);
}

void Reconcile()
{
	// No predicate means the runtime that owns it is gone (teardown) or not yet
	// built, and either way nothing can be judged idle.
	if (!GateEnabled() || !g_mainActiveFn) {
		UngateAll();
		return;
	}

	SourceSet mainTree;
	{
		OBSSourceAutoRelease main = obs_get_output_source(0);
		if (main) {
			CollectActiveTree(main, mainTree);
		}
	}

	// Everything reachable from a root other than Main. A source in here renders
	// for that root's sake even while Main is idle, so it must not be gated.
	SourceSet wanted;
	if (g_canvasRootsFn) {
		g_canvasRootsFn([&wanted](obs_source_t *root) { CollectActiveTree(root, wanted); });
	}
	for (const auto &holder : g_showingRoots) {
		OBSSourceAutoRelease root = obs_get_source_by_uuid(holder.first.c_str());
		if (root) {
			CollectActiveTree(root, wanted);
		}
	}

	const bool mainIdle = !g_mainActiveFn();

	std::set<std::string> nextGated;
	for (const auto &[uuid, source] : mainTree) {
		const bool gate = mainIdle && wanted.find(uuid) == wanted.end();
		obs_source_set_video_gated(source, gate);
		if (gate) {
			nextGated.insert(uuid);
		}
	}
	for (const std::string &uuid : g_gated) {
		if (mainTree.find(uuid) != mainTree.end()) {
			continue;
		}
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_video_gated(source, false);
		}
	}

	size_t added = 0;
	for (const std::string &uuid : nextGated) {
		added += g_gated.count(uuid) ? 0 : 1;
	}
	const size_t removed = g_gated.size() - (nextGated.size() - added);
	if (added || removed) {
		blog(LOG_INFO, "VideoGate: +%zu gated, -%zu ungated (%zu now gated)", added, removed, nextGated.size());
	}
	g_gated.swap(nextGated);
}

void IncShowing(obs_source_t *source)
{
	obs_source_inc_showing(source);
	const std::string uuid = SourceUuid(source);
	if (uuid.empty()) {
		return;
	}
	g_showingRoots[uuid]++;
	Reconcile(); // a new holder is a consumer appearing; restore is the fast path
}

void DecShowing(obs_source_t *source)
{
	const std::string uuid = SourceUuid(source);
	auto it = g_showingRoots.find(uuid);
	if (it != g_showingRoots.end() && --it->second <= 0) {
		g_showingRoots.erase(it);
	}
	obs_source_dec_showing(source);
	Reconcile();
}

void Shutdown()
{
	UngateAll();
	g_showingRoots.clear();
	g_mainActiveFn = nullptr;
	g_canvasRootsFn = nullptr;
}

} // namespace VideoGate
