#include "overlay_store.hpp"

#include "../log.hpp"
#include "../multistream/StorePaths.hpp"
#include "overlay_template.hpp"
#include "util/file_util.hpp"
#include "util/json_util.hpp"
#include "util/random_util.hpp"
#include "uuid_util.hpp"

#include <obs.hpp>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace Overlay {

namespace {

// Widget-token length; the server compares the whole string, so this is the only
// place it is decided.
constexpr size_t kTokenBytes = 16;

// The persisted document's shape. v2 replaced a widget's baked-in html/css/js/fields with
// `settings` (overrides only) plus `custom` (absent until the user forks). v3 added the
// alert box's scope layers (`overrides`, `variations`) and moved its per-event message and
// burst keys into them (MigrateV2AlertBox). A document already carrying this number is
// loaded as it stands, which is the whole of the already-migrated test: the number is
// written by the same Save that installs the new shape, so it cannot be present without it.
constexpr int kStoreVersion = 3;

// Where the pre-upgrade document is kept, beside overlays.json, named for the version it was
// so a v1 copy an earlier upgrade kept is never confused with a v2 one. Deliberately NOT
// "overlays.json.bak": that name is the save envelope's own rotation slot
// (obs_data_save_json_pretty_safe writes .tmp then rotates the outgoing file into .bak)
// and Load() reads it as the fallback for a torn write, so the first ordinary save after
// the migration would overwrite a pre-migration backup stored there with post-migration
// content.
std::string PreMigrationSuffix(int fromVersion)
{
	return ".v" + std::to_string(fromVersion) + ".bak";
}

// A widget's own directory (the parent of AssetsDir): the single place the
// overlays/<id> layout is spelled out.
std::string WidgetDir(const std::string &id)
{
	return MultistreamBasicPath(("overlays/" + id).c_str());
}

// Drop a widget's whole directory, errors swallowed (a leftover dir is not worth
// failing a delete over). Call with mutex_ RELEASED -- see the note in AddAsset.
void RemoveWidgetDir(const std::string &id)
{
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::u8path(WidgetDir(id)), ec);
}

// Clone one widget's assets directory onto another's. Call with mutex_ RELEASED -- the
// tree walk is the same blocking I/O RemoveWidgetDir and AddAsset keep off the lock. A
// source that has never had an upload has no directory, which is nothing to copy rather
// than a failure.
bool CopyAssetsDir(const std::string &fromId, const std::string &toId)
{
	const std::filesystem::path from = std::filesystem::u8path(OverlayStore::AssetsDir(fromId));
	std::error_code ec;
	if (!std::filesystem::exists(from, ec)) {
		return true;
	}
	const std::string to = OverlayStore::AssetsDir(toId);
	if (os_mkdirs(to.c_str()) == MKDIR_ERROR) {
		return false;
	}
	std::filesystem::copy(from, std::filesystem::u8path(to), std::filesystem::copy_options::recursive, ec);
	return !ec;
}

// The widget with this id, or null. Caller holds mutex_.
Widget *FindWidget(std::vector<Widget> &widgets, const std::string &id)
{
	for (Widget &w : widgets) {
		if (w.id == id) {
			return &w;
		}
	}
	return nullptr;
}

// The same lookup from a const member function, so the read-only accessors go through one
// spelling of "which widget is this id" rather than each carrying its own loop.
const Widget *FindWidget(const std::vector<Widget> &widgets, const std::string &id)
{
	for (const Widget &w : widgets) {
		if (w.id == id) {
			return &w;
		}
	}
	return nullptr;
}

// One v1 widget in the v2 model. Whether it keeps its code is decided by the code itself,
// because that is the only honest signal separating "never touched it" from "edited it":
//
//   html, css and js all byte-identical to what this build ships for the type -> stock,
//   so it picks up every later improvement to that template.
//
//   anything else -> forked: the v1 code becomes the widget's own, and it renders exactly
//   what it rendered before. That covers the widget whose type ships no template at all,
//   and also the widget that is merely STALE -- created against an older template and
//   never edited. Treating stale as forked is the safe direction to be wrong in: it
//   preserves what is on screen, and overlays.resetDefaults is one click back to stock.
//
// Values leave fields[] either way. In v1 a field carried its own value; in v2 the widget
// carries every value in settings and a field list is schema only, which is the shape
// overlays.fork produces and the shape the server merges against.
Widget MigrateV1Widget(const json &j)
{
	// v1 documents carry neither key, so this reads id/token/name/type/assets/rev and
	// leaves the widget stock with no overrides -- exactly the base the rules below
	// amend.
	Widget w = Widget::FromJson(j);
	const std::string html = j.value("html", std::string());
	const std::string css = j.value("css", std::string());
	const std::string js = j.value("js", std::string());
	const json fields = j.contains("fields") && j["fields"].is_array() ? j["fields"] : json::array();

	const TypeTemplate shipped = TemplateFor(w.type);
	const bool stock = shipped.status == TemplateStatus::Ok && html == shipped.html && css == shipped.css &&
			   js == shipped.js;
	if (!stock) {
		CustomCode code{html, css, js, json::array()};
		for (json f : fields) {
			if (f.is_object()) {
				f.erase("value");
			}
			code.fields.push_back(std::move(f));
		}
		w.custom = std::move(code);
	}

	// Only the schema that decides "unchanged" differs between the two. A stock widget is
	// compared against the CURRENT shipped fields.json, so a value that is merely a copy
	// of a default goes back to tracking it even if that default has since moved; a key
	// the type has dropped survives as an override of a null default, which loses nothing.
	// A forked widget is compared against its own list, which is now its schema, so every
	// value it actually rendered with is preserved to the byte.
	const json &schema = w.custom ? w.custom->fields : shipped.schema;
	for (const json &f : fields) {
		if (!f.is_object()) {
			continue;
		}
		const auto keyIt = f.find("key");
		const auto valueIt = f.find("value");
		if (keyIt == f.end() || !keyIt->is_string() || valueIt == f.end()) {
			continue;
		}
		const std::string key = keyIt->get<std::string>();
		if (*valueIt == SchemaDefault(schema, key)) {
			continue;
		}
		w.settings[key] = *valueIt;
	}
	return w;
}

// The alert box's v2 per-event message keys, the event each now overrides, and the text each
// shipped with. Frozen: the v3 template ships these messages in scopes.json instead, and a
// value equal to what v2 shipped was never the user's choice, so it is dropped rather than
// pinned as an override that would then ignore a later change to the built-in message.
struct LegacyMessage {
	const char *key;
	const char *event;
	const char *shipped;
};

constexpr LegacyMessage kLegacyMessages[] = {
	{"msgFollow", "follow", "{name} just followed!"},
	{"msgSub", "sub", "{name} subscribed!"},
	{"msgCheer", "cheer", "{name} cheered {amountText}!"},
	{"msgRaid", "raid", "{name} raided with {amountText}!"},
	{"msgSuperchat", "superchat", "{name} sent {amount}!"},
	{"msgSupersticker", "supersticker", "{name} sent a {amount} sticker!"},
	{"msgMember", "member", "{name} is now a member!"},
	{"msgKicks", "kicks", "{name} sent {amountText}!"},
};

// v2's burst exit dropdown, by value, to the v3 Out preset that draws the same motion. The
// v2 cards left at 1.5x the In speed; v2's default ("slide") is v3's default exit exactly,
// so it is dropped rather than stored.
struct LegacyBurstExit {
	const char *from;
	const char *preset;
};

constexpr LegacyBurstExit kLegacyBurstExits[] = {
	{"flip", "flip-out-x"},
	{"fall", "drop-out"},
	{"fade", "fade-out"},
	{"zoom", "zoom-out"},
};
constexpr char kLegacyBurstKey[] = "burstAnimation";
constexpr char kLegacyBurstDefault[] = "slide";
constexpr char kBurstExitKey[] = "burstExit";
constexpr double kLegacyBurstSpeed = 1.5;
constexpr char kAlertBoxType[] = "alertbox";

// v2's YouTube follow message. v2 showed it for every YouTube follow, whatever msgFollow said
// (its shipped text when the widget never set it). v3 ships that text as the follow event's
// built-in YouTube message, but a follow override's message reaches YouTube too, so when the
// upgrade moves a changed msgFollow into the override, or the user changed this key, a
// follow variation on YouTube keeps v2's YouTube wording.
constexpr char kLegacyYouTubeFollowKey[] = "msgSubscribeYouTube";
constexpr char kLegacyYouTubeFollowShipped[] = "{name} just subscribed!";
constexpr char kYouTubeFollowVariationId[] = "v_follow_youtube";

// Adds the YouTube follow variation unless one with its id is already there.
void AddYouTubeFollowVariation(Widget &w, const std::string &message)
{
	if (!w.variations.is_array()) {
		w.variations = json::array();
	}
	for (const json &v : w.variations) {
		if (v.is_object() && v.value("id", std::string()) == kYouTubeFollowVariationId) {
			return;
		}
	}
	w.variations.push_back(json{{"id", kYouTubeFollowVariationId},
				    {"event", "follow"},
				    {"when", {{"field", "platform"}, {"op", "=="}, {"value", "youtube"}}},
				    {"settings", {{"message", message}}}});
}

// One stock alert box in the v3 model: each legacy message the user changed becomes that
// event's override, and the burst dropdown becomes the burstExit animation value. Forks are
// left exactly as they are -- their own code reads the old keys -- and so is every other key.
// Idempotent, so returning a fork to stock can run it again on whatever the fork kept.
void MigrateV2AlertBox(Widget &w)
{
	if (w.IsForked() || w.type != kAlertBoxType) {
		return;
	}
	bool movedFollow = false;
	for (const LegacyMessage &m : kLegacyMessages) {
		const auto it = w.settings.find(m.key);
		if (it == w.settings.end()) {
			continue;
		}
		const json value = *it;
		w.settings.erase(m.key);
		if (!value.is_string() || value == m.shipped) {
			continue;
		}
		json &layer = w.overrides[m.event];
		if (!layer.is_object()) {
			layer = json::object();
		}
		// A message the v3 layer already holds was set in v3 and wins over the old one.
		if (!layer.contains("message")) {
			layer["message"] = value;
			movedFollow = movedFollow || std::string(m.event) == "follow";
		}
	}
	const auto youtube = w.settings.find(kLegacyYouTubeFollowKey);
	std::string youtubeMessage = kLegacyYouTubeFollowShipped;
	if (youtube != w.settings.end()) {
		if (youtube->is_string()) {
			youtubeMessage = youtube->get<std::string>();
		}
		w.settings.erase(kLegacyYouTubeFollowKey);
	}
	if (movedFollow || youtubeMessage != kLegacyYouTubeFollowShipped) {
		AddYouTubeFollowVariation(w, youtubeMessage);
	}
	const auto burst = w.settings.find(kLegacyBurstKey);
	if (burst == w.settings.end()) {
		return;
	}
	const std::string from = burst->is_string() ? burst->get<std::string>() : std::string();
	w.settings.erase(kLegacyBurstKey);
	if (from == kLegacyBurstDefault || w.settings.contains(kBurstExitKey)) {
		return;
	}
	for (const LegacyBurstExit &b : kLegacyBurstExits) {
		if (from == b.from) {
			w.settings[kBurstExitKey] = json{{"preset", b.preset}, {"speed", kLegacyBurstSpeed}};
			return;
		}
	}
}

// How hard the upgrade tries before it believes a partial read. Small on purpose: the
// whole budget is spent once, only after something has already gone wrong.
constexpr int kPartialReadAttempts = 3;
constexpr std::chrono::milliseconds kPartialReadRetryDelay(50);

// The first type in `widgets` whose template STILL reads back incomplete after the
// retries, or nothing when they all resolved.
//
// Partial only, deliberately -- not Absent and not Corrupt. Those two are facts about what
// shipped: a legacy type with no template on disk, or a fields.json that does not parse,
// reads the same way on every boot, so deferring the upgrade on them would defer it
// forever and those widgets would never leave v1. A partial read is the outcome of one
// attempt on files that ARE there, and the next attempt may well get all of them.
//
// It matters because the upgrade is one-shot: it decides stock-vs-forked by comparing a
// widget's code against the shipped template, writes `version`, and never revisits it. A
// css file locked for the milliseconds of that read makes the comparison meaningless, and
// every stock widget of that type is then written to disk as forked -- rendering
// correctly, silently cut off from template improvements, and recoverable only if the user
// notices and resets each one by hand.
//
// Hence the retry, and hence it living HERE rather than in TemplateFor. On this machine the
// likely cause of a partial read is antivirus holding a file open for a moment mid-boot --
// the same transient lock that makes the build spuriously fail on build_info.obj -- so a
// second look a moment later usually returns everything, and the alternative for that blip
// is the read-only session the deferral imposes. TemplateFor is on the request path, where
// a widget document is being assembled for a browser source; sleeping there would stall a
// live overlay to no purpose, since a request can simply be made again.
std::optional<std::string> FirstTypeStillPartial(const json &widgets)
{
	std::set<std::string> checked;
	for (const json &item : widgets) {
		if (!item.is_object()) {
			continue;
		}
		const std::string type = item.value("type", std::string());
		if (!checked.insert(type).second) {
			continue;
		}
		// Only Partial is retried; TemplateFor caches nothing else, so each pass is a
		// genuine re-read rather than the same cached verdict handed back.
		for (int attempt = 1; TemplateFor(type).status == TemplateStatus::Partial; ++attempt) {
			if (attempt == kPartialReadAttempts) {
				return type;
			}
			HostLog("[overlay] the template for type '" + type +
				"' read back incomplete; looking again before deciding the overlays.json upgrade");
			std::this_thread::sleep_for(kPartialReadRetryDelay);
		}
	}
	return std::nullopt;
}

// Reduce a caller-supplied asset key to a safe filename: keep only [A-Za-z0-9._-]
// (this already drops both path separators), then remove every ".." so a sanitized
// name can never traverse out of the assets dir. Empty result => caller must bail.
std::string SanitizeAssetKey(const std::string &key)
{
	std::string out;
	out.reserve(key.size());
	for (char c : key) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				c == '.' || c == '_' || c == '-';
		if (ok) {
			out.push_back(c);
		}
	}
	size_t pos;
	while ((pos = out.find("..")) != std::string::npos) {
		out.erase(pos, 2);
	}
	return out;
}

constexpr char kAssetPrefix[] = "assets/";

// Every upload file `v` names, at any depth: a string value "assets/<file>" anywhere in an
// object or array. Read off the values rather than the schema, so a reference is found
// whatever field type holds it -- the same prefix test the server's rewrite uses.
void CollectAssetRefs(const json &v, std::set<std::string> &out)
{
	if (v.is_string()) {
		const std::string &s = v.get_ref<const std::string &>();
		if (s.rfind(kAssetPrefix, 0) == 0) {
			out.insert(s.substr(sizeof(kAssetPrefix) - 1));
		}
		return;
	}
	if (v.is_object() || v.is_array()) {
		for (const json &child : v) {
			CollectAssetRefs(child, out);
		}
	}
}

} // namespace

json CustomCode::ToJson() const
{
	return json{{"html", html}, {"css", css}, {"js", js}, {"fields", fields}};
}

CustomCode CustomCode::FromJson(const json &j)
{
	CustomCode c;
	if (!j.is_object()) {
		return c;
	}
	c.html = j.value("html", std::string());
	c.css = j.value("css", std::string());
	c.js = j.value("js", std::string());
	c.fields = j.contains("fields") && j["fields"].is_array() ? j["fields"] : json::array();
	return c;
}

json Widget::ToJson() const
{
	// `custom` is emitted even when the widget is stock, as an explicit null: the editor
	// reads it as the stock/forked flag, and a key that comes and goes would make an
	// absent one ambiguous with a field the transport dropped.
	return json{
		{"id", id},
		{"token", token},
		{"name", name},
		{"type", type},
		{"settings", settings},
		{"overrides", overrides},
		{"variations", variations},
		{"custom", custom ? custom->ToJson() : json(nullptr)},
		{"assets", assets},
		{"rev", rev},
	};
}

json Widget::ToListJson(int port) const
{
	return json{{"id", id}, {"name", name}, {"type", type}, {"token", token}, {"url", WidgetUrl(*this, port)}};
}

Widget Widget::FromJson(const json &j)
{
	Widget w;
	if (!j.is_object()) {
		return w;
	}
	w.id = j.value("id", std::string());
	w.token = j.value("token", std::string());
	w.name = j.value("name", std::string());
	w.type = j.value("type", std::string());
	if (j.contains("settings") && j["settings"].is_object()) {
		w.settings = j["settings"];
	}
	if (j.contains("overrides") && j["overrides"].is_object()) {
		w.overrides = j["overrides"];
	}
	if (j.contains("variations") && j["variations"].is_array()) {
		w.variations = j["variations"];
	}
	// Only an object forks a widget. A null -- what a stock widget persists -- and an
	// absent key both mean stock, so neither obs_data's round trip nor a hand-trimmed
	// file can turn a stock widget into a forked one serving an empty document.
	if (j.contains("custom") && j["custom"].is_object()) {
		w.custom = CustomCode::FromJson(j["custom"]);
	}
	w.assets = j.contains("assets") && j["assets"].is_array() ? j["assets"] : json::array();
	w.rev = j.value("rev", 0);
	return w;
}

ResolvedWidget Resolve(const Widget &w)
{
	if (w.custom) {
		return ResolvedWidget{w.custom->fields, w.custom->html, w.custom->css, w.custom->js};
	}
	TypeTemplate shipped = TemplateFor(w.type);
	return ResolvedWidget{std::move(shipped.schema), std::move(shipped.html), std::move(shipped.css),
			      std::move(shipped.js)};
}

const char *DescribeMutateResult(MutateResult r)
{
	switch (r) {
	case MutateResult::Ok:
		return "no error";
	case MutateResult::NoSuchWidget:
		return "no such overlay";
	case MutateResult::NotForked:
		return "custom code requires overlays.fork first";
	case MutateResult::AlreadyForked:
		return "the overlay already has custom code";
	case MutateResult::AlreadyStock:
		return "the overlay is already on the shipped template";
	case MutateResult::NoTemplate:
		return "this overlay's type has no template that resolves";
	case MutateResult::NotPersisted:
		return "the change could not be written to disk, so nothing was changed";
	}
	return "unknown error";
}

std::string WidgetUrl(const Widget &w, int port)
{
	return "http://127.0.0.1:" + std::to_string(port) + "/w/" + w.id + "?t=" + w.token;
}

std::string OverlayStore::FilePath()
{
	return MultistreamBasicPath("overlays.json");
}

std::string OverlayStore::AssetsDir(const std::string &id)
{
	return WidgetDir(id) + "/assets";
}

std::vector<Widget> OverlayStore::List() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return widgets_;
}

std::optional<Widget> OverlayStore::Get(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Widget *w = FindWidget(widgets_, id);
	return w ? std::optional<Widget>(*w) : std::nullopt;
}

std::optional<std::string> OverlayStore::TypeOf(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Widget *w = FindWidget(widgets_, id);
	return w ? std::optional<std::string>(w->type) : std::nullopt;
}

std::optional<bool> OverlayStore::MayPlayAudio(const std::string &id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const Widget *w = FindWidget(widgets_, id);
	return w ? std::optional<bool>(w->MayPlayAudio()) : std::nullopt;
}

int OverlayStore::Port() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return port_;
}

void OverlayStore::SetPort(int port)
{
	std::lock_guard<std::mutex> lock(mutex_);
	port_ = port;
	Save();
}

std::optional<Widget> OverlayStore::Create(const std::string &name, const std::string &type)
{
	Widget w;
	w.token = RandomUtil::HexToken(kTokenBytes);
	if (w.token.empty()) {
		HostLog("[overlay] Create: no system entropy for an access token; no widget created");
		return std::nullopt;
	}
	w.id = UuidUtil::New();
	w.name = name;
	w.type = type;

	// Nothing is copied out of the template. A widget with no code of its own and no
	// overrides serves whatever its type ships today and, unlike every widget created
	// before this model, whatever it ships after the next update too.
	if (TemplateFor(type).status == TemplateStatus::Absent) {
		HostLog("[overlay] Create: no template for type '" + type + "' at " + TemplateDir(type) +
			" -- this widget will serve an empty document");
	}

	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
	Save();
	return w;
}

MutateResult OverlayStore::Update(const std::string &id, const json &patch, int *newRev)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Widget *w = FindWidget(widgets_, id);
	if (w == nullptr) {
		return MutateResult::NoSuchWidget;
	}
	// Presence, not type, is the test: a caller that names one of these keys is asking to
	// write code, and answering "not forked" is more use to them than silently ignoring a
	// malformed value. Refusing before anything is written is what keeps a stock widget
	// from being detached from its shipped template as a side effect of saving a form.
	const bool carriesCode = patch.contains("html") || patch.contains("css") || patch.contains("js") ||
				 patch.contains("fields");
	if (carriesCode && !w->IsForked()) {
		return MutateResult::NotForked;
	}

	// Snapshot before the first write, so a save that does not land can put the widget
	// back exactly as it was. Restoring is the only way the refusal can be truthful: the
	// alternative leaves memory ahead of disk, and any later save in the session then
	// commits the change the caller was told had not been made.
	const Widget before = *w;
	if (patch.contains("name") && patch["name"].is_string()) {
		w->name = patch["name"].get<std::string>();
	}
	if (patch.contains("settings") && patch["settings"].is_object()) {
		w->settings = patch["settings"];
	}
	if (patch.contains("overrides") && patch["overrides"].is_object()) {
		w->overrides = patch["overrides"];
	}
	if (patch.contains("variations") && patch["variations"].is_array()) {
		w->variations = patch["variations"];
	}
	if (carriesCode) {
		if (patch.contains("html") && patch["html"].is_string()) {
			w->custom->html = patch["html"].get<std::string>();
		}
		if (patch.contains("css") && patch["css"].is_string()) {
			w->custom->css = patch["css"].get<std::string>();
		}
		if (patch.contains("js") && patch["js"].is_string()) {
			w->custom->js = patch["js"].get<std::string>();
		}
		if (patch.contains("fields") && patch["fields"].is_array()) {
			w->custom->fields = patch["fields"];
		}
	}
	++w->rev;
	if (!Save()) {
		*w = before;
		return MutateResult::NotPersisted;
	}
	// Only once it is on disk: a revision reported for a change that was rolled back is a
	// revision a browser source would resolve and the store would never write.
	if (newRev != nullptr) {
		*newRev = w->rev;
	}
	return MutateResult::Ok;
}

MutateResult OverlayStore::Fork(const std::string &id, int *newRev, CustomCode *custom)
{
	std::string type;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const Widget *w = FindWidget(widgets_, id);
		if (w == nullptr) {
			return MutateResult::NoSuchWidget;
		}
		if (w->IsForked()) {
			return MutateResult::AlreadyForked;
		}
		type = w->type;
	}

	// Outside mutex_ (the discipline AddAsset documents), and required to resolve cleanly
	// before the widget is touched: a fork copies what it is handed and stores it, so a
	// file that happened not to read -- or a fields.json that read but did not parse --
	// becomes a permanent gap in the user's own code that only a work-discarding reset
	// undoes.
	const TypeTemplate shipped = TemplateFor(type);
	if (shipped.status != TemplateStatus::Ok) {
		HostLog("[overlay] Fork: the template for type '" + type + "' at " + TemplateDir(type) +
			" did not resolve; leaving " + id + " untouched");
		return MutateResult::NoTemplate;
	}
	// The schema seeds custom.fields so the user starts from the form they were already
	// looking at. It carries declarations only; their values stay where they have always
	// been, in the widget's settings, and are keyed the same way either side of the fork.
	CustomCode code{shipped.html, shipped.css, shipped.js, shipped.schema};

	std::lock_guard<std::mutex> lock(mutex_);
	Widget *w = FindWidget(widgets_, id);
	if (w == nullptr) {
		return MutateResult::NoSuchWidget; // deleted while the template was being read
	}
	if (w->IsForked()) {
		return MutateResult::AlreadyForked; // forked by another caller in the same window
	}
	const Widget before = *w;
	w->custom = code;
	++w->rev;
	if (!Save()) {
		// Put it back stock. Leaving it forked here is what made one failed click answer
		// "could not be written" and the next one "the overlay already has custom code":
		// two contradictory refusals for the same button, with the editor still showing
		// a widget the store no longer agreed about.
		*w = before;
		return MutateResult::NotPersisted;
	}
	if (newRev != nullptr) {
		*newRev = w->rev;
	}
	if (custom != nullptr) {
		*custom = std::move(code);
	}
	return MutateResult::Ok;
}

MutateResult OverlayStore::ReturnToStock(const std::string &id, int *newRev)
{
	std::string type;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const Widget *w = FindWidget(widgets_, id);
		if (w == nullptr) {
			return MutateResult::NoSuchWidget;
		}
		// Nothing to return: bumping the revision here would reload every browser source
		// bound to this widget for no change at all, which on stream is a visible blink
		// bought by a double-click.
		if (!w->IsForked()) {
			return MutateResult::AlreadyStock;
		}
		type = w->type;
	}

	// Outside mutex_, and checked before anything is dropped: a widget pointed back at a
	// template that does not resolve renders that gap, and by then its own code is gone.
	// The unknown or legacy type and the file locked for a moment land here as a failed
	// read; the fields.json a bad install truncated lands here as a failed PARSE, having
	// read back perfectly well.
	if (TemplateFor(type).status != TemplateStatus::Ok) {
		HostLog("[overlay] ReturnToStock: the template for type '" + type + "' at " + TemplateDir(type) +
			" did not resolve; leaving " + id + " untouched");
		return MutateResult::NoTemplate;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	Widget *w = FindWidget(widgets_, id);
	if (w == nullptr) {
		return MutateResult::NoSuchWidget; // deleted while the template was being read
	}
	if (!w->IsForked()) {
		return MutateResult::AlreadyStock; // returned by another caller in the same window
	}
	const Widget before = *w;
	w->custom.reset();
	// The fork kept the alert box's v2 keys for its own code; back on the shipped template
	// they mean nothing, so they move to where v3 reads them, exactly as the upgrade does.
	MigrateV2AlertBox(*w);
	++w->rev;
	if (!Save()) {
		*w = before;
		return MutateResult::NotPersisted;
	}
	if (newRev != nullptr) {
		*newRev = w->rev;
	}
	return MutateResult::Ok;
}

std::optional<Widget> OverlayStore::Duplicate(const std::string &id)
{
	Widget copy;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const Widget *source = FindWidget(widgets_, id);
		if (source == nullptr) {
			HostLog("[overlay] Duplicate: no such overlay " + id);
			return std::nullopt;
		}
		copy = *source;
		copy.token = RandomUtil::HexToken(kTokenBytes);
		if (copy.token.empty()) {
			HostLog("[overlay] Duplicate: no system entropy for an access token; no duplicate created");
			return std::nullopt;
		}
		copy.id = UuidUtil::New();
		copy.name = source->name + " copy";
	}
	// Outside mutex_, on the same discipline AddAsset documents: this walks a directory
	// tree, and the video thread takes this lock on every overlay source's update.
	//
	// The copy keeps assets[], so every field holding an "assets/<file>" path resolves
	// under the new id. A failed copy fails the whole duplicate rather than registering a
	// widget whose alert sound and image 404 -- that failure mode is silent on stream,
	// which is the worst place to discover it.
	if (!CopyAssetsDir(id, copy.id)) {
		RemoveWidgetDir(copy.id);
		HostLog("[overlay] Duplicate: could not copy the assets of " + id + "; no duplicate created");
		return std::nullopt;
	}
	bool sourceGone = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (FindWidget(widgets_, id) != nullptr) {
			widgets_.push_back(copy);
			Save();
		} else {
			sourceGone = true;
		}
	}
	if (sourceGone) {
		// Deleted while the tree was being copied. Registering the copy now would put the
		// content that delete just removed straight back.
		RemoveWidgetDir(copy.id);
		HostLog("[overlay] duplicate of " + id + " aborted; the source was deleted mid-copy");
		return std::nullopt;
	}
	return copy;
}

bool OverlayStore::Delete(const std::string &id)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const size_t before = widgets_.size();
		widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(),
					      [&](const Widget &w) { return w.id == id; }),
			       widgets_.end());
		if (widgets_.size() == before) {
			return false;
		}
		Save();
	}
	// Outside mutex_: remove_all walks a directory tree, and the video thread now takes
	// this lock on every overlay source's update, so a held lock here is dropped frames.
	// The path derives from the id alone, so nothing under the lock is needed. Dropping
	// the directory AFTER the registry is persisted also fails safe -- a crash in
	// between orphans a directory nothing references, rather than leaving a widget whose
	// assets are already gone.
	RemoveWidgetDir(id);
	return true;
}

std::string OverlayStore::AddAsset(const std::string &id, const std::string &key, const std::string &kind,
				   const std::vector<unsigned char> &bytes, const std::string &scope)
{
	const std::string safeKey = SanitizeAssetKey(key);
	if (safeKey.empty() || SanitizeAssetKey(scope) != scope) {
		return std::string();
	}
	{
		// Existence check only; the blob below is written with mutex_ RELEASED. At the
		// 8 MB cap the write and its flush to disk are tens of milliseconds, and a replace
		// that has to retry adds up to a further 220 ms, while the video thread takes this
		// lock on every overlay source's update -- a hold that long is several missed
		// composites, not jitter. Same discipline OverlayServer applies to sseMutex_
		// (snapshot under lock, do the blocking part unlocked; see overlay_server.hpp).
		std::lock_guard<std::mutex> lock(mutex_);
		if (!FindWidget(widgets_, id)) {
			return std::string();
		}
	}

	const std::string dir = AssetsDir(id);
	if (os_mkdirs(dir.c_str()) == MKDIR_ERROR) {
		return std::string();
	}
	const std::string full = dir + "/" + safeKey;
	// Atomic write: a crash or partial write must never leave a truncated asset the
	// overlay would then serve. No backup is kept, and a temp file a failed replace kept is
	// deleted: nothing would ever read either, and each would sit in the assets directory
	// unreferenced, outliving RemoveAsset and copied along by every Duplicate.
	const FileUtil::AtomicWriteResult written = FileUtil::WriteBinaryFileAtomic(full, bytes.data(), bytes.size());
	if (written == FileUtil::AtomicWriteResult::ReplaceFailed) {
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(FileUtil::AtomicWriteTempPath(full)), ec);
	}
	if (written != FileUtil::AtomicWriteResult::Ok) {
		return std::string();
	}
	bool stillPresent = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Widget *target = FindWidget(widgets_, id);
		if (target) {
			stillPresent = true;
			json record{{"key", key}, {"kind", kind}, {"file", safeKey}, {"bytes", bytes.size()}};
			if (!scope.empty()) {
				record["scope"] = scope;
			}
			// A re-upload under the same name replaces the record too, so its size and
			// kind describe the bytes now on disk rather than the first upload's.
			bool exists = false;
			for (json &a : target->assets) {
				if (a.is_object() && a.value("file", std::string()) == safeKey) {
					a = record;
					exists = true;
					break;
				}
			}
			if (!exists) {
				target->assets.push_back(std::move(record));
			}
			// Unconditionally, including when `exists` was already true -- that is the
			// re-upload-under-the-same-name case, where the filename and therefore the
			// served URL do not move but the bytes just did. The revision is what the
			// asset URL is keyed on (overlay_server.cpp AssembleDocument) and what the
			// source URL is keyed on (overlay_sources.cpp), so bumping it here, under the
			// same lock as the append and in the same call as the byte write, is what
			// makes "these bytes changed" and "this URL changed" a single fact. It does
			// not RE-RESOLVE a live source -- this call sweeps nothing, so one already on
			// a scene keeps the old URL until some later mutation refreshes it.
			//
			// Here rather than in the editor's follow-up overlays.update, because that
			// follow-up is not guaranteed: FieldsPanel drops it when the panel moved to
			// another widget during the upload, and Update rolls `rev` back to its
			// pre-call value when the save fails. Both leave the URL still naming bytes
			// that are no longer there. A caller that does follow up merely bumps a
			// second time, which costs one extra revalidation and nothing else -- no
			// caller passes or compares a revision, it is only ever read back.
			++target->rev;
			// The Save() below persists it: Widget::ToJson writes `rev`, so no extra
			// write is needed. It is a no-op in a deferred-upgrade session, exactly as
			// the appended asset entry above already is -- see Save()'s own note.
			Save();
		}
	}
	if (!stillPresent) {
		// Deleted while the blob was being written: that Delete already dropped the
		// directory, which the write above then recreated. Clear it again rather than
		// leave a directory no widget references.
		RemoveWidgetDir(id);
		return std::string();
	}
	return "assets/" + safeKey;
}

bool OverlayStore::RemoveAsset(const std::string &id, const std::string &file)
{
	// The same guard AddAsset writes through, so the two agree on what a stored file can
	// be named and a crafted `file` cannot reach outside the assets directory.
	const std::string safeFile = SanitizeAssetKey(file);
	if (safeFile.empty()) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Widget *target = FindWidget(widgets_, id);
		if (target == nullptr) {
			return false;
		}
		json kept = json::array();
		bool found = false;
		for (const json &a : target->assets) {
			if (a.is_object() && a.value("file", std::string()) == safeFile) {
				found = true;
				continue;
			}
			kept.push_back(a);
		}
		if (!found) {
			return false;
		}
		target->assets = std::move(kept);
		Save();
	}
	// Outside mutex_, and only once the registry no longer names the file -- the ordering
	// Delete uses. A crash in between orphans a file nothing points at, rather than
	// leaving a record whose file is already gone.
	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(AssetsDir(id) + "/" + safeFile), ec);
	return true;
}

size_t OverlayStore::RemoveScopeAssets(const std::string &id, const std::string &scope)
{
	// The same shape AddAsset accepts, so a scope that could never have been recorded is
	// refused before anything is compared against it.
	if (scope.empty() || SanitizeAssetKey(scope) != scope) {
		return 0;
	}
	std::vector<std::string> doomed;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Widget *target = FindWidget(widgets_, id);
		if (target == nullptr) {
			return 0;
		}
		std::set<std::string> referenced;
		CollectAssetRefs(target->settings, referenced);
		CollectAssetRefs(target->overrides, referenced);
		CollectAssetRefs(target->variations, referenced);
		json kept = json::array();
		for (const json &a : target->assets) {
			const std::string file = a.is_object() ? a.value("file", std::string()) : std::string();
			const bool ours = a.is_object() && a.value("scope", std::string()) == scope;
			if (ours && !file.empty() && referenced.count(file) == 0) {
				doomed.push_back(file);
				continue;
			}
			kept.push_back(a);
		}
		if (doomed.empty()) {
			return 0;
		}
		target->assets = std::move(kept);
		Save();
	}
	// Outside mutex_, after the registry stopped naming them -- RemoveAsset's ordering, for
	// its reason: a crash in between orphans a file, never a record. A recorded name that
	// is not already sanitized was not written by AddAsset and is left alone.
	for (const std::string &file : doomed) {
		if (SanitizeAssetKey(file) != file) {
			continue;
		}
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(AssetsDir(id) + "/" + file), ec);
	}
	return doomed.size();
}

void OverlayStore::InjectForTest(const Widget &w)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.push_back(w);
}

void OverlayStore::RemoveForTest(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	widgets_.erase(std::remove_if(widgets_.begin(), widgets_.end(), [&](const Widget &w) { return w.id == id; }),
		       widgets_.end());
}

void OverlayStore::Load()
{
	// Called from the ctor before `this` is visible to any other thread, so no lock.
	bool unusable = false;
	OBSDataAutoRelease root = LoadStoreData(path_, &unusable, "[overlay]");
	const char *js = root ? obs_data_get_json(root) : nullptr;
	const json parsed = js ? JsonUtil::ParseJson(js) : json();
	const json &widgets = JsonUtil::Obj(parsed, "widgets");
	if (!widgets.is_array()) {
		// Starting empty rather than aborting boot. A file that reads but holds no widget
		// list is kept aside like one that does not read at all (LoadStoreData has kept that
		// one), so its widgets are not lost to the empty store's first save.
		if (!unusable) {
			unusable = KeepUnusableStore(path_, "[overlay]");
		}
		hold_.AfterLoad(unusable, WidgetsJson().dump());
		return;
	}
	port_ = parsed.value("port", 43000);
	// Every document written before the field existed is a v1 document.
	const int version = parsed.value("version", 1);
	const bool migrating = version < kStoreVersion;
	// Only the v1 step compares code against the shipped templates, so only it can be
	// misled by a partial read. Asked before a single widget is converted, because the
	// answer decides whether anything at all may be written for the rest of the session.
	const std::optional<std::string> unreadableType = version < 2 ? FirstTypeStillPartial(widgets) : std::nullopt;
	upgradeDeferred_ = unreadableType.has_value();
	for (const json &item : widgets) {
		Widget w = version < 2 ? MigrateV1Widget(item) : Widget::FromJson(item);
		if (migrating) {
			MigrateV2AlertBox(w);
		}
		widgets_.push_back(std::move(w));
	}

	// The server refuses a request whose token is empty, so a stored widget without one
	// is unreachable forever unless it is re-minted here. Its URL changes, which is why
	// the log names the widget.
	bool reminted = false;
	for (Widget &w : widgets_) {
		if (!w.token.empty()) {
			continue;
		}
		w.token = RandomUtil::HexToken(kTokenBytes);
		if (w.token.empty()) {
			HostLog("[overlay] widget " + w.id + " has no access token and none could be minted");
			continue;
		}
		reminted = true;
		HostLog("[overlay] widget " + w.id + " had no access token; minted one -- its URL changed");
	}

	if (migrating) {
		if (upgradeDeferred_) {
			// The old document stays exactly as it is and this session runs read-only
			// on the in-memory conversion, so nothing commits a verdict reached from
			// an incomplete read. Whether the cause was a momentary lock or a genuinely
			// broken rundir cannot be told apart from here, and both are served by the
			// same answer: try again next start.
			//
			// A warning, and specific about what read-only costs, because the store
			// cannot say it per call: only Update, Fork and ReturnToStock surface the
			// refusal. This one line is the only notice the other five give.
			const std::string warning =
				"[overlay] overlays.json left at v" + std::to_string(version) +
				": the template for type '" + *unreadableType + "' at " + TemplateDir(*unreadableType) +
				" still read back incomplete after " + std::to_string(kPartialReadAttempts) +
				" attempts, and the upgrade needs it to tell an edited widget from an untouched "
				"one. The overlay store is READ-ONLY until the install is repaired: edits, forks "
				"and resets refuse outright, while new widgets, deletes and asset changes appear "
				"to work and are gone at the next start.";
			blog(LOG_WARNING, "%s", warning.c_str());
			return;
		}
		// The whole conversion happened in memory above, so nothing on disk has moved
		// yet and a throw on the way here would have left the v1 file exactly as it was.
		// The copy goes down before the save that replaces it.
		WritePreMigrationBackup(path_, parsed, version);
		HostLog("[overlay] overlays.json upgraded to v" + std::to_string(kStoreVersion) + " (" +
			std::to_string(widgets_.size()) + " widgets)");
		if (!Save()) {
			// The document on disk is still the old version, so the next start reads it and upgrades
			// again -- the conversion is idempotent, and this session runs on the
			// in-memory result meanwhile.
			HostLog("[overlay] the upgraded overlays.json could not be written; it will be upgraded "
				"again on the next start");
		}
		return;
	}
	if (reminted) {
		Save();
	}
}

// Keep the document as it was read, beside overlays.json, before the migrated save
// replaces it. Best-effort by design, for two reasons. The save envelope already rotates
// the outgoing old file into overlays.json.bak by itself, so this is a second copy rather
// than the only one. And skipping the upgrade save would not even stop the new version
// reaching disk: the widgets are already converted in memory, and the next Save from any
// source rewrites the whole document -- SetPort does it on every boot -- so refusing here
// would lose the backup and write the new version anyway.
void OverlayStore::WritePreMigrationBackup(const std::string &path, const json &asRead, int fromVersion) const
{
	const std::string backupPath = path + PreMigrationSuffix(fromVersion);
	std::error_code ec;
	if (std::filesystem::exists(std::filesystem::u8path(backupPath), ec)) {
		// An earlier upgrade already put one here. The one that is already down is the
		// older, more original document; do not paper over it with a newer one.
		return;
	}
	// The file's own bytes, so the copy is what the user had rather than what obs_data made
	// of it. `asRead` covers only the narrow case where the file stops being readable
	// between obs_data's read and this one -- deleted, locked, or its permissions changed
	// in that window. It is NOT the .bak-fallback case: obs_data_create_from_json_file_safe
	// renames the backup OVER the original before re-reading it, so after a fallback the
	// file at `path` is the recovered document and reads fine.
	std::string bytes;
	if (!FileUtil::ReadUtf8File(path, bytes)) {
		bytes = asRead.dump(4);
	}
	std::ofstream out(std::filesystem::u8path(backupPath), std::ios::binary | std::ios::trunc);
	if (out) {
		out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		out.flush();
	}
	if (!out) {
		HostLog("[overlay] could not write " + backupPath + "; upgrading anyway");
		return;
	}
	HostLog("[overlay] kept the pre-upgrade overlays.json at " + backupPath);
}

bool OverlayStore::Save() const
{
	if (upgradeDeferred_) {
		// Writing anything writes the WHOLE document, in the current shape the widgets
		// already hold in memory -- which is exactly the upgrade Load() declined to commit.
		//
		// What that costs is uneven, and this is the honest account of it. Update, Fork and
		// ReturnToStock roll back and answer NotPersisted, so those three refuse where the
		// user can see it. Create, Delete, Duplicate, AddAsset and RemoveAsset ignore this
		// bool and report success: a widget made during a deferred session serves correctly,
		// then is simply not there at the next start. Load's warning is the only notice
		// those five give, which is why it spells the consequence out.
		//
		// Not logged here: SetPort alone would reach it once per boot.
		return false;
	}
	json arr = WidgetsJson();
	if (hold_.Skips(arr.dump())) {
		return true;
	}
	json root = json{{"version", kStoreVersion}, {"port", port_}, {"widgets", std::move(arr)}};
	return SaveStoreJson(root, path_);
}

json OverlayStore::WidgetsJson() const
{
	json arr = json::array();
	for (const Widget &w : widgets_) {
		arr.push_back(w.ToJson());
	}
	return arr;
}

OverlayStore &Store()
{
	static OverlayStore s;
	return s;
}

} // namespace Overlay
