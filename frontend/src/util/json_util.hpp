#ifndef OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_
#define OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

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

// Read the first element of the array field at `key`: missing key, non-array, or
// empty array -> a null json. Platform list endpoints all answer with a
// single-element array for a by-id lookup, so this is how a caller reaches the one
// row it asked for without trusting the shape.
inline json First(const json &j, const char *key)
{
	if (!j.is_object()) {
		return json(nullptr);
	}
	auto it = j.find(key);
	if (it == j.end() || !it->is_array() || it->empty()) {
		return json(nullptr);
	}
	return (*it)[0];
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

// Copy the string at `src[key]` into `out[outKey]`, and ONLY when `src` actually carries a string
// there -- an absent or wrong-typed field leaves `out` untouched rather than writing "". Returns
// whether the copy happened, so a caller that owes an explanation for a field it could not read
// can tell "the platform said nothing" from "the platform said nothing was set".
//
// The scalar sibling of CopyStringList, load-bearing for the same reason: an invented "" reads as
// a value the platform stated and disagrees with every non-empty request, while a genuinely absent
// key reads as "not reported". An empty string that IS present is copied through -- that is a real
// answer ("nothing is set"), not the absence of one.
inline bool CopyString(const json &src, const char *key, json &out, const char *outKey)
{
	if (!src.is_object()) {
		return false;
	}
	auto it = src.find(key);
	if (it == src.end() || !it->is_string()) {
		return false;
	}
	out[outKey] = *it;
	return true;
}

// Copy the array-of-strings at `src[key]` into `out[outKey]`, and ONLY when `src` actually
// carries an array there -- an absent or wrong-typed field leaves `out` untouched rather than
// writing an empty list.
//
// The distinction is load-bearing wherever a bag is later compared against what was asked for:
// an invented empty list reads as "the platform holds none of these" and reports every requested
// entry as dropped, while a genuinely absent key reads as "not reported", which is the truth.
// Shared because every platform's own-channel read has the same list-shaped fields (tags,
// classification labels) and the same need to keep absence absent.
inline void CopyStringList(const json &src, const char *key, json &out, const char *outKey)
{
	const json &list = Obj(src, key);
	if (!list.is_array()) {
		return;
	}
	json kept = json::array();
	for (const json &entry : list) {
		if (entry.is_string()) {
			kept.push_back(entry);
		}
	}
	out[outKey] = std::move(kept);
}

} // namespace JsonUtil

#endif // OBS_MULTISTREAM_FRONTEND_JSON_UTIL_HPP_
