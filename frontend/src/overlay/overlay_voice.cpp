#include "overlay_voice.hpp"

#include "overlay_sources.hpp"

#include "../audio/mixer_flags.hpp"
#include "../bridge.hpp"
#include "../log.hpp"
#include "../util/async_task.hpp"

#include <obs.h>
#include <obs.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OverlayVoice {

namespace {

// ---- the field table ------------------------------------------------------------------

// One row per Field, in Field order. `signal` is the per-source libobs signal announcing a
// change to the field, or nullptr when only the bridge changes it.
struct FieldSpec {
	Field field;
	const char *name;
	const char *signal;
	bool (*same)(obs_source_t *a, obs_source_t *b);
	void (*copy)(obs_source_t *from, obs_source_t *to);
};

// The getter/setter pair for a plain field, compared exactly: a copied value is the
// original's own bits.
#define VOICE_FIELD(field, name, signal, get, set)                                                    \
	FieldSpec                                                                                     \
	{                                                                                             \
		field, name, signal, [](obs_source_t *a, obs_source_t *b) { return get(a) == get(b); }, \
			[](obs_source_t *from, obs_source_t *to) { set(to, get(from)); }                \
	}
#define VOICE_MIXER_FLAG(field, name, key)                                                                    \
	FieldSpec                                                                                             \
	{                                                                                                     \
		field, name, nullptr,                                                                         \
			[](obs_source_t *a, obs_source_t *b) { return MixerFlags::Get(a, key) == MixerFlags::Get(b, key); }, \
			[](obs_source_t *from, obs_source_t *to) { MixerFlags::Set(to, key, MixerFlags::Get(from, key)); } \
	}

const FieldSpec kFields[] = {
	VOICE_FIELD(Field::Volume, "volume", "volume", obs_source_get_volume, obs_source_set_volume),
	VOICE_FIELD(Field::Muted, "muted", "mute", obs_source_muted, obs_source_set_muted),
	VOICE_FIELD(Field::Balance, "balance", "audio_balance", obs_source_get_balance_value,
		    obs_source_set_balance_value),
	VOICE_FIELD(Field::SyncOffset, "syncOffset", "audio_sync", obs_source_get_sync_offset,
		    obs_source_set_sync_offset),
	VOICE_FIELD(Field::Mixers, "tracks", "audio_mixers", obs_source_get_audio_mixers, obs_source_set_audio_mixers),
	VOICE_FIELD(Field::Monitoring, "monitoring", "audio_monitoring", obs_source_get_monitoring_type,
		    obs_source_set_monitoring_type),
	VOICE_FIELD(Field::ForceMono, "forceMono", "update_flags", MixerFlags::ForceMono, MixerFlags::SetForceMono),
	VOICE_FIELD(Field::PushToMute, "pushToMute", "push_to_mute_changed", obs_source_push_to_mute_enabled,
		    obs_source_enable_push_to_mute),
	VOICE_FIELD(Field::PushToMuteDelay, "pushToMuteDelay", "push_to_mute_delay", obs_source_get_push_to_mute_delay,
		    obs_source_set_push_to_mute_delay),
	VOICE_FIELD(Field::PushToTalk, "pushToTalk", "push_to_talk_changed", obs_source_push_to_talk_enabled,
		    obs_source_enable_push_to_talk),
	VOICE_FIELD(Field::PushToTalkDelay, "pushToTalkDelay", "push_to_talk_delay", obs_source_get_push_to_talk_delay,
		    obs_source_set_push_to_talk_delay),
	VOICE_MIXER_FLAG(Field::MixerHidden, "hidden", MixerFlags::kHidden),
	VOICE_MIXER_FLAG(Field::VolumeLocked, "volumeLocked", MixerFlags::kVolumeLocked),
	VOICE_MIXER_FLAG(Field::MixerPinned, "pinned", MixerFlags::kPinned),
};

#undef VOICE_FIELD
#undef VOICE_MIXER_FLAG

static_assert(std::size(kFields) == size_t(Field::Count), "one kFields row per Field");

const FieldSpec &Spec(Field field)
{
	return kFields[size_t(field)];
}

// ---- state shared with the signal threads -----------------------------------------------

// Cleared first thing in Stop; every callback and every queued task checks it.
std::atomic<bool> g_alive{false};
// Set when a pass is queued, cleared by the pass before it reads anything, so one pass
// serves every change before it and a change after the clear queues the next.
std::atomic<bool> g_pending{false};

// A leaf: nothing is called while it is held but the audio-group atomics, which take no
// lock. Guards everything down to the UI-only block below.
std::mutex g_mutex;
// overlay_id <-> group id, for the session; ids are never reused, so a group id in a
// queued task can never come to mean another overlay.
std::unordered_map<std::string, uint16_t> g_groupOf;
std::vector<std::string> g_overlayOf{std::string()}; // indexed by group; 0 is "none"
// Ranks are never reused in a session, so no two live members share one. 0 is "never
// published".
uint32_t g_nextRank = 1;
// Groups whose membership may have moved since the last pass.
std::set<uint16_t> g_dirty;
// member uuid -> the group it has adopted the settings of. A member signal is acted on only
// when its source had adopted its current group at the time.
std::unordered_map<std::string, uint16_t> g_adopted;
// (group, field) -> the newest change signalled for it. A queued sync that is no longer
// the newest stands down, so it can never write a stale value over a later change, and a
// bridge fan-out stands down while another member's later change is still queued.
struct Signalled {
	uint64_t seq = 0;
	std::string uuid;
	bool pending = false; // its sync has not run yet
	// The newest earlier change that stood down for it: spread from instead if this one's
	// source is gone by the time its sync runs, since its own change then never spreads.
	uint64_t fallbackSeq = 0;
	std::string fallbackUuid;
};
std::map<std::pair<uint16_t, size_t>, Signalled> g_latest;
uint64_t g_seq = 0;

// ---- UI thread only -----------------------------------------------------------------------

std::map<uint16_t, Group> g_groups;
std::unordered_map<std::string, std::string> g_representative; // member uuid -> representative uuid

uint32_t TakeRankLocked()
{
	return g_nextRank++;
}

uint16_t InternLocked(const std::string &overlayId)
{
	if (auto it = g_groupOf.find(overlayId); it != g_groupOf.end()) {
		return it->second;
	}
	if (g_overlayOf.size() > UINT16_MAX) {
		return 0; // out of group ids for this session: the overlay plays ungrouped
	}
	const uint16_t group = uint16_t(g_overlayOf.size());
	g_groupOf.emplace(overlayId, group);
	g_overlayOf.push_back(overlayId);
	return group;
}

std::string OverlayOfLocked(uint16_t group)
{
	return group < g_overlayOf.size() ? g_overlayOf[group] : std::string();
}

std::string UuidOf(obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	return uuid ? uuid : std::string();
}

uint16_t GroupOf(obs_source_t *source)
{
	return obs_source_get_audio_group(source, nullptr, nullptr);
}

void RunPass();
void SyncFromSignal(const std::string &uuid, uint16_t group, size_t field, uint64_t seq);

void SchedulePass()
{
	if (!g_pending.exchange(true)) {
		AsyncTask::QueueOnUi(&RunPass);
	}
}

// Decide `source`'s group from its settings and publish it, synchronously, so the next
// audio tick already gates a paste or a duplicate. The rank is taken once, at the first
// publish, and kept for the source's life: a property edit republishes without moving the
// strip to another copy.
void Publish(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	const std::string overlayId = settings ? obs_data_get_string(settings, Overlay::kOverlayIdKey) : "";
	// A source playing through CEF's own output is out of reach of the gate, so it stays
	// out of the group rather than stand as a silent voice.
	const bool rerouted = settings && obs_data_get_bool(settings, Overlay::kRerouteAudioKey) &&
			      obs_source_audio_active(source);
	const bool shutdown = settings && obs_data_get_bool(settings, Overlay::kShutdownWhenHiddenKey);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		uint32_t rank = 0;
		bool wasShutdown = false;
		const uint16_t previous = obs_source_get_audio_group(source, &rank, &wasShutdown);
		const bool first = rank == 0;
		const uint16_t group = rerouted && !overlayId.empty() ? InternLocked(overlayId) : 0;
		if (!first && group == previous && shutdown == wasShutdown) {
			return;
		}
		obs_source_set_audio_group(source, group, first ? TakeRankLocked() : rank, shutdown);
		if (previous) {
			g_dirty.insert(previous);
		}
		if (group) {
			g_dirty.insert(group);
		}
	}
	SchedulePass();
}

// ---- signal trampolines (any thread) ----------------------------------------------------

obs_source_t *SignalSource(calldata_t *cd)
{
	return static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
}

void OnMemberChanged(void *data, calldata_t *cd)
{
	if (!g_alive.load()) {
		return;
	}
	obs_source_t *source = SignalSource(cd);
	const uint16_t group = source ? GroupOf(source) : 0;
	if (!group) {
		return;
	}
	const size_t field = reinterpret_cast<uintptr_t>(data);
	std::string uuid = UuidOf(source);
	uint64_t seq = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_adopted.find(uuid);
		if (it == g_adopted.end() || it->second != group) {
			return; // not yet adopted: its value is about to be replaced, never spread
		}
		seq = ++g_seq;
		Signalled &latest = g_latest[{group, field}];
		latest.seq = seq;
		latest.uuid = uuid;
		latest.pending = true;
	}
	AsyncTask::QueueOnUi([uuid = std::move(uuid), group, field, seq] { SyncFromSignal(uuid, group, field, seq); });
}

void ConnectMemberSignals(obs_source_t *source)
{
	signal_handler_t *handler = obs_source_get_signal_handler(source);
	for (size_t i = 0; i < std::size(kFields); i++) {
		if (kFields[i].signal) {
			signal_handler_connect(handler, kFields[i].signal, OnMemberChanged,
					       reinterpret_cast<void *>(uintptr_t(i)));
		}
	}
}

void OnSourceCreate(void *, calldata_t *cd)
{
	obs_source_t *source = SignalSource(cd);
	if (!g_alive.load() || !Overlay::IsOverlaySource(source)) {
		return;
	}
	ConnectMemberSignals(source);
	Publish(source);
}

// source_update, source_audio_activate, source_audio_deactivate: an overlay_id edit, a
// shutdown toggle, or reroute_audio switching the audio path.
void OnSourceRepublish(void *, calldata_t *cd)
{
	obs_source_t *source = SignalSource(cd);
	if (!g_alive.load() || !Overlay::IsOverlaySource(source)) {
		return;
	}
	Publish(source);
}

// source_remove, source_destroy: the group may have lost its representative. A source
// recreated under the same uuid (an undone delete, a collection reloaded) is a new member and
// adopts the group's settings again, so its load signals never spread.
void OnSourceGone(void *, calldata_t *cd)
{
	obs_source_t *source = SignalSource(cd);
	if (!g_alive.load() || !source) {
		return;
	}
	const uint16_t group = GroupOf(source);
	if (!group) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_adopted.erase(UuidOf(source));
		g_dirty.insert(group);
	}
	SchedulePass();
}

struct GlobalSignal {
	const char *name;
	signal_callback_t callback;
};

const GlobalSignal kGlobalSignals[] = {
	{"source_create", OnSourceCreate},
	{"source_update", OnSourceRepublish},
	{"source_audio_activate", OnSourceRepublish},
	{"source_audio_deactivate", OnSourceRepublish},
	{"source_remove", OnSourceGone},
	{"source_destroy", OnSourceGone},
};

// ---- UI thread ----------------------------------------------------------------------------

// The live members of `group`: the overlay's sources still publishing that group, by rank.
std::vector<OBSSource> Members(uint16_t group, const std::string &overlayId)
{
	std::vector<std::pair<uint32_t, OBSSource>> ranked;
	for (OBSSource &source : Overlay::SourcesUsing(overlayId)) {
		uint32_t rank = 0;
		if (obs_source_get_audio_group(source, &rank, nullptr) == group) {
			ranked.emplace_back(rank, std::move(source));
		}
	}
	std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	std::vector<OBSSource> members;
	members.reserve(ranked.size());
	for (auto &[rank, source] : ranked) {
		members.push_back(std::move(source));
	}
	return members;
}

// Copy `field` from `from` onto every other member that differs; the copy changed, if any.
obs_source_t *SpreadField(obs_source_t *from, const std::vector<OBSSource> &members, const FieldSpec &spec)
{
	obs_source_t *changed = nullptr;
	for (const OBSSource &member : members) {
		if (member.Get() == from || spec.same(from, member)) {
			continue;
		}
		spec.copy(from, member);
		changed = member;
	}
	return changed;
}

bool FanOutFrom(obs_source_t *source, uint16_t group, const std::string &overlayId, const FieldSpec &spec)
{
	obs_source_t *changed = SpreadField(source, Members(group, overlayId), spec);
	if (changed) {
		DBG(LogCat::Overlay, "voice group %u: %s of '%s' copied to its copies", unsigned(group), spec.name,
		    obs_source_get_name(source));
	}
	return changed != nullptr;
}

// Whether a change to `field` of `group` is signalled and not yet spread. Caller holds
// g_mutex.
bool SyncPendingLocked(uint16_t group, size_t field)
{
	auto it = g_latest.find({group, field});
	return it != g_latest.end() && it->second.pending;
}

// Settle one group after its membership moved, or after a change could not be spread:
// members that joined adopt the group's established settings, an established member that
// differs from the representative on a field with no change in flight is brought back to
// it, and the strip's representative and member list are rebuilt. Returns whether what the
// mixer lists for it changed.
bool Settle(uint16_t group, const std::string &overlayId)
{
	const std::vector<OBSSource> members = overlayId.empty() ? std::vector<OBSSource>() : Members(group, overlayId);
	std::vector<std::string> uuids;
	for (const OBSSource &member : members) {
		uuids.push_back(UuidOf(member));
	}

	std::vector<bool> established(members.size(), false);
	std::vector<bool> settled(std::size(kFields), false); // no change in flight
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (size_t i = 0; i < uuids.size(); i++) {
			auto it = g_adopted.find(uuids[i]);
			established[i] = it != g_adopted.end() && it->second == group;
		}
		for (size_t f = 0; f < settled.size(); f++) {
			settled[f] = !SyncPendingLocked(group, f);
		}
	}

	if (!members.empty()) {
		const auto firstEstablished = std::find(established.begin(), established.end(), true);
		const bool firstAdoption = firstEstablished == established.end();
		const size_t rep = firstAdoption ? 0 : size_t(firstEstablished - established.begin());
		obs_source_t *changed = nullptr;

		// The copy a scene duplicate used to mute on another canvas was never the user's
		// choice, so where the copies of a group that has never been settled disagree,
		// unmuted wins.
		if (firstAdoption && members.size() > 1) {
			const bool anyUnmuted = std::any_of(members.begin(), members.end(),
							    [](const OBSSource &m) { return !obs_source_muted(m); });
			for (const OBSSource &member : members) {
				if (anyUnmuted && obs_source_muted(member)) {
					obs_source_set_muted(member, false);
					changed = member;
				}
			}
		}

		for (size_t i = 0; i < members.size(); i++) {
			if (i == rep) {
				continue;
			}
			for (size_t f = 0; f < std::size(kFields); f++) {
				const FieldSpec &spec = kFields[f];
				if ((!established[i] || settled[f]) && !spec.same(members[rep], members[i])) {
					spec.copy(members[rep], members[i]);
					changed = members[i];
				}
			}
		}

		if (changed) {
			HostLog("[overlay] voice group " + std::to_string(group) + " (" + overlayId +
				"): " + std::to_string(members.size()) + " copies now share the audio settings of '" +
				obs_source_get_name(members[rep]) + "'" +
				(firstAdoption ? ", unmuted where they differed" : ""));
			Bridge::PersistSourceState(changed);
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	auto old = g_groups.find(group);
	const std::vector<std::string> previous = old != g_groups.end() ? old->second.members
									: std::vector<std::string>();
	for (const std::string &uuid : previous) {
		if (std::find(uuids.begin(), uuids.end(), uuid) == uuids.end()) {
			if (auto it = g_adopted.find(uuid); it != g_adopted.end() && it->second == group) {
				g_adopted.erase(it);
			}
			if (auto it = g_representative.find(uuid); it != g_representative.end() &&
								   old != g_groups.end() &&
								   it->second == old->second.members.front()) {
				g_representative.erase(it);
			}
		}
	}
	for (const std::string &uuid : uuids) {
		g_adopted[uuid] = group;
	}
	if (uuids.empty()) {
		if (old != g_groups.end()) {
			g_groups.erase(old);
			return true;
		}
		return false;
	}
	for (const std::string &uuid : uuids) {
		g_representative[uuid] = uuids.front();
	}
	if (old != g_groups.end() && old->second.members == uuids) {
		return false;
	}
	g_groups[group] = Group{overlayId, uuids};
	return true;
}

void RunPass()
{
	if (!g_alive.load()) {
		return;
	}
	g_pending.exchange(false);

	std::vector<std::pair<uint16_t, std::string>> dirty;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (uint16_t group : g_dirty) {
			dirty.emplace_back(group, OverlayOfLocked(group));
		}
		g_dirty.clear();
	}

	bool listChanged = false;
	for (const auto &[group, overlayId] : dirty) {
		listChanged = Settle(group, overlayId) || listChanged;
	}
	if (listChanged) {
		Bridge::EmitAudioChanged();
	}
}

// `uuid` while it is still a member of `group`, else null.
OBSSourceAutoRelease MemberByUuid(const std::string &uuid, uint16_t group)
{
	OBSSourceAutoRelease source = uuid.empty() ? nullptr : obs_get_source_by_uuid(uuid.c_str());
	if (source && GroupOf(source) != group) {
		source = nullptr;
	}
	return source;
}

// A member's field changed outside the bridge (a hotkey, a filter, libobs itself): spread
// it, unless a later change to the same field has been signalled since. When its source is
// gone by now, the newest earlier change spreads instead, and with none left the group is
// settled from its representative, so every signalled change ends in a group that agrees.
void SyncFromSignal(const std::string &uuid, uint16_t group, size_t field, uint64_t seq)
{
	if (!g_alive.load()) {
		return;
	}
	std::string overlayId;
	std::string fallbackUuid;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_latest.find({group, field});
		if (it == g_latest.end()) {
			return;
		}
		Signalled &latest = it->second;
		if (latest.seq != seq) {
			// A later change to this field is queued or already spread.
			if (seq > latest.fallbackSeq) {
				latest.fallbackSeq = seq;
				latest.fallbackUuid = uuid;
			}
			return;
		}
		latest.pending = false;
		fallbackUuid = std::move(latest.fallbackUuid);
		latest.fallbackSeq = 0;
		latest.fallbackUuid.clear();
		overlayId = OverlayOfLocked(group);
	}
	OBSSourceAutoRelease source = MemberByUuid(uuid, group);
	if (!source) {
		source = MemberByUuid(fallbackUuid, group);
	}
	if (!source) {
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_dirty.insert(group);
		}
		SchedulePass();
		return;
	}
	if (FanOutFrom(source, group, overlayId, kFields[field])) {
		Bridge::PersistSourceState(source);
		Bridge::EmitAudioChanged();
	}
}

} // namespace

void Start()
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (!handler) {
		return;
	}
	g_alive.store(true);
	for (const GlobalSignal &signal : kGlobalSignals) {
		signal_handler_connect(handler, signal.name, signal.callback, nullptr);
	}
}

void Stop()
{
	g_alive.store(false);
	if (signal_handler_t *handler = obs_get_signal_handler()) {
		for (const GlobalSignal &signal : kGlobalSignals) {
			signal_handler_disconnect(handler, signal.name, signal.callback, nullptr);
		}
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_dirty.clear();
		g_adopted.clear();
		g_latest.clear();
	}
	g_pending.store(false);
	g_groups.clear();
	g_representative.clear();
}

bool FanOut(obs_source_t *source, Field field)
{
	if (!g_alive.load() || !source) {
		return false;
	}
	const uint16_t group = GroupOf(source);
	if (!group) {
		return false;
	}
	const std::string uuid = UuidOf(source);
	std::string overlayId;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_adopted.find(uuid);
		if (it == g_adopted.end() || it->second != group) {
			return false;
		}
		// Another member changed this field after the caller's write (a hotkey on another
		// thread) and its sync is still queued: that change is the newer one, and it will
		// spread over this one.
		auto latest = g_latest.find({group, size_t(field)});
		if (latest != g_latest.end() && latest->second.pending && latest->second.uuid != uuid) {
			return false;
		}
		overlayId = OverlayOfLocked(group);
	}
	return FanOutFrom(source, group, overlayId, Spec(field));
}

std::string RepresentativeOf(const std::string &uuid)
{
	auto it = g_representative.find(uuid);
	return it != g_representative.end() ? it->second : uuid;
}

const Group *GroupRepresentedBy(const std::string &uuid)
{
	for (const auto &[group, info] : g_groups) {
		if (!info.members.empty() && info.members.front() == uuid) {
			return &info;
		}
	}
	return nullptr;
}

} // namespace OverlayVoice
