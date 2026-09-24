#include "overlay_template.hpp"

#include "../log.hpp"
#include "util/file_util.hpp"
#include "util/web_bundle.hpp"

#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace Overlay {

namespace {

std::mutex g_cacheMutex;
std::map<std::string, TypeTemplate> g_cache;

// The one spelling of where templates live and how a type's directory is named, so the
// read below and the sweep at the bottom of this file cannot come to disagree about either.
constexpr char kTypeDirPrefix[] = "default-";

std::string TemplateRoot()
{
	// Off WebBundle::Root() rather than a second spelling of the rundir layout: this is
	// the same tree scheme.cpp serves at app://app/, so the two cannot name different
	// directories after one of them moves.
	return WebBundle::Root() + "/overlay";
}

// One row per shipped widget type: the browser-source rectangle it is designed at, in
// pixels. A data table rather than a branch, so a new type is one row.
//
// The knob in each template's stylesheet is what decides these, since it is what puts 1rem
// at 16 design px: labels resolves min(100vw / 24, 100vh / 3.375), so its terms give
// exactly 16 at 384x54 px. Each row is exact on the axis that BINDS there and generous on
// the other, which is why most of the widths below are more than the width term alone would
// give. Nine of these are bars and lists, bound by height: the height is the divisor times
// 16 and the width is the shape a streamer actually draws one at, far enough past the width
// term that a longer line or a wider name has room before the type starts shrinking to fit.
// Two invert it -- the alert card is centred and sized by its message, and the chat column
// wraps to its width -- so those two are exact in width and generous in height, their
// height terms being guards (a card that would fall off the bottom, a column too short to
// hold three messages) rather than fits.
//
// The ticker is the one where "generous" was tempting to read as the whole canvas. It is
// not: a belt is height-bound, so widening one costs a streamer nothing and they stretch it
// to whatever they want, whereas a 1920-wide row here would make the editor's preview --
// which fits this rectangle into its pane -- draw 20-design-px type at six device px. 640
// keeps it in the same proportion to its width term as the other ten.
//
// `replay` rides along as a second column because this table is the only registry of widget
// types that exists in C++, and the rule it encodes -- whether the type accepts a REPLAYED
// event (events.replay) -- has to be STATED per type rather than inferred from anything
// else here. Only the alert box takes one: of the four templates that implement onEvent at
// all, the other three accumulate (a goal's total, the labels' last-of-each, the recent-
// events belt), and a replay must not be countable as a second real occurrence on stream;
// the remaining seven never look at an event. Forking does not change a widget's `type`, so
// a fork follows its type's row like any other widget of it.
//
// False is the answer for a type with no row AND for a row that forgets the column (it
// value-initializes), and that is deliberate: an absent type is a document from a newer
// version or a broken widget, never one a user authored -- a user can fork a widget, not
// author a type -- so accepting it would report a delivery nothing can show. A new type
// added without a row is named by TypesMissingNaturalSize below before a build ships.
//
// `audio` is the third column, and the one place a type declares whether its stock page can
// make sound (PlaysAudio). Only the alert box can: it is the one template that calls
// OBSOverlay.playSound, and none of the others creates an Audio element or an AudioContext.
// Plays is the enum's zero, so a row that leaves the column off stays in the mixer: a silent
// type wrongly kept there costs a mixer row, while a sound-playing type wrongly marked silent
// would drop its sound off the stream without a word.
enum class PageAudio { Plays, Silent };

struct TypeRow {
	const char *type;
	uint32_t w;
	uint32_t h;
	bool replay;
	PageAudio audio;
};

constexpr PageAudio kPlays = PageAudio::Plays;
constexpr PageAudio kSilent = PageAudio::Silent;

constexpr TypeRow kTypeRows[] = {
	{"alertbox", 600, 400, true, kPlays},          {"chatbox", 400, 480, false, kSilent},
	{"chatleaderboard", 340, 191, false, kSilent}, {"countdown", 300, 54, false, kSilent},
	{"followercount", 640, 58, false, kSilent},    {"goalbar", 600, 76, false, kSilent},
	{"labels", 600, 54, false, kSilent},           {"ticker", 640, 24, false, kSilent},
	{"uptime", 300, 54, false, kSilent},           {"viewercount", 400, 58, false, kSilent},
	{"wheretowatch", 320, 174, false, kSilent},
};

// `type`'s row, or null when the table has none.
const TypeRow *FindTypeRow(const std::string &type)
{
	for (const TypeRow &row : kTypeRows) {
		if (type == row.type) {
			return &row;
		}
	}
	return nullptr;
}

TypeTemplate ReadTemplate(const std::string &type)
{
	TypeTemplate t;
	const std::string dir = TemplateDir(type);
	std::string fieldsJson;
	// One row per file a template is made of, so "complete" is a list rather than four
	// hand-written reads and a conjunction someone has to remember to extend. Every
	// result is checked: a read whose outcome is discarded is a failure that reaches the
	// user as an empty stylesheet with nothing in the log.
	std::string unread;
	size_t read = 0;
	for (const auto &[name, out] : std::initializer_list<std::pair<const char *, std::string *>>{
		     {"template.html", &t.html},
		     {"template.css", &t.css},
		     {"template.js", &t.js},
		     {"fields.json", &fieldsJson},
	     }) {
		if (FileUtil::ReadUtf8File(dir + name, *out)) {
			++read;
			continue;
		}
		unread += unread.empty() ? "" : ", ";
		unread += name;
	}

	if (read == 0) {
		t.status = TemplateStatus::Absent;
		return t;
	}
	if (!unread.empty()) {
		// Named individually because the usual cause is one file, momentarily: knowing
		// which one is the difference between "a bad install dropped template.css" and
		// "an antivirus pass held it for a moment".
		HostLog("[overlay] template for type '" + type + "' at " + dir + " is incomplete (could not read " +
			unread + ")");
		t.status = TemplateStatus::Partial;
		return t;
	}

	json parsed;
	try {
		parsed = json::parse(fieldsJson);
	} catch (const std::exception &e) {
		HostLog("[overlay] fields.json for type '" + type + "' is unparseable (" + e.what() + ")");
		t.status = TemplateStatus::Corrupt;
		return t;
	}
	// A file that parses but is not an array carries no field list either, and it reaches
	// a caller as the same empty result an unparseable one would.
	if (!parsed.is_array()) {
		HostLog("[overlay] fields.json for type '" + type + "' is not an array");
		t.status = TemplateStatus::Corrupt;
		return t;
	}
	t.schema = std::move(parsed);
	t.status = TemplateStatus::Ok;
	return t;
}

} // namespace

std::string TemplateDir(const std::string &type)
{
	return TemplateRoot() + "/" + kTypeDirPrefix + type + "/";
}

bool NaturalSize(const std::string &type, uint32_t &w, uint32_t &h)
{
	const TypeRow *row = FindTypeRow(type);
	if (row == nullptr) {
		return false;
	}
	w = row->w;
	h = row->h;
	return true;
}

bool AcceptsReplay(const std::string &type)
{
	// No row, including the empty type a widget with a broken document carries: rejected,
	// so `delivered` never counts a widget that was never going to show the frame.
	const TypeRow *row = FindTypeRow(type);
	return row != nullptr && row->replay;
}

bool PlaysAudio(const std::string &type)
{
	// No row answers the opposite way to AcceptsReplay, and for the same reason turned
	// around: a type nothing here describes is a page nothing here has read, so it keeps
	// the mixer channel its sound would need.
	const TypeRow *row = FindTypeRow(type);
	return row == nullptr || row->audio == PageAudio::Plays;
}

std::vector<std::string> TypesMissingNaturalSize()
{
	std::vector<std::string> missing;
	std::error_code ec;
	// The shipped directories are the only list of types that exists at runtime, which is
	// exactly why this sweep reads them rather than a second list someone would have to
	// remember to extend alongside the one it is guarding.
	// Stepped by hand rather than with a range-for so that every filesystem call here takes
	// its error_code overload. The range-for form only routes the CONSTRUCTOR through `ec`;
	// is_directory() and the increment are the throwing overloads, and this runs inside the
	// self-test battery, where an escaping filesystem_error would take down the whole run
	// over a directory that was briefly unreadable.
	const std::filesystem::directory_iterator end;
	std::filesystem::directory_iterator it(TemplateRoot(), ec);
	for (; !ec && it != end; it.increment(ec)) {
		if (!it->is_directory(ec) || ec) {
			continue;
		}
		const std::string name = it->path().filename().string();
		if (name.rfind(kTypeDirPrefix, 0) != 0) {
			continue;
		}
		const std::string type = name.substr(sizeof(kTypeDirPrefix) - 1);
		uint32_t w = 0;
		uint32_t h = 0;
		if (!NaturalSize(type, w, h)) {
			missing.push_back(type);
		}
	}
	// A directory that cannot be read answers "nothing missing" rather than "every type
	// missing": this is a guard against a forgotten table row, and it must not turn a
	// momentarily unreadable rundir into a failing self-test that names eleven types. This
	// discards a partial sweep too -- half the directories read is not evidence about the
	// half that did not.
	return ec ? std::vector<std::string>() : missing;
}

TypeTemplate TemplateFor(const std::string &type)
{
	// Read under the lock so two threads asking for the same unread type do not both read
	// it. The read is four small files off the local rundir, and only the first ask per
	// type pays for it.
	std::lock_guard<std::mutex> lock(g_cacheMutex);
	const auto cached = g_cache.find(type);
	if (cached != g_cache.end()) {
		return cached->second;
	}
	TypeTemplate t = ReadTemplate(type);
	DBG(LogCat::Overlay, "template for type '%s' read (status=%d)", type.c_str(), static_cast<int>(t.status));
	if (t.status != TemplateStatus::Ok) {
		// Deliberately not kept. An incomplete or unparseable read is a fact about one
		// attempt, and caching it would turn a file locked for a moment into a type that
		// serves unstyled for the rest of the session -- and into an empty stylesheet
		// written permanently into the next fork of it.
		return t;
	}
	return g_cache.emplace(type, std::move(t)).first->second;
}

json SchemaDefault(const json &schema, const std::string &key)
{
	if (!schema.is_array()) {
		return json(nullptr);
	}
	for (const json &f : schema) {
		if (!f.is_object()) {
			continue;
		}
		const auto keyIt = f.find("key");
		if (keyIt != f.end() && keyIt->is_string() && keyIt->get<std::string>() == key) {
			return f.value("default", json(nullptr));
		}
	}
	return json(nullptr);
}

json MergeSettings(const json &schema, const json &settings)
{
	json out = json::object();
	if (!schema.is_array()) {
		return out;
	}
	for (const json &f : schema) {
		if (!f.is_object()) {
			continue;
		}
		const auto keyIt = f.find("key");
		if (keyIt == f.end() || !keyIt->is_string()) {
			continue;
		}
		const std::string key = keyIt->get<std::string>();
		// find() answers end() for any non-object, so a widget whose settings were
		// stored as something else simply has no overrides rather than throwing.
		const auto override_ = settings.find(key);
		out[key] = override_ != settings.end() ? *override_ : f.value("default", json(nullptr));
	}
	return out;
}

} // namespace Overlay
