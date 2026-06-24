#include "properties_serializer.hpp"

#include <obs-properties.h>

namespace PropertiesSerializer {

namespace {

// Stable string names for each enum, kept next to libobs's enum order so adding
// a new variant is one row, not a switch arm.

const char *PropertyTypeName(obs_property_type type)
{
	switch (type) {
	case OBS_PROPERTY_BOOL:
		return "bool";
	case OBS_PROPERTY_INT:
		return "int";
	case OBS_PROPERTY_FLOAT:
		return "float";
	case OBS_PROPERTY_TEXT:
		return "text";
	case OBS_PROPERTY_PATH:
		return "path";
	case OBS_PROPERTY_LIST:
		return "list";
	case OBS_PROPERTY_COLOR:
		return "color";
	case OBS_PROPERTY_BUTTON:
		return "button";
	case OBS_PROPERTY_FONT:
		return "font";
	case OBS_PROPERTY_EDITABLE_LIST:
		return "editable_list";
	case OBS_PROPERTY_FRAME_RATE:
		return "frame_rate";
	case OBS_PROPERTY_GROUP:
		return "group";
	case OBS_PROPERTY_COLOR_ALPHA:
		return "color_alpha";
	case OBS_PROPERTY_INVALID:
	default:
		return "invalid";
	}
}

const char *NumberTypeName(obs_number_type type)
{
	return type == OBS_NUMBER_SLIDER ? "slider" : "scroller";
}

const char *TextTypeName(obs_text_type type)
{
	switch (type) {
	case OBS_TEXT_PASSWORD:
		return "password";
	case OBS_TEXT_MULTILINE:
		return "multiline";
	case OBS_TEXT_INFO:
		return "info";
	case OBS_TEXT_DEFAULT:
	default:
		return "default";
	}
}

const char *TextInfoTypeName(obs_text_info_type type)
{
	switch (type) {
	case OBS_TEXT_INFO_WARNING:
		return "warning";
	case OBS_TEXT_INFO_ERROR:
		return "error";
	case OBS_TEXT_INFO_NORMAL:
	default:
		return "normal";
	}
}

const char *EditableListTypeName(obs_editable_list_type type)
{
	switch (type) {
	case OBS_EDITABLE_LIST_TYPE_FILES:
		return "files";
	case OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS:
		return "files_and_urls";
	case OBS_EDITABLE_LIST_TYPE_STRINGS:
	default:
		return "strings";
	}
}

const char *PathTypeName(obs_path_type type)
{
	switch (type) {
	case OBS_PATH_FILE_SAVE:
		return "file_save";
	case OBS_PATH_DIRECTORY:
		return "directory";
	case OBS_PATH_FILE:
	default:
		return "file";
	}
}

const char *ComboFormatName(obs_combo_format format)
{
	switch (format) {
	case OBS_COMBO_FORMAT_INT:
		return "int";
	case OBS_COMBO_FORMAT_FLOAT:
		return "float";
	case OBS_COMBO_FORMAT_STRING:
		return "string";
	case OBS_COMBO_FORMAT_BOOL:
		return "bool";
	case OBS_COMBO_FORMAT_INVALID:
	default:
		return "invalid";
	}
}

const char *ComboTypeName(obs_combo_type type)
{
	switch (type) {
	case OBS_COMBO_TYPE_EDITABLE:
		return "editable";
	case OBS_COMBO_TYPE_RADIO:
		return "radio";
	case OBS_COMBO_TYPE_LIST:
	default:
		return "list";
	}
}

const char *GroupTypeName(obs_group_type type)
{
	return type == OBS_GROUP_CHECKABLE ? "checkable" : "normal";
}

const char *ButtonTypeName(obs_button_type type)
{
	return type == OBS_BUTTON_URL ? "url" : "default";
}

// A short-string -> json helper that maps null C strings to JSON null and empty
// strings to "" (callers decide which they want).
json Str(const char *s)
{
	return s ? json(std::string(s)) : json(nullptr);
}

void SerializeListItems(obs_property_t *prop, json &out)
{
	const obs_combo_format format = obs_property_list_format(prop);
	const size_t count = obs_property_list_item_count(prop);

	json items = json::array();
	for (size_t i = 0; i < count; i++) {
		json value;
		switch (format) {
		case OBS_COMBO_FORMAT_INT:
			value = static_cast<int64_t>(obs_property_list_item_int(prop, i));
			break;
		case OBS_COMBO_FORMAT_FLOAT:
			value = obs_property_list_item_float(prop, i);
			break;
		case OBS_COMBO_FORMAT_BOOL:
			value = obs_property_list_item_bool(prop, i);
			break;
		case OBS_COMBO_FORMAT_STRING:
		default: {
			const char *s = obs_property_list_item_string(prop, i);
			value = s ? json(std::string(s)) : json("");
			break;
		}
		}
		items.push_back(json{
			{"name", Str(obs_property_list_item_name(prop, i))},
			{"value", value},
			{"disabled", obs_property_list_item_disabled(prop, i)},
		});
	}
	out["items"] = std::move(items);
}

// Forward declaration; groups recurse back into the per-property serializer.
void SerializeProperty(obs_property_t *prop, obs_data_t *settings, json &out);

// Read the live value for `prop` from `settings` and stuff it under "value".
// Mirrors the legacy Qt renderer's per-type obs_data getters so the UI binds the
// same data the source would read. Composite types (font/editable_list/
// frame_rate) are emitted as their raw obs_data sub-object via JSON best-effort.
void SerializeValue(obs_property_t *prop, obs_data_t *settings, json &out)
{
	const char *name = obs_property_name(prop);
	const obs_property_type type = obs_property_get_type(prop);

	switch (type) {
	case OBS_PROPERTY_BOOL:
		out["value"] = obs_data_get_bool(settings, name);
		break;
	case OBS_PROPERTY_INT:
		out["value"] = static_cast<int64_t>(obs_data_get_int(settings, name));
		break;
	case OBS_PROPERTY_FLOAT:
		out["value"] = obs_data_get_double(settings, name);
		break;
	case OBS_PROPERTY_COLOR:
	case OBS_PROPERTY_COLOR_ALPHA:
		// Color is stored as a packed ABGR int; the UI reads/writes it as an
		// integer to round-trip losslessly.
		out["value"] = static_cast<int64_t>(obs_data_get_int(settings, name));
		break;
	case OBS_PROPERTY_TEXT:
	case OBS_PROPERTY_PATH:
		out["value"] = Str(obs_data_get_string(settings, name));
		break;
	case OBS_PROPERTY_LIST: {
		switch (obs_property_list_format(prop)) {
		case OBS_COMBO_FORMAT_INT:
			out["value"] = static_cast<int64_t>(obs_data_get_int(settings, name));
			break;
		case OBS_COMBO_FORMAT_FLOAT:
			out["value"] = obs_data_get_double(settings, name);
			break;
		case OBS_COMBO_FORMAT_BOOL:
			out["value"] = obs_data_get_bool(settings, name);
			break;
		case OBS_COMBO_FORMAT_STRING:
		default:
			out["value"] = Str(obs_data_get_string(settings, name));
			break;
		}
		break;
	}
	case OBS_PROPERTY_FONT:
	case OBS_PROPERTY_FRAME_RATE: {
		// Composite obj-valued types: emit the raw obs_data sub-object as JSON
		// so the UI can at least display it (full editing is a TODO).
		obs_data_t *obj = obs_data_get_obj(settings, name);
		if (obj) {
			const char *raw = obs_data_get_json(obj);
			out["value"] = raw ? json::parse(raw, nullptr, false) : json(nullptr);
			obs_data_release(obj);
		} else {
			out["value"] = json(nullptr);
		}
		break;
	}
	case OBS_PROPERTY_EDITABLE_LIST: {
		// Array of {value, selected, hidden, uuid} objects.
		obs_data_array_t *array = obs_data_get_array(settings, name);
		json arr = json::array();
		const size_t count = array ? obs_data_array_count(array) : 0;
		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);
			arr.push_back(json{
				{"value", Str(obs_data_get_string(item, "value"))},
				{"selected", obs_data_get_bool(item, "selected")},
				{"hidden", obs_data_get_bool(item, "hidden")},
				{"uuid", Str(obs_data_get_string(item, "uuid"))},
			});
			obs_data_release(item);
		}
		if (array) {
			obs_data_array_release(array);
		}
		out["value"] = std::move(arr);
		break;
	}
	case OBS_PROPERTY_GROUP:
		// A checkable group's own bool lives under its name; normal groups have
		// no scalar value of their own.
		if (obs_property_group_type(prop) == OBS_GROUP_CHECKABLE) {
			out["value"] = obs_data_get_bool(settings, name);
		}
		break;
	case OBS_PROPERTY_BUTTON:
	case OBS_PROPERTY_INVALID:
	default:
		break;
	}
}

void SerializeProperty(obs_property_t *prop, obs_data_t *settings, json &out)
{
	const obs_property_type type = obs_property_get_type(prop);

	out["name"] = Str(obs_property_name(prop));
	out["type"] = PropertyTypeName(type);
	out["label"] = Str(obs_property_description(prop));
	out["enabled"] = obs_property_enabled(prop);
	out["visible"] = obs_property_visible(prop);

	const char *longDesc = obs_property_long_description(prop);
	if (longDesc) {
		out["long_description"] = std::string(longDesc);
	}

	switch (type) {
	case OBS_PROPERTY_INT: {
		out["min"] = obs_property_int_min(prop);
		out["max"] = obs_property_int_max(prop);
		out["step"] = obs_property_int_step(prop);
		out["int_type"] = NumberTypeName(obs_property_int_type(prop));
		out["suffix"] = Str(obs_property_int_suffix(prop));
		break;
	}
	case OBS_PROPERTY_FLOAT: {
		out["min"] = obs_property_float_min(prop);
		out["max"] = obs_property_float_max(prop);
		out["step"] = obs_property_float_step(prop);
		out["float_type"] = NumberTypeName(obs_property_float_type(prop));
		out["suffix"] = Str(obs_property_float_suffix(prop));
		break;
	}
	case OBS_PROPERTY_TEXT: {
		const obs_text_type tt = obs_property_text_type(prop);
		out["text_type"] = TextTypeName(tt);
		out["monospace"] = obs_property_text_monospace(prop);
		if (tt == OBS_TEXT_INFO) {
			out["info_type"] = TextInfoTypeName(obs_property_text_info_type(prop));
			out["info_word_wrap"] = obs_property_text_info_word_wrap(prop);
		}
		break;
	}
	case OBS_PROPERTY_PATH: {
		out["path_type"] = PathTypeName(obs_property_path_type(prop));
		out["filter"] = Str(obs_property_path_filter(prop));
		out["default_path"] = Str(obs_property_path_default_path(prop));
		break;
	}
	case OBS_PROPERTY_LIST: {
		out["combo_format"] = ComboFormatName(obs_property_list_format(prop));
		out["combo_type"] = ComboTypeName(obs_property_list_type(prop));
		SerializeListItems(prop, out);
		break;
	}
	case OBS_PROPERTY_BUTTON: {
		out["button_type"] = ButtonTypeName(obs_property_button_type(prop));
		const char *url = obs_property_button_url(prop);
		if (url && *url) {
			out["url"] = std::string(url);
		}
		break;
	}
	case OBS_PROPERTY_GROUP: {
		out["group_type"] = GroupTypeName(obs_property_group_type(prop));
		json nested = json::array();
		obs_properties_t *content = obs_property_group_content(prop);
		obs_property_t *el = obs_properties_first(content);
		while (el) {
			json child = json::object();
			SerializeProperty(el, settings, child);
			nested.push_back(std::move(child));
			obs_property_next(&el);
		}
		out["props"] = std::move(nested);
		break;
	}
	case OBS_PROPERTY_EDITABLE_LIST: {
		out["editable_list_type"] = EditableListTypeName(obs_property_editable_list_type(prop));
		out["filter"] = Str(obs_property_editable_list_filter(prop));
		out["default_path"] = Str(obs_property_editable_list_default_path(prop));
		break;
	}
	case OBS_PROPERTY_FRAME_RATE: {
		json options = json::array();
		const size_t optCount = obs_property_frame_rate_options_count(prop);
		for (size_t i = 0; i < optCount; i++) {
			options.push_back(json{
				{"name", Str(obs_property_frame_rate_option_name(prop, i))},
				{"description", Str(obs_property_frame_rate_option_description(prop, i))},
			});
		}
		out["fps_options"] = std::move(options);

		json ranges = json::array();
		const size_t rangeCount = obs_property_frame_rate_fps_ranges_count(prop);
		for (size_t i = 0; i < rangeCount; i++) {
			const media_frames_per_second min = obs_property_frame_rate_fps_range_min(prop, i);
			const media_frames_per_second max = obs_property_frame_rate_fps_range_max(prop, i);
			ranges.push_back(json{
				{"min", json{{"numerator", min.numerator}, {"denominator", min.denominator}}},
				{"max", json{{"numerator", max.numerator}, {"denominator", max.denominator}}},
			});
		}
		out["fps_ranges"] = std::move(ranges);
		break;
	}
	default:
		break;
	}

	SerializeValue(prop, settings, out);
}

} // namespace

json SerializeProperties(obs_properties_t *props, obs_data_t *settings)
{
	json arr = json::array();
	if (!props) {
		return arr;
	}
	obs_property_t *prop = obs_properties_first(props);
	while (prop) {
		json descriptor = json::object();
		SerializeProperty(prop, settings, descriptor);
		arr.push_back(std::move(descriptor));
		obs_property_next(&prop);
	}
	return arr;
}

} // namespace PropertiesSerializer
