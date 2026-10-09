#ifndef OBS_MULTISTREAM_FRONTEND_SETTINGS_FIELDS_HPP_
#define OBS_MULTISTREAM_FRONTEND_SETTINGS_FIELDS_HPP_

#include <obs.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

// Field-descriptor tables for a flat settings struct: the single source for the wire
// (camelCase JSON) <-> file (snake_case obs_data) <-> member mapping. Load, Save,
// ToJson and ApplyPatch all iterate one Table, so persistence and the bridge cannot
// drift. A string field may carry an allowed-value list (an enum on the wire) and a
// length cap; a double field carries its clamp range.
namespace SettingsFields {

template<class T> struct BoolField {
	const char *json;
	const char *file;
	bool T::*member;
};

template<class T> struct StringField {
	const char *json;
	const char *file;
	std::string T::*member;
	const char *const *allowed; // nullptr = free text
	size_t allowedCount;
	size_t maxLen; // 0 = unlimited
};

template<class T> struct DoubleField {
	const char *json;
	const char *file;
	double T::*member;
	double min;
	double max;
};

template<class T> struct Table {
	const BoolField<T> *bools;
	size_t boolCount;
	const StringField<T> *strings;
	size_t stringCount;
	const DoubleField<T> *doubles;
	size_t doubleCount;
};

inline double Clamp(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

template<class T> bool StringAllowed(const StringField<T> &f, const std::string &v)
{
	if (f.maxLen != 0 && v.size() > f.maxLen) {
		return false;
	}
	if (!f.allowed) {
		return true;
	}
	for (size_t i = 0; i < f.allowedCount; ++i) {
		if (v == f.allowed[i]) {
			return true;
		}
	}
	return false;
}

// Missing keys keep `out`'s current values (the struct defaults on a fresh struct).
// A stored string that is no longer allowed also keeps the default, so a value a later
// build retires cannot wedge the setting.
template<class T> void Load(const Table<T> &t, obs_data_t *root, T &out)
{
	for (size_t i = 0; i < t.boolCount; ++i) {
		const BoolField<T> &f = t.bools[i];
		obs_data_set_default_bool(root, f.file, out.*f.member);
		out.*f.member = obs_data_get_bool(root, f.file);
	}
	for (size_t i = 0; i < t.stringCount; ++i) {
		const StringField<T> &f = t.strings[i];
		obs_data_set_default_string(root, f.file, (out.*f.member).c_str());
		const std::string v = obs_data_get_string(root, f.file);
		if (StringAllowed(f, v)) {
			out.*f.member = v;
		}
	}
	for (size_t i = 0; i < t.doubleCount; ++i) {
		const DoubleField<T> &f = t.doubles[i];
		obs_data_set_default_double(root, f.file, out.*f.member);
		out.*f.member = Clamp(obs_data_get_double(root, f.file), f.min, f.max);
	}
}

template<class T> void Save(const Table<T> &t, obs_data_t *root, const T &in)
{
	for (size_t i = 0; i < t.boolCount; ++i) {
		obs_data_set_bool(root, t.bools[i].file, in.*t.bools[i].member);
	}
	for (size_t i = 0; i < t.stringCount; ++i) {
		obs_data_set_string(root, t.strings[i].file, (in.*t.strings[i].member).c_str());
	}
	for (size_t i = 0; i < t.doubleCount; ++i) {
		obs_data_set_double(root, t.doubles[i].file, in.*t.doubles[i].member);
	}
}

template<class T> nlohmann::json ToJson(const Table<T> &t, const T &in)
{
	nlohmann::json out = nlohmann::json::object();
	for (size_t i = 0; i < t.boolCount; ++i) {
		out[t.bools[i].json] = in.*t.bools[i].member;
	}
	for (size_t i = 0; i < t.stringCount; ++i) {
		out[t.strings[i].json] = in.*t.strings[i].member;
	}
	for (size_t i = 0; i < t.doubleCount; ++i) {
		out[t.doubles[i].json] = in.*t.doubles[i].member;
	}
	return out;
}

// Apply only the present keys of the matching JSON type; unknown keys and wrong types
// are ignored. A string outside its allowed set or length cap fails the whole patch
// with `error` set and `inOut` untouched. Doubles clamp.
template<class T> bool ApplyPatch(const Table<T> &t, const nlohmann::json &patch, T &inOut, std::string &error)
{
	if (!patch.is_object()) {
		error = "expected an object";
		return false;
	}
	T next = inOut;
	for (size_t i = 0; i < t.boolCount; ++i) {
		auto it = patch.find(t.bools[i].json);
		if (it != patch.end() && it->is_boolean()) {
			next.*t.bools[i].member = it->template get<bool>();
		}
	}
	for (size_t i = 0; i < t.stringCount; ++i) {
		const StringField<T> &f = t.strings[i];
		auto it = patch.find(f.json);
		if (it != patch.end() && it->is_string()) {
			const std::string v = it->template get<std::string>();
			if (!StringAllowed(f, v)) {
				error = std::string("invalid value for '") + f.json + "'";
				return false;
			}
			next.*f.member = v;
		}
	}
	for (size_t i = 0; i < t.doubleCount; ++i) {
		const DoubleField<T> &f = t.doubles[i];
		auto it = patch.find(f.json);
		if (it != patch.end() && it->is_number()) {
			next.*f.member = Clamp(it->template get<double>(), f.min, f.max);
		}
	}
	inOut = next;
	return true;
}

} // namespace SettingsFields

#endif // OBS_MULTISTREAM_FRONTEND_SETTINGS_FIELDS_HPP_
