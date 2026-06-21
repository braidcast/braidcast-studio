#include "CanvasDefinition.hpp"

#include <util/dstr.h>

namespace {
video_format VideoFormatFromName(const std::string &name)
{
	if (astrcmpi(name.c_str(), "I420") == 0) {
		return VIDEO_FORMAT_I420;
	} else if (astrcmpi(name.c_str(), "NV12") == 0) {
		return VIDEO_FORMAT_NV12;
	} else if (astrcmpi(name.c_str(), "I444") == 0) {
		return VIDEO_FORMAT_I444;
	} else if (astrcmpi(name.c_str(), "I010") == 0) {
		return VIDEO_FORMAT_I010;
	} else if (astrcmpi(name.c_str(), "P010") == 0) {
		return VIDEO_FORMAT_P010;
	} else if (astrcmpi(name.c_str(), "P216") == 0) {
		return VIDEO_FORMAT_P216;
	} else if (astrcmpi(name.c_str(), "P416") == 0) {
		return VIDEO_FORMAT_P416;
	}
	return VIDEO_FORMAT_BGRA; // includes the editor's "RGB" data value
}

video_colorspace VideoColorSpaceFromName(const std::string &name)
{
	if (astrcmpi(name.c_str(), "601") == 0) {
		return VIDEO_CS_601;
	} else if (astrcmpi(name.c_str(), "709") == 0) {
		return VIDEO_CS_709;
	} else if (astrcmpi(name.c_str(), "2100PQ") == 0) {
		return VIDEO_CS_2100_PQ;
	} else if (astrcmpi(name.c_str(), "2100HLG") == 0) {
		return VIDEO_CS_2100_HLG;
	}
	return VIDEO_CS_SRGB;
}

video_range_type VideoRangeFromName(const std::string &name)
{
	return astrcmpi(name.c_str(), "Full") == 0 ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
}
}

static OBSDataAutoRelease EncoderToData(const CanvasEncoderDef &enc)
{
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_string(d, "id", enc.id.c_str());
	obs_data_set_bool(d, "use_default", enc.useDefault);
	if (enc.settings) {
		obs_data_set_obj(d, "settings", enc.settings);
	}
	return d;
}

static CanvasEncoderDef EncoderFromData(obs_data_t *parent, const char *key)
{
	CanvasEncoderDef enc;
	OBSDataAutoRelease d = obs_data_get_obj(parent, key);
	if (!d) {
		return enc;
	}
	enc.id = obs_data_get_string(d, "id");
	enc.useDefault = obs_data_get_bool(d, "use_default");
	enc.settings = obs_data_get_obj(d, "settings"); // null if absent
	return enc;
}

OBSDataAutoRelease CanvasDefinition::ToData() const
{
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_string(d, "uuid", uuid.c_str());
	obs_data_set_string(d, "name", name.c_str());
	obs_data_set_bool(d, "is_default", isDefault);

	obs_data_set_int(d, "width", width);
	obs_data_set_int(d, "height", height);
	obs_data_set_int(d, "fps_num", fpsNum);
	obs_data_set_int(d, "fps_den", fpsDen);
	obs_data_set_bool(d, "use_default_resolution", useDefaultResolution);

	obs_data_set_obj(d, "video_encoder", EncoderToData(video));
	obs_data_set_obj(d, "audio_encoder", EncoderToData(audio));

	OBSDataAutoRelease c = obs_data_create();
	obs_data_set_string(c, "format", color.format.c_str());
	obs_data_set_string(c, "space", color.space.c_str());
	obs_data_set_string(c, "range", color.range.c_str());
	obs_data_set_int(c, "sdr_white_level", color.sdrWhiteLevel);
	obs_data_set_int(c, "hdr_nominal_peak_level", color.hdrNominalPeakLevel);
	obs_data_set_bool(c, "use_default", color.useDefault);
	obs_data_set_obj(d, "color", c);

	return d;
}

CanvasDefinition CanvasDefinition::FromData(obs_data_t *data)
{
	CanvasDefinition def;
	def.uuid = obs_data_get_string(data, "uuid");
	def.name = obs_data_get_string(data, "name");
	def.isDefault = obs_data_get_bool(data, "is_default");

	/* obs_data_get_* return 0/"" for absent keys; guard resolution so a malformed
	 * entry can't create a 0x0 canvas. */
	def.width = (uint32_t)obs_data_get_int(data, "width");
	def.height = (uint32_t)obs_data_get_int(data, "height");
	def.fpsNum = (uint32_t)obs_data_get_int(data, "fps_num");
	def.fpsDen = (uint32_t)obs_data_get_int(data, "fps_den");
	if (def.width == 0 || def.height == 0) {
		def.width = 1920;
		def.height = 1080;
	}
	if (def.fpsNum == 0) {
		def.fpsNum = 60;
		def.fpsDen = 1;
	}
	def.useDefaultResolution = obs_data_get_bool(data, "use_default_resolution");

	def.video = EncoderFromData(data, "video_encoder");
	def.audio = EncoderFromData(data, "audio_encoder");

	OBSDataAutoRelease c = obs_data_get_obj(data, "color");
	if (c) {
		def.color.format = obs_data_get_string(c, "format");
		def.color.space = obs_data_get_string(c, "space");
		def.color.range = obs_data_get_string(c, "range");
		def.color.sdrWhiteLevel = (uint32_t)obs_data_get_int(c, "sdr_white_level");
		def.color.hdrNominalPeakLevel = (uint32_t)obs_data_get_int(c, "hdr_nominal_peak_level");
		def.color.useDefault = obs_data_get_bool(c, "use_default");
	}
	return def;
}

void CanvasDefinition::ToVideoInfo(struct obs_video_info &ovi, const CanvasDefinition *inheritFrom) const
{
	const CanvasDefinition *res = (useDefaultResolution && inheritFrom) ? inheritFrom : this;
	const CanvasColorDef &col = (color.useDefault && inheritFrom) ? inheritFrom->color : color;

	ovi = {}; // graphics_module left null; the caller fills it from the running pipeline
	ovi.base_width = res->width;
	ovi.base_height = res->height;
	ovi.output_width = res->width;   // unified: edit-surface == encode size
	ovi.output_height = res->height;
	ovi.fps_num = res->fpsNum;
	ovi.fps_den = res->fpsDen;
	ovi.gpu_conversion = true;  // GPU colorspace conversion, matching the main video pipeline
	ovi.output_format = VideoFormatFromName(col.format);
	ovi.colorspace = VideoColorSpaceFromName(col.space);
	ovi.range = VideoRangeFromName(col.range);
	ovi.scale_type = OBS_SCALE_BICUBIC;
}
