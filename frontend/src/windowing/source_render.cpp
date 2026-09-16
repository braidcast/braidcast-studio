#include "source_render.hpp"

#include <obs.hpp>

namespace SourceRender {

bool BaseSize(obs_source_t *source, float &baseCX, float &baseCY)
{
	if (!source) {
		return false;
	}
	const uint32_t w = obs_source_get_width(source);
	const uint32_t h = obs_source_get_height(source);
	if (w > 0 && h > 0) {
		baseCX = float(w);
		baseCY = float(h);
		return true;
	}
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return false;
	}
	baseCX = float(ovi.base_width);
	baseCY = float(ovi.base_height);
	return true;
}

void Letterboxed(obs_source_t *source, uint32_t cx, uint32_t cy)
{
	float baseCX = 0.0f;
	float baseCY = 0.0f;
	if (!BaseSize(source, baseCX, baseCY)) {
		return;
	}
	if (baseCX <= 0.0f || baseCY <= 0.0f || cx == 0 || cy == 0) {
		return;
	}

	const float scale = (float(cx) / baseCX < float(cy) / baseCY) ? float(cx) / baseCX : float(cy) / baseCY;
	const int drawCX = int(baseCX * scale);
	const int drawCY = int(baseCY * scale);
	const int drawX = (int(cx) - drawCX) / 2;
	const int drawY = (int(cy) - drawCY) / 2;

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, baseCX, 0.0f, baseCY, -100.0f, 100.0f);
	gs_set_viewport(drawX, drawY, drawCX, drawCY);
	obs_source_video_render(source);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

obs_source_t *CreateTextLabel(const char *name, const char *text, int fontSize, obs_data_t *extra)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

	obs_data_set_string(font, "face", "Arial");
	obs_data_set_int(font, "flags", 1); // bold
	obs_data_set_int(font, "size", fontSize);

	obs_data_set_obj(settings, "font", font);
	obs_data_set_string(settings, "text", text ? text : "");
	obs_data_set_bool(settings, "outline", true);
	if (extra) {
		obs_data_apply(settings, extra);
	}

	// Windows registers "text_gdiplus" (verified in plugins/obs-text/gdiplus/obs-text.cpp).
	return obs_source_create_private("text_gdiplus", name, settings);
}

} // namespace SourceRender
