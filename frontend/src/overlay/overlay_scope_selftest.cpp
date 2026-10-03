// RunOverlayScopeSelfTest: the alert-box scope layers end to end on the host side, against
// throwaway stores under the self-test config directory. No sockets -- the HTTP routes are
// covered by RunOverlaySelfTest -- so this TU needs neither winsock nor obs.h.

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "../log.hpp"
#include "../obs_bootstrap.hpp"
#include "util/file_util.hpp"
#include "util/selftest_paths.hpp"
#include "overlay_store.hpp"

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

// One row per step, so a new check is a row rather than another hand-written conjunction.
struct ScopeStep {
	const char *name;
	bool (*run)();
};

constexpr ScopeStep kScopeSteps[] = {
	{"scoped assets", &ScopedAssetsStep},
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
