#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_TEMPLATE_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_TEMPLATE_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Overlay {

using json = nlohmann::json;

// Where a type's shipped default template lives: the single place that layout is spelled
// out, so a read and the log line that quotes the path cannot name different directories.
std::string TemplateDir(const std::string &type);

// The browser-source rectangle `type` is designed at, in pixels, written to `w`/`h`. False
// for a type with no shipped template, leaving both untouched.
//
// A browser source's viewport is the only box its stylesheet can measure itself against --
// the scene-item transform that scales the rendered bitmap is invisible to CSS -- so this
// is the size at which a stock template resolves its root scale to exactly 1rem = 16 design
// px. It is a property of the type in the same way TemplateDir is, and it is decided by the
// widget's own stylesheet: the divisors in that template's
// `html { font-size: min(100vw / <width>, 100vh / <height>) }` knob are what put 1rem at 16.
// Change a knob and this answer changes with it.
bool NaturalSize(const std::string &type, uint32_t &w, uint32_t &h);

// Every shipped template directory NaturalSize has no answer for, empty when the table
// covers them all. The table is data and nothing links it to the directories it describes,
// so a twelfth type added under default-<type>/ without a row would be created at the
// canvas resolution and drawn at fifteen times its design scale -- which reads as a broken
// template rather than as a missing row, and is found on stream. The overlay self-test asks
// this against the staged rundir, so the omission is named before a build ships rather than
// after someone adds the source.
std::vector<std::string> TypesMissingNaturalSize();

// What reading a type's shipped template yielded. The three failures are kept apart
// because they are not the same risk, and two of them are not even the same KIND of fact:
// Absent and Corrupt describe what is on disk, while Partial describes how one read
// attempt went and may well not be true of the next one.
//
// Partial keeps whatever did read, but the template must not be used: a stock widget of
// that type would serve an empty stylesheet, and a fork would copy one into the user's
// own code permanently.
//
// Ok means every one of the four reads RETURNED, which is weaker than "the bytes are
// whole". FileUtil::ReadBinaryFile reports success for a read that dies partway and comes
// back short -- its own header says so -- and nothing here can tell that apart from a
// small file. Closing that door means changing the shared helper, so until then read Ok as
// "all four opened and read back", never as an integrity guarantee.
enum class TemplateStatus {
	Ok,      // all four reads returned, and fields.json parsed as a field list
	Absent,  // none of it is there: an unknown or legacy type
	Partial, // some of it read and some did not: a missing file, or a momentary lock
	Corrupt, // all four read, but fields.json is not a field list
};

// One widget type's shipped template, exactly as it is on disk. `schema` is fields.json:
// per entry {key,type,label,default} plus the type-specific extras (options|min|max|step).
// It carries no values -- a value belongs to a widget, not to its type.
struct TypeTemplate {
	TemplateStatus status = TemplateStatus::Absent;
	std::string html;
	std::string css;
	std::string js;
	json schema = json::array();
};

// The shipped template for `type`. An Ok is cached for the rest of the process: templates
// are staged into the rundir by the build, so their content cannot change under a running
// app and there is nothing to invalidate. Every other outcome is re-read on the next ask,
// because it is a read attempt's outcome rather than a fact about the template -- a
// momentarily locked file must not leave a type broken for the whole session.
//
// By value, not by reference, precisely because of that re-read: a reference into the
// cache could not be promised to outlive the next caller's retry.
//
// It never sleeps or retries on the caller's behalf, because this runs on the request path
// while a document is being assembled for a live browser source, and a request can simply
// be made again. A caller for which one attempt's answer is not good enough retries it
// itself -- the overlays.json upgrade does, being one-shot; see FirstTypeStillPartial.
TypeTemplate TemplateFor(const std::string &type);

// {key: value} for one field schema: every key the schema declares, taking `settings`'
// override where there is one and the schema's own `default` where there is not. The
// single definition of what a widget's page receives, so the served document and what the
// editor is told the page receives cannot drift apart.
json MergeSettings(const json &schema, const json &settings);

// The `default` `schema` declares for `key`, or null when it declares no such key. Null is
// the honest answer for both an absent key and one whose entry omits a default: neither
// names a value the widget would fall back to.
json SchemaDefault(const json &schema, const std::string &key);

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_TEMPLATE_HPP_
