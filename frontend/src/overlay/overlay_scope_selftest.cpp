// RunOverlayScopeSelfTest: the alert-box scope layers end to end on the host side, against
// throwaway stores under the self-test config directory. No sockets -- the HTTP routes are
// covered by RunOverlaySelfTest -- so this TU needs neither winsock nor obs.h.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "util/file_util.hpp"
#include "util/selftest_paths.hpp"
#include "overlay_scopes.hpp"
#include "overlay_store.hpp"
#include "overlay_template.hpp"

namespace {

using Overlay::json;

// A store file under the self-test directory with nothing left over from an earlier run:
// the store reads whatever is there, and a stale widget would make every count below wrong.
std::string FreshStorePath(const char *file)
{
	const std::string path = SelfTest::ConfigPath(file);
	if (!path.empty()) {
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(path), ec);
		std::filesystem::remove(std::filesystem::u8path(path + ".bak"), ec);
	}
	return path;
}

void RemoveStoreFiles(const std::string &path)
{
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(path), ec);
	std::filesystem::remove(std::filesystem::u8path(path + ".bak"), ec);
}

std::vector<unsigned char> Bytes(const char *s)
{
	return std::vector<unsigned char>(s, s + std::strlen(s));
}

bool FileExists(const std::string &path)
{
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

// The asset record for `file`, or null.
json AssetRecord(const Overlay::Widget &w, const std::string &file)
{
	for (const json &a : w.assets) {
		if (a.is_object() && a.value("file", std::string()) == file) {
			return a;
		}
	}
	return json(nullptr);
}

// The same field uploaded in three scopes lands in three files, each recorded with its scope
// and size; a scope's file survives a prune while anything names it and goes once nothing
// does; and the scope layers round-trip through the file.
bool ScopedAssetsStep()
{
	const std::string path = FreshStorePath("overlays-scope-assets.json");
	if (path.empty()) {
		HostLog("[selftest] overlay scoped assets -> FAILED (no scratch path)");
		return false;
	}
	bool named = false;
	bool recorded = false;
	bool refused = false;
	bool persisted = false;
	bool kept = false;
	bool released = false;
	{
		Overlay::OverlayStore store(path);
		const std::optional<Overlay::Widget> created = store.Create("scope-selftest", "alertbox");
		if (!created) {
			HostLog("[selftest] overlay scoped assets -> FAILED (no widget)");
			return false;
		}
		const std::string id = created->id;
		const std::string dir = Overlay::OverlayStore::AssetsDir(id);

		const std::string cheer = store.AddAsset(id, "cheer-media.webm", "video", Bytes("cheer"), "cheer");
		const std::string variation =
			store.AddAsset(id, "v_ab12-media.png", "image", Bytes("variation"), "v_ab12");
		const std::string defaults =
			store.AddAsset(id, "default-media.png", "image", Bytes("defaults"), "default");
		named = cheer == "assets/cheer-media.webm" && variation == "assets/v_ab12-media.png" &&
			defaults == "assets/default-media.png" &&
			FileUtil::ReadUtf8File(dir + "/cheer-media.webm").value_or("") == "cheer" &&
			FileUtil::ReadUtf8File(dir + "/default-media.png").value_or("") == "defaults";

		if (const std::optional<Overlay::Widget> w = store.Get(id)) {
			const json rec = AssetRecord(*w, "cheer-media.webm");
			recorded = rec.is_object() && rec.value("scope", std::string()) == "cheer" &&
				   rec.value("kind", std::string()) == "video" && rec.value("bytes", 0) == 5;
		}
		refused = store.AddAsset(id, "x.png", "image", Bytes("x"), "../up").empty() &&
			  store.RemoveScopeAssets(id, "../up") == 0;

		json variationDoc = json::object();
		variationDoc["id"] = "v_ab12";
		variationDoc["event"] = "cheer";
		variationDoc["when"] = json{{"field", "amount"}, {"op", ">="}, {"value", 1000}};
		variationDoc["settings"] = json{{"media", "assets/v_ab12-media.png"}};
		json patch = json::object();
		patch["settings"] = json{{"media", "assets/default-media.png"}};
		patch["overrides"] = json{{"cheer", json{{"media", "assets/cheer-media.webm"}}}};
		patch["variations"] = json::array({variationDoc});
		const bool updated = store.Update(id, patch) == Overlay::MutateResult::Ok;
		{
			Overlay::OverlayStore reread(path);
			const std::optional<Overlay::Widget> back = reread.Get(id);
			persisted = updated && back && back->overrides == patch["overrides"] &&
				    back->variations == patch["variations"];
		}

		kept = store.RemoveScopeAssets(id, "cheer") == 0 && store.RemoveScopeAssets(id, "v_ab12") == 0 &&
		       FileExists(dir + "/cheer-media.webm") && FileExists(dir + "/v_ab12-media.png");

		// Clearing the override and deleting the variation release their files; Defaults
		// still names its own, so it stays.
		json cleared = json::object();
		cleared["overrides"] = json::object();
		cleared["variations"] = json::array();
		const bool clearedOk = store.Update(id, cleared) == Overlay::MutateResult::Ok;
		const size_t cheerRemoved = store.RemoveScopeAssets(id, "cheer");
		const size_t variationRemoved = store.RemoveScopeAssets(id, "v_ab12");
		bool recordsGone = false;
		if (const std::optional<Overlay::Widget> w = store.Get(id)) {
			recordsGone = AssetRecord(*w, "cheer-media.webm").is_null() &&
				      AssetRecord(*w, "v_ab12-media.png").is_null() &&
				      AssetRecord(*w, "default-media.png").is_object();
		}
		released = clearedOk && cheerRemoved == 1 && variationRemoved == 1 && recordsGone &&
			   !FileExists(dir + "/cheer-media.webm") && !FileExists(dir + "/v_ab12-media.png") &&
			   FileExists(dir + "/default-media.png") && store.RemoveScopeAssets(id, "default") == 0;

		store.Delete(id);
	}
	RemoveStoreFiles(path);

	const bool ok = named && recorded && refused && persisted && kept && released;
	HostLog(std::string("[selftest] overlay scoped assets -> ") + (ok ? "OK" : "MISMATCH") +
		" (named=" + (named ? "ok" : "bad") + " recorded=" + (recorded ? "ok" : "bad") +
		" refused=" + (refused ? "ok" : "bad") + " persisted=" + (persisted ? "ok" : "bad") +
		" kept=" + (kept ? "ok" : "bad") + " released=" + (released ? "ok" : "bad") + ")");
	return ok;
}

// One widget as a v2 document stored it.
json V2Widget(const char *id, bool forked, const json &settings)
{
	json w = json::object();
	w["id"] = id;
	w["token"] = std::string("tok-") + id;
	w["name"] = id;
	w["type"] = "alertbox";
	w["settings"] = settings;
	w["custom"] = forked ? json{{"html", "<div></div>"}, {"css", ""}, {"js", ""}, {"fields", json::array()}}
			     : json(nullptr);
	w["assets"] = json::array();
	w["rev"] = 4;
	return w;
}

// The follow variation the upgrade adds to keep v2's YouTube follow wording.
json YouTubeFollowVariation(const std::string &message)
{
	return json::array({json{{"id", "v_follow_youtube"},
				 {"event", "follow"},
				 {"when", {{"field", "platform"}, {"op", "=="}, {"value", "youtube"}}},
				 {"settings", {{"message", message}}}}});
}

// v2 -> v3 on a document with every legacy key set and one with none: changed messages
// become event overrides, shipped ones are dropped, the burst dropdown becomes burstExit,
// a changed follow message or YouTube message keeps v2's YouTube wording as a follow
// variation, everything else stays, forks are untouched, the old file is kept as .v2.bak,
// and returning the fork to stock migrates what it kept.
bool MigrationStep()
{
	const std::string path = FreshStorePath("overlays-v2.json");
	if (path.empty()) {
		HostLog("[selftest] overlay v2 migration -> FAILED (no scratch path)");
		return false;
	}
	const std::string backup = path + ".v2.bak";
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(backup), ec);

	json every = json::object();
	every["msgFollow"] = "{name} is here!";
	every["msgSub"] = "{name} subscribed!"; // the shipped text: dropped, not pinned
	every["msgCheer"] = "Bits from {name}";
	every["msgRaid"] = "Raid: {name}";
	every["msgSuperchat"] = "SC {amount}";
	every["msgSupersticker"] = "Sticker {amount}";
	every["msgMember"] = "Welcome {name}";
	every["msgKicks"] = "Kicks {amountText}";
	every["burstAnimation"] = "fall";
	every["sound"] = "assets/ding.ogg";
	every["accent"] = "#ff0000";
	every["msgBurstMore"] = "and {count} others";
	json slide = json::object();
	slide["burstAnimation"] = "slide";
	json forkSettings = json::object();
	forkSettings["msgFollow"] = "{name} forked a follow";
	forkSettings["burstAnimation"] = "zoom";
	json youtube = json::object();
	youtube["msgSubscribeYouTube"] = "{name} hit subscribe!";
	json youtubeShipped = json::object();
	youtubeShipped["msgSubscribeYouTube"] = "{name} just subscribed!"; // dropped, not pinned

	json doc = json::object();
	doc["version"] = 2;
	doc["port"] = 43000;
	doc["widgets"] =
		json::array({V2Widget("every", false, every), V2Widget("none", false, json::object()),
			     V2Widget("slide", false, slide), V2Widget("fork", true, forkSettings),
			     V2Widget("youtube", false, youtube), V2Widget("youtubeShipped", false, youtubeShipped)});
	const std::string original = doc.dump(4);
	{
		std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
		out.write(original.data(), static_cast<std::streamsize>(original.size()));
	}

	bool everyOk = false;
	bool noneOk = false;
	bool slideOk = false;
	bool forkOk = false;
	bool stockedOk = false;
	bool youtubeOk = false;
	{
		Overlay::OverlayStore store(path);
		if (const std::optional<Overlay::Widget> w = store.Get("every")) {
			json expected = json::object();
			expected["follow"] = json{{"message", "{name} is here!"}};
			expected["cheer"] = json{{"message", "Bits from {name}"}};
			expected["raid"] = json{{"message", "Raid: {name}"}};
			expected["superchat"] = json{{"message", "SC {amount}"}};
			expected["supersticker"] = json{{"message", "Sticker {amount}"}};
			expected["member"] = json{{"message", "Welcome {name}"}};
			expected["kicks"] = json{{"message", "Kicks {amountText}"}};
			json settings = json::object();
			settings["sound"] = "assets/ding.ogg";
			settings["accent"] = "#ff0000";
			settings["msgBurstMore"] = "and {count} others";
			settings["burstExit"] = json{{"preset", "drop-out"}, {"speed", 1.5}};
			// msgFollow moved into the follow override, which would reach YouTube too, so v2's
			// shipped YouTube wording stays as a variation.
			everyOk = w->overrides == expected && w->settings == settings &&
				  w->variations == YouTubeFollowVariation("{name} just subscribed!") && w->rev == 4;
		}
		if (const std::optional<Overlay::Widget> w = store.Get("none")) {
			noneOk = w->settings.empty() && w->overrides.empty() && w->variations.empty();
		}
		if (const std::optional<Overlay::Widget> yt = store.Get("youtube")) {
			if (const std::optional<Overlay::Widget> shipped = store.Get("youtubeShipped")) {
				youtubeOk = yt->settings.empty() && yt->overrides.empty() &&
					    yt->variations == YouTubeFollowVariation("{name} hit subscribe!") &&
					    shipped->settings.empty() && shipped->overrides.empty() &&
					    shipped->variations.empty();
			}
		}
		if (const std::optional<Overlay::Widget> w = store.Get("slide")) {
			slideOk = w->settings.empty() && w->overrides.empty();
		}
		if (const std::optional<Overlay::Widget> w = store.Get("fork")) {
			forkOk = w->IsForked() && w->settings == forkSettings && w->overrides.empty();
		}
		// Back on the shipped template the fork's keys move too.
		if (store.ReturnToStock("fork") == Overlay::MutateResult::Ok) {
			if (const std::optional<Overlay::Widget> w = store.Get("fork")) {
				stockedOk = !w->IsForked() &&
					    w->overrides ==
						    json{{"follow", json{{"message", "{name} forked a follow"}}}} &&
					    w->settings ==
						    json{{"burstExit", json{{"preset", "zoom-out"}, {"speed", 1.5}}}} &&
					    w->variations == YouTubeFollowVariation("{name} just subscribed!");
			}
		}
	}
	const std::optional<std::string> written = FileUtil::ReadUtf8File(path);
	const json reread = written ? json::parse(*written, nullptr, false) : json();
	const bool versionOk = reread.is_object() && reread.value("version", 0) == 3;
	const bool backupOk = FileUtil::ReadUtf8File(backup).value_or("") == original;
	// Loaded again it is already v3: nothing moves and no second backup is attempted.
	bool idempotentOk = false;
	{
		Overlay::OverlayStore again(path);
		const std::optional<Overlay::Widget> w = again.Get("every");
		idempotentOk = w && w->overrides.contains("follow") && !w->settings.contains("msgFollow") &&
			       w->variations == YouTubeFollowVariation("{name} just subscribed!");
	}
	RemoveStoreFiles(path);
	std::filesystem::remove(std::filesystem::u8path(backup), ec);

	const bool ok = everyOk && noneOk && slideOk && forkOk && stockedOk && youtubeOk && versionOk && backupOk &&
			idempotentOk;
	HostLog(std::string("[selftest] overlay v2 migration -> ") + (ok ? "OK" : "MISMATCH") + " (every=" +
		(everyOk ? "ok" : "bad") + " none=" + (noneOk ? "ok" : "bad") + " slide=" + (slideOk ? "ok" : "bad") +
		" fork=" + (forkOk ? "ok" : "bad") + " stock=" + (stockedOk ? "ok" : "bad") +
		" youtube=" + (youtubeOk ? "ok" : "bad") + " version=" + (versionOk ? "ok" : "bad") +
		" backup=" + (backupOk ? "ok" : "bad") + " again=" + (idempotentOk ? "ok" : "bad") + ")");
	return ok;
}

// The alert box's shipped scopes.json reads, declares the eight events the template handles,
// and reaches a stock widget through Resolve.
bool TypeScopesStep()
{
	const Overlay::TypeTemplate shipped = Overlay::TemplateFor("alertbox");
	std::string keys;
	if (shipped.scopes.is_object() && shipped.scopes.contains("events") && shipped.scopes["events"].is_array()) {
		for (const json &ev : shipped.scopes["events"]) {
			keys += keys.empty() ? "" : ",";
			keys += ev.is_object() ? ev.value("key", std::string("?")) : std::string("?");
		}
	}
	Overlay::Widget stock;
	stock.type = "alertbox";
	const bool resolved = Overlay::Resolve(stock).scopes == shipped.scopes;
	const bool ok = shipped.status == Overlay::TemplateStatus::Ok &&
			keys == "follow,sub,cheer,raid,superchat,supersticker,member,kicks" && resolved;
	HostLog(std::string("[selftest] overlay type scopes -> ") + (ok ? "OK" : "MISMATCH") + " (events=" + keys +
		")");
	return ok;
}

// The served copy of a stock alert box: uploads and library sounds in every scope become
// served URLs, a sound only a variation names is still preloaded, widget-only keys and
// unknown keys stay out of the scope layers, stored values are untouched, and a fork gets
// the events alone.
bool ServedDataStep()
{
	json schema = json::array();
	schema.push_back(json{{"key", "sound"}, {"type", "sound"}, {"scope", "alert"}, {"default", ""}});
	schema.push_back(json{{"key", "media"}, {"type", "media"}, {"scope", "alert"}, {"default", ""}});
	schema.push_back(json{{"key", "burstWindow"}, {"type", "slider"}, {"scope", "widget"}, {"default", 1.5}});
	Overlay::ResolvedWidget resolved;
	resolved.schema = schema;
	resolved.scopes = json{{"events", json::array({json{{"key", "cheer"}, {"label", "Bits"}}})}};

	Overlay::Widget w;
	w.id = "served";
	w.token = "tok";
	w.rev = 7;
	w.type = "alertbox";
	w.settings = json{{"sound", "library:chime-01"}};
	w.overrides = json{{"cheer", json{{"media", "assets/cheer-media.webm"}, {"burstWindow", 9}, {"stray", 1}}}};
	json variation = json::object();
	variation["id"] = "v_ab12";
	variation["event"] = "cheer";
	variation["when"] = json{{"field", "amount"}, {"op", ">="}, {"value", 1000}};
	variation["settings"] = json{{"sound", "assets/v_ab12-sound.ogg"}, {"media", "library:chime-01"}};
	w.variations = json::array({variation});
	const Overlay::Widget stored = w;

	const Overlay::ServedData served = Overlay::BuildServedData(w, resolved);
	const std::string chime = "/lib/" + Overlay::LibraryFileFor("chime-01").value_or("?");
	const std::string upload = "/w/served/assets/v_ab12-sound.ogg?t=tok&r=7";
	const json expectedSounds = json::array({chime, upload});
	const bool fieldsOk = served.fields.value("sound", std::string()) == chime &&
			      served.fields.value("burstWindow", 0.0) == 1.5;
	const json &scopes = served.scopes;
	const bool layersOk = scopes.is_object() && scopes["events"] == resolved.scopes["events"] &&
			      scopes["overrides"] ==
				      json{{"cheer", json{{"media", "/w/served/assets/cheer-media.webm?t=tok&r=7"}}}} &&
			      scopes["variations"].size() == 1 &&
			      scopes["variations"][0]["settings"]["sound"] == upload &&
			      // A library id on a field that is not a sound is not a sound URL.
			      scopes["variations"][0]["settings"]["media"] == "library:chime-01" &&
			      scopes["variations"][0]["when"] == variation["when"];
	const bool soundsOk = served.sounds == expectedSounds;
	const bool storedOk = w.settings == stored.settings && w.overrides == stored.overrides &&
			      w.variations == stored.variations;

	Overlay::Widget fork = w;
	fork.custom = Overlay::CustomCode{};
	const Overlay::ServedData forked = Overlay::BuildServedData(fork, resolved);
	const bool forkOk = forked.scopes == json{{"events", resolved.scopes["events"]}};

	Overlay::ResolvedWidget plain;
	plain.schema = schema;
	const bool plainOk = Overlay::BuildServedData(w, plain).scopes.is_null();

	const bool ok = fieldsOk && layersOk && soundsOk && storedOk && forkOk && plainOk;
	HostLog(std::string("[selftest] overlay served scopes -> ") + (ok ? "OK" : "MISMATCH") +
		" (fields=" + (fieldsOk ? "ok" : "bad") + " layers=" + (layersOk ? "ok" : "bad") +
		" preload=" + (soundsOk ? "ok" : "bad") + " stored=" + (storedOk ? "ok" : "bad") +
		" fork=" + (forkOk ? "ok" : "bad") + " plain=" + (plainOk ? "ok" : "bad") + ")");
	return ok;
}

// One row per step, so a new check is a row rather than another hand-written conjunction.
struct ScopeStep {
	const char *name;
	bool (*run)();
};

constexpr ScopeStep kScopeSteps[] = {
	{"scoped assets", &ScopedAssetsStep},
	{"v2 migration", &MigrationStep},
	{"type scopes", &TypeScopesStep},
	{"served scopes", &ServedDataStep},
};

} // namespace

void ObsBootstrap::RunOverlayScopeSelfTest()
{
	std::string failed;
	for (const ScopeStep &step : kScopeSteps) {
		if (!step.run()) {
			failed += failed.empty() ? "" : ", ";
			failed += step.name;
		}
	}
	HostLog(failed.empty() ? std::string("[selftest] overlay scopes -> OK")
			       : "[selftest] overlay scopes -> FAILED (" + failed + ")");
}
