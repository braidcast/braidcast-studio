#ifndef OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_

#include <cstdint>
#include <exception>
#include <string>

#include <nlohmann/json.hpp>

// Tolerant JSON field accessors shared by every platform integration (OAuth
// providers, chat transports, event transports). Each read defends against a
// missing key or a mis-typed value -- unofficial/reverse-engineered payloads may
// omit or re-type anything -- so a bad field degrades gracefully instead of
// throwing. Kept in ONE place so the semantics can't drift per translation unit
// (the drift these replace: int-only `Num` silently returned 0 for a stringified
// number while `NumLoose` parsed it).
namespace JsonUtil {

using json = nlohmann::json;

// Parse a response body into JSON, tolerating garbage (returns a null json on
// failure rather than throwing).
inline json ParseJson(const std::string &body)
{
	return json::parse(body, nullptr, false);
}

// Read a string field tolerantly: missing/non-string -> "".
inline std::string Str(const json &j, const char *key)
{
	if (!j.is_object()) {
		return std::string();
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) {
		return std::string();
	}
	return it->get<std::string>();
}

// Read a boolean field tolerantly: missing/non-bool -> `fallback` (default false).
inline bool Bool(const json &j, const char *key, bool fallback = false)
{
	if (!j.is_object()) {
		return fallback;
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_boolean()) {
		return fallback;
	}
	return it->get<bool>();
}

// Read an integer field that a platform may serialize either as a JSON number or,
// for 64-bit quantities (YouTube's amountMicros) or numeric-string ids, as a
// string. Missing / wrong-typed / unparseable -> `fallback` (default 0). This is
// the strict superset of the old int-only readers, so no caller regresses.
inline int64_t NumLoose(const json &j, const char *key, int64_t fallback = 0)
{
	if (!j.is_object()) {
		return fallback;
	}
	auto it = j.find(key);
	if (it == j.end()) {
		return fallback;
	}
	if (it->is_number()) {
		return it->get<int64_t>();
	}
	if (it->is_string()) {
		try {
			return std::stoll(it->get<std::string>());
		} catch (const std::exception &) {
			return fallback;
		}
	}
	return fallback;
}

// Return a reference to `j[key]` when `j` is an object holding it, else a shared
// null json -- lets nested-field accessors chain (Obj(Obj(msg,"payload"),"session"))
// without intermediate copies or per-hop null checks.
inline const json &Obj(const json &j, const char *key)
{
	static const json kNull = json(nullptr);
	if (!j.is_object()) {
		return kNull;
	}
	auto it = j.find(key);
	return it == j.end() ? kNull : *it;
}

} // namespace JsonUtil

#endif // OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_
