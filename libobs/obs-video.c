/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <time.h>
#include <stdlib.h>

#include "obs.h"
#include "obs-internal.h"
#include "graphics/vec4.h"
#include "media-io/format-conversion.h"
#include "media-io/video-frame.h"
#include "util/source-profiler.h"
#include "util/util_uint64.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static uint64_t tick_sources(uint64_t cur_time, uint64_t last_time)
{
	struct obs_core_data *data = &obs->data;
	struct obs_source *source;
	uint64_t delta_time;
	float seconds;

	if (!last_time) {
		last_time = cur_time - obs->video.video_frame_interval_ns;
	}

	delta_time = cur_time - last_time;
	seconds = (float)((double)delta_time / 1000000000.0);

	/* ------------------------------------- */
	/* call tick callbacks                   */

	pthread_mutex_lock(&data->draw_callbacks_mutex);

	for (size_t i = data->tick_callbacks.num; i > 0; i--) {
		struct tick_callback *callback;
		callback = data->tick_callbacks.array + (i - 1);
		callback->tick(callback->param, seconds);
	}

	pthread_mutex_unlock(&data->draw_callbacks_mutex);

	/* ------------------------------------- */
	/* get an array of all sources to tick   */

	da_clear(data->sources_to_tick);

	pthread_mutex_lock(&data->sources_mutex);

	source = data->sources;
	while (source) {
		obs_source_t *s = obs_source_removed(source) ? NULL : obs_source_get_ref(source);
		if (s) {
			da_push_back(data->sources_to_tick, &s);
		}
		source = (struct obs_source *)source->context.hh_uuid.next;
	}

	pthread_mutex_unlock(&data->sources_mutex);

	/* ------------------------------------- */
	/* call the tick function of each source */

	for (size_t i = 0; i < data->sources_to_tick.num; i++) {
		obs_source_t *s = data->sources_to_tick.array[i];
		if (!obs_source_removed(s)) {
			const uint64_t start = source_profiler_source_tick_start();
			obs_source_video_tick(s, seconds);
			source_profiler_source_tick_end(s, start);
		}
		obs_source_release(s);
	}

	return cur_time;
}

/* in obs-display.c */
extern void render_display(struct obs_display *display);

static inline void render_displays(void)
{
	struct obs_display *display;

	if (!obs->data.valid) {
		return;
	}

	gs_enter_context(obs->video.graphics);

	/* render extra displays/swaps */
	pthread_mutex_lock(&obs->data.displays_mutex);

	display = obs->data.first_display;
	while (display) {
		render_display(display);
		display = display->next;
	}

	pthread_mutex_unlock(&obs->data.displays_mutex);

	gs_leave_context();
}

static inline void set_render_size(uint32_t width, uint32_t height)
{
	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);

	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
	gs_set_viewport(0, 0, width, height);
}

static inline void unmap_last_surface(struct obs_core_video_mix *video)
{
	for (int c = 0; c < NUM_CHANNELS; ++c) {
		if (video->mapped_surfaces[c]) {
			gs_stagesurface_unmap(video->mapped_surfaces[c]);
			video->mapped_surfaces[c] = NULL;
		}
	}
}

static inline bool can_reuse_mix_texture(const struct obs_core_video_mix *mix, size_t *idx)
{
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		const struct obs_core_video_mix *other = obs->video.mixes.array[i];
		if (other == mix) {
			break;
		}
		if (other->view != mix->view) {
			continue;
		}
		if (other->render_space != mix->render_space) {
			continue;
		}
		if (other->ovi.base_width != mix->ovi.base_width || other->ovi.base_height != mix->ovi.base_height) {
			continue;
		}
		if (!other->texture_rendered) {
			continue;
		}

		*idx = i;
		return true;
	}

	return false;
}

static inline void draw_mix_texture(const size_t mix_idx)
{
	gs_texture_t *tex = obs->video.mixes.array[mix_idx]->render_texture;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, tex);

	gs_enable_framebuffer_srgb(true);
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(tex, 0, 0, 0);
	}
	gs_enable_framebuffer_srgb(false);
}

static const char *render_main_texture_name = "render_main_texture";
static inline void render_main_texture(struct obs_core_video_mix *video)
{
	uint32_t base_width = video->ovi.base_width;
	uint32_t base_height = video->ovi.base_height;

	profile_start(render_main_texture_name);
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_MAIN_TEXTURE, render_main_texture_name);

	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 0.0f);

	gs_set_render_target_with_color_space(video->render_texture, NULL, video->render_space);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);

	set_render_size(base_width, base_height);

	pthread_mutex_lock(&obs->data.draw_callbacks_mutex);

	for (size_t i = obs->data.draw_callbacks.num; i > 0; i--) {
		struct draw_callback *const callback = obs->data.draw_callbacks.array + (i - 1);
		callback->draw(callback->param, base_width, base_height);
	}

	pthread_mutex_unlock(&obs->data.draw_callbacks_mutex);

	/* In some cases we can reuse a previous mix's texture and save re-rendering everything */
	size_t reuse_idx;
	if (can_reuse_mix_texture(video, &reuse_idx)) {
		draw_mix_texture(reuse_idx);
	} else {
		obs_view_render(video->view);
	}

	video->texture_rendered = true;

	pthread_mutex_lock(&obs->data.draw_callbacks_mutex);

	for (size_t i = 0; i < obs->data.rendered_callbacks.num; ++i) {
		struct rendered_callback *const callback = &obs->data.rendered_callbacks.array[i];
		callback->rendered(callback->param);
	}

	pthread_mutex_unlock(&obs->data.draw_callbacks_mutex);

	GS_DEBUG_MARKER_END();
	profile_end(render_main_texture_name);
}

static inline gs_effect_t *get_scale_effect_internal(struct obs_core_video_mix *mix)
{
	struct obs_core_video *video = &obs->video;
	const struct video_output_info *info = video_output_get_info(mix->video);

	/* if the dimension is under half the size of the original image,
	 * bicubic/lanczos can't sample enough pixels to create an accurate
	 * image, so use the bilinear low resolution effect instead */
	if (info->width < (mix->ovi.base_width / 2) && info->height < (mix->ovi.base_height / 2)) {
		return video->bilinear_lowres_effect;
	}

	switch (mix->ovi.scale_type) {
	case OBS_SCALE_BILINEAR:
		return video->default_effect;
	case OBS_SCALE_LANCZOS:
		return video->lanczos_effect;
	case OBS_SCALE_AREA:
		return video->area_effect;
	case OBS_SCALE_BICUBIC:
	default:;
	}

	return video->bicubic_effect;
}

static inline bool resolution_close(struct obs_core_video_mix *mix, uint32_t width, uint32_t height)
{
	long width_cmp = (long)mix->ovi.base_width - (long)width;
	long height_cmp = (long)mix->ovi.base_height - (long)height;

	return labs(width_cmp) <= 16 && labs(height_cmp) <= 16;
}

static inline gs_effect_t *get_scale_effect(struct obs_core_video_mix *mix, uint32_t width, uint32_t height)
{
	struct obs_core_video *video = &obs->video;

	if (resolution_close(mix, width, height)) {
		return video->default_effect;
	} else {
		/* if the scale method couldn't be loaded, use either bicubic
		 * or bilinear by default */
		gs_effect_t *effect = get_scale_effect_internal(mix);
		if (!effect) {
			effect = !!video->bicubic_effect ? video->bicubic_effect : video->default_effect;
		}
		return effect;
	}
}

static const char *render_output_texture_name = "render_output_texture";
static inline gs_texture_t *render_output_texture(struct obs_core_video_mix *mix)
{
	struct obs_video_info *const ovi = &mix->ovi;
	gs_texture_t *texture = mix->render_texture;
	gs_texture_t *target = mix->output_texture;
	const uint32_t width = gs_texture_get_width(target);
	const uint32_t height = gs_texture_get_height(target);
	if ((width == ovi->base_width) && (height == ovi->base_height)) {
		return texture;
	}

	profile_start(render_output_texture_name);

	gs_effect_t *effect = get_scale_effect(mix, width, height);
	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *bres = gs_effect_get_param_by_name(effect, "base_dimension");
	gs_eparam_t *bres_i = gs_effect_get_param_by_name(effect, "base_dimension_i");
	size_t passes, i;

	gs_set_render_target(target, NULL);
	set_render_size(width, height);

	if (bres) {
		struct vec2 base;
		vec2_set(&base, (float)mix->ovi.base_width, (float)mix->ovi.base_height);
		gs_effect_set_vec2(bres, &base);
	}

	if (bres_i) {
		struct vec2 base_i;
		vec2_set(&base_i, 1.0f / (float)mix->ovi.base_width, 1.0f / (float)mix->ovi.base_height);
		gs_effect_set_vec2(bres_i, &base_i);
	}

	gs_effect_set_texture_srgb(image, texture);

	gs_enable_framebuffer_srgb(true);
	gs_enable_blending(false);
	passes = gs_technique_begin(tech);
	for (i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(texture, 0, width, height);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
	gs_enable_blending(true);
	gs_enable_framebuffer_srgb(false);

	profile_end(render_output_texture_name);

	return target;
}

static void render_convert_plane(gs_effect_t *effect, gs_texture_t *target, const char *tech_name)
{
	gs_technique_t *tech = gs_effect_get_technique(effect, tech_name);

	const uint32_t width = gs_texture_get_width(target);
	const uint32_t height = gs_texture_get_height(target);

	gs_set_render_target(target, NULL);
	set_render_size(width, height);

	size_t passes = gs_technique_begin(tech);
	for (size_t i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw(GS_TRIS, 0, 3);
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

static const char *render_convert_texture_name = "render_convert_texture";
static void render_convert_texture(struct obs_core_video_mix *video, gs_texture_t *const *const convert_textures,
				   gs_texture_t *texture)
{
	profile_start(render_convert_texture_name);

	gs_effect_t *effect = obs->video.conversion_effect;
	gs_eparam_t *color_vec0 = gs_effect_get_param_by_name(effect, "color_vec0");
	gs_eparam_t *color_vec1 = gs_effect_get_param_by_name(effect, "color_vec1");
	gs_eparam_t *color_vec2 = gs_effect_get_param_by_name(effect, "color_vec2");
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *width_i = gs_effect_get_param_by_name(effect, "width_i");
	gs_eparam_t *height_i = gs_effect_get_param_by_name(effect, "height_i");
	gs_eparam_t *sdr_white_nits_over_maximum = gs_effect_get_param_by_name(effect, "sdr_white_nits_over_maximum");
	gs_eparam_t *hdr_lw = gs_effect_get_param_by_name(effect, "hdr_lw");

	struct vec4 vec0, vec1, vec2;
	vec4_set(&vec0, video->color_matrix[4], video->color_matrix[5], video->color_matrix[6], video->color_matrix[7]);
	vec4_set(&vec1, video->color_matrix[0], video->color_matrix[1], video->color_matrix[2], video->color_matrix[3]);
	vec4_set(&vec2, video->color_matrix[8], video->color_matrix[9], video->color_matrix[10],
		 video->color_matrix[11]);

	gs_enable_blending(false);

	if (convert_textures[0]) {
		const float hdr_nominal_peak_level = obs->video.hdr_nominal_peak_level;
		const float multiplier = obs_get_video_sdr_white_level() / 10000.f;
		gs_effect_set_texture(image, texture);
		gs_effect_set_vec4(color_vec0, &vec0);
		gs_effect_set_float(sdr_white_nits_over_maximum, multiplier);
		gs_effect_set_float(hdr_lw, hdr_nominal_peak_level);
		render_convert_plane(effect, convert_textures[0], video->conversion_techs[0]);

		if (convert_textures[1]) {
			gs_effect_set_texture(image, texture);
			gs_effect_set_vec4(color_vec1, &vec1);
			if (!convert_textures[2]) {
				gs_effect_set_vec4(color_vec2, &vec2);
			}
			gs_effect_set_float(width_i, video->conversion_width_i);
			gs_effect_set_float(height_i, video->conversion_height_i);
			gs_effect_set_float(sdr_white_nits_over_maximum, multiplier);
			gs_effect_set_float(hdr_lw, hdr_nominal_peak_level);
			render_convert_plane(effect, convert_textures[1], video->conversion_techs[1]);

			if (convert_textures[2]) {
				gs_effect_set_texture(image, texture);
				gs_effect_set_vec4(color_vec2, &vec2);
				gs_effect_set_float(width_i, video->conversion_width_i);
				gs_effect_set_float(height_i, video->conversion_height_i);
				gs_effect_set_float(sdr_white_nits_over_maximum, multiplier);
				gs_effect_set_float(hdr_lw, hdr_nominal_peak_level);
				render_convert_plane(effect, convert_textures[2], video->conversion_techs[2]);
			}
		}
	}

	gs_enable_blending(true);

	video->texture_converted = true;

	profile_end(render_convert_texture_name);
}

static const char *stage_output_texture_name = "stage_output_texture";
static inline void stage_output_texture(struct obs_core_video_mix *video, int cur_texture,
					gs_texture_t *const *const convert_textures, gs_texture_t *output_texture,
					gs_stagesurf_t *const *const copy_surfaces, size_t channel_count)
{
	profile_start(stage_output_texture_name);

	unmap_last_surface(video);

	if (!video->gpu_conversion) {
		gs_stagesurf_t *copy = copy_surfaces[0];
		if (copy) {
			gs_stage_texture(copy, output_texture);
		}
		video->active_copy_surfaces[cur_texture][0] = copy;

		for (size_t i = 1; i < NUM_CHANNELS; ++i) {
			video->active_copy_surfaces[cur_texture][i] = NULL;
		}

		video->textures_copied[cur_texture] = true;
	} else if (video->texture_converted) {
		for (size_t i = 0; i < channel_count; i++) {
			gs_stagesurf_t *copy = copy_surfaces[i];
			if (copy) {
				gs_stage_texture(copy, convert_textures[i]);
			}
			video->active_copy_surfaces[cur_texture][i] = copy;
		}

		for (size_t i = channel_count; i < NUM_CHANNELS; ++i) {
			video->active_copy_surfaces[cur_texture][i] = NULL;
		}

		video->textures_copied[cur_texture] = true;
	}

	profile_end(stage_output_texture_name);
}

static inline bool queue_frame(struct obs_core_video_mix *video, bool raw_active, struct obs_vframe_info *vframe_info)
{
	bool duplicate = !video->gpu_encoder_avail_queue.size ||
			 (video->gpu_encoder_queue.size && vframe_info->count > 1);

	if (duplicate) {
		struct obs_tex_frame *tf =
			deque_data(&video->gpu_encoder_queue, video->gpu_encoder_queue.size - sizeof(*tf));

		/* texture-based encoding is stopping */
		if (!tf) {
			return false;
		}

		tf->count++;
		os_sem_post(video->gpu_encode_semaphore);
		goto finish;
	}

	struct obs_tex_frame tf;
	deque_pop_front(&video->gpu_encoder_avail_queue, &tf, sizeof(tf));

	if (tf.released) {
#ifdef _WIN32
		const uint64_t acquire_start = os_gettime_ns();
		gs_texture_acquire_sync(tf.tex, tf.lock_key, GS_WAIT_INFINITE);
		video->debug_encwait_ns += os_gettime_ns() - acquire_start;
#endif
		tf.released = false;
	}

	/* the vframe_info->count > 1 case causing a copy can only happen if by
	 * some chance the very first frame has to be duplicated for whatever
	 * reason.  otherwise, it goes to the 'duplicate' case above, which
	 * will ensure better performance. */
	if (raw_active || vframe_info->count > 1) {
		gs_copy_texture(tf.tex, video->convert_textures_encode[0]);
#ifndef _WIN32
		/* Y and UV textures are views of the same texture on D3D, and
		 * gs_copy_texture will copy all views of the underlying
		 * texture. On other platforms, these are two distinct textures
		 * that must be copied separately. */
		gs_copy_texture(tf.tex_uv, video->convert_textures_encode[1]);
#endif
	} else {
		gs_texture_t *tex = video->convert_textures_encode[0];
		gs_texture_t *tex_uv = video->convert_textures_encode[1];

		video->convert_textures_encode[0] = tf.tex;
		video->convert_textures_encode[1] = tf.tex_uv;

		tf.tex = tex;
		tf.tex_uv = tex_uv;
	}

	tf.count = 1;
	tf.timestamp = vframe_info->timestamp;
	tf.released = true;
#ifdef _WIN32
	tf.handle = gs_texture_get_shared_handle(tf.tex);
	gs_texture_release_sync(tf.tex, ++tf.lock_key);
#endif
	deque_push_back(&video->gpu_encoder_queue, &tf, sizeof(tf));

	os_sem_post(video->gpu_encode_semaphore);

finish:
	return --vframe_info->count;
}

extern void full_stop(struct obs_encoder *encoder);

static inline void encode_gpu(struct obs_core_video_mix *video, bool raw_active, struct obs_vframe_info *vframe_info)
{
	while (queue_frame(video, raw_active, vframe_info))
		;
}

static const char *output_gpu_encoders_name = "output_gpu_encoders";
static void output_gpu_encoders(struct obs_core_video_mix *video, bool raw_active)
{
	profile_start(output_gpu_encoders_name);

	if (!video->texture_converted) {
		goto end;
	}
	if (!video->vframe_info_buffer_gpu.size) {
		goto end;
	}

	struct obs_vframe_info vframe_info;
	deque_pop_front(&video->vframe_info_buffer_gpu, &vframe_info, sizeof(vframe_info));

	pthread_mutex_lock(&video->gpu_encoder_mutex);
	encode_gpu(video, raw_active, &vframe_info);
	pthread_mutex_unlock(&video->gpu_encoder_mutex);

end:
	profile_end(output_gpu_encoders_name);
}

static inline void render_video(struct obs_core_video_mix *video, bool raw_active, const bool gpu_active,
				int cur_texture)
{
	gs_begin_scene();

	gs_enable_depth_test(false);
	gs_set_cull_mode(GS_NEITHER);

	render_main_texture(video);

	if (raw_active || gpu_active) {
		gs_texture_t *const *convert_textures = video->convert_textures;
		gs_stagesurf_t *const *copy_surfaces = video->copy_surfaces[cur_texture];
		size_t channel_count = NUM_CHANNELS;
		gs_texture_t *output_texture = render_output_texture(video);

		if (gpu_active) {
			convert_textures = video->convert_textures_encode;
#ifdef _WIN32
			copy_surfaces = video->copy_surfaces_encode;
			channel_count = 1;
#endif
		}

		if (video->gpu_conversion) {
			render_convert_texture(video, convert_textures, output_texture);
		}

		if (gpu_active) {
			output_gpu_encoders(video, raw_active);
		}

		if (raw_active) {
			stage_output_texture(video, cur_texture, convert_textures, output_texture, copy_surfaces,
					     channel_count);
		}
	}

	gs_set_render_target(NULL, NULL);
	gs_enable_blending(true);

	gs_end_scene();
}

static inline bool download_frame(struct obs_core_video_mix *video, int prev_texture, struct video_data *frame)
{
	if (!video->textures_copied[prev_texture]) {
		return false;
	}

	for (int channel = 0; channel < NUM_CHANNELS; ++channel) {
		gs_stagesurf_t *surface = video->active_copy_surfaces[prev_texture][channel];
		if (surface) {
			if (!gs_stagesurface_map(surface, &frame->data[channel], &frame->linesize[channel])) {
				return false;
			}

			video->mapped_surfaces[channel] = surface;
		}
	}
	return true;
}

static const uint8_t *set_gpu_converted_plane(uint32_t width, uint32_t height, uint32_t linesize_input,
					      uint32_t linesize_output, const uint8_t *in, uint8_t *out)
{
	if ((width == linesize_input) && (width == linesize_output)) {
		size_t total = (size_t)width * (size_t)height;
		memcpy(out, in, total);
		in += total;
	} else {
		for (size_t y = 0; y < height; y++) {
			memcpy(out, in, width);
			out += linesize_output;
			in += linesize_input;
		}
	}

	return in;
}

static void set_gpu_converted_data(struct video_frame *output, const struct video_data *input,
				   const struct video_output_info *info)
{
	switch (info->format) {
	case VIDEO_FORMAT_I420: {
		const uint32_t width = info->width;
		const uint32_t height = info->height;

		set_gpu_converted_plane(width, height, input->linesize[0], output->linesize[0], input->data[0],
					output->data[0]);

		const uint32_t width_d2 = width / 2;
		const uint32_t height_d2 = height / 2;

		set_gpu_converted_plane(width_d2, height_d2, input->linesize[1], output->linesize[1], input->data[1],
					output->data[1]);

		set_gpu_converted_plane(width_d2, height_d2, input->linesize[2], output->linesize[2], input->data[2],
					output->data[2]);

		break;
	}
	case VIDEO_FORMAT_NV12: {
		const uint32_t width = info->width;
		const uint32_t height = info->height;
		const uint32_t height_d2 = height / 2;
		if (input->linesize[1]) {
			set_gpu_converted_plane(width, height, input->linesize[0], output->linesize[0], input->data[0],
						output->data[0]);
			set_gpu_converted_plane(width, height_d2, input->linesize[1], output->linesize[1],
						input->data[1], output->data[1]);
		} else {
			const uint8_t *const in_uv = set_gpu_converted_plane(width, height, input->linesize[0],
									     output->linesize[0], input->data[0],
									     output->data[0]);
			set_gpu_converted_plane(width, height_d2, input->linesize[0], output->linesize[1], in_uv,
						output->data[1]);
		}

		break;
	}
	case VIDEO_FORMAT_I444: {
		const uint32_t width = info->width;
		const uint32_t height = info->height;

		set_gpu_converted_plane(width, height, input->linesize[0], output->linesize[0], input->data[0],
					output->data[0]);

		set_gpu_converted_plane(width, height, input->linesize[1], output->linesize[1], input->data[1],
					output->data[1]);

		set_gpu_converted_plane(width, height, input->linesize[2], output->linesize[2], input->data[2],
					output->data[2]);

		break;
	}
	case VIDEO_FORMAT_I010: {
		const uint32_t width = info->width;
		const uint32_t height = info->height;

		set_gpu_converted_plane(width * 2, height, input->linesize[0], output->linesize[0], input->data[0],
					output->data[0]);

		const uint32_t height_d2 = height / 2;

		set_gpu_converted_plane(width, height_d2, input->linesize[1], output->linesize[1], input->data[1],
					output->data[1]);

		set_gpu_converted_plane(width, height_d2, input->linesize[2], output->linesize[2], input->data[2],
					output->data[2]);

		break;
	}
	case VIDEO_FORMAT_P010: {
		const uint32_t width_x2 = info->width * 2;
		const uint32_t height = info->height;
		const uint32_t height_d2 = height / 2;
		if (input->linesize[1]) {
			set_gpu_converted_plane(width_x2, height, input->linesize[0], output->linesize[0],
						input->data[0], output->data[0]);
			set_gpu_converted_plane(width_x2, height_d2, input->linesize[1], output->linesize[1],
						input->data[1], output->data[1]);
		} else {
			const uint8_t *const in_uv = set_gpu_converted_plane(width_x2, height, input->linesize[0],
									     output->linesize[0], input->data[0],
									     output->data[0]);
			set_gpu_converted_plane(width_x2, height_d2, input->linesize[0], output->linesize[1], in_uv,
						output->data[1]);
		}

		break;
	}
	case VIDEO_FORMAT_P216: {
		const uint32_t width_x2 = info->width * 2;
		const uint32_t height = info->height;

		set_gpu_converted_plane(width_x2, height, input->linesize[0], output->linesize[0], input->data[0],
					output->data[0]);

		set_gpu_converted_plane(width_x2, height, input->linesize[1], output->linesize[1], input->data[1],
					output->data[1]);

		break;
	}
	case VIDEO_FORMAT_P416: {
		const uint32_t height = info->height;

		set_gpu_converted_plane(info->width * 2, height, input->linesize[0], output->linesize[0],
					input->data[0], output->data[0]);

		set_gpu_converted_plane(info->width * 4, height, input->linesize[1], output->linesize[1],
					input->data[1], output->data[1]);

		break;
	}

	case VIDEO_FORMAT_NONE:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
	case VIDEO_FORMAT_YA2L:
	case VIDEO_FORMAT_AYUV:
	case VIDEO_FORMAT_V210:
	case VIDEO_FORMAT_R10L:
		/* unimplemented */
		;
	}
}

static inline void copy_rgbx_frame(struct video_frame *output, const struct video_data *input,
				   const struct video_output_info *info)
{
	uint8_t *in_ptr = input->data[0];
	uint8_t *out_ptr = output->data[0];

	/* if the line sizes match, do a single copy */
	if (input->linesize[0] == output->linesize[0]) {
		memcpy(out_ptr, in_ptr, (size_t)input->linesize[0] * (size_t)info->height);
	} else {
		const size_t copy_size = (size_t)info->width * 4;
		for (size_t y = 0; y < info->height; y++) {
			memcpy(out_ptr, in_ptr, copy_size);
			in_ptr += input->linesize[0];
			out_ptr += output->linesize[0];
		}
	}
}

static inline void output_video_data(struct obs_core_video_mix *video, struct video_data *input_frame, int count)
{
	const struct video_output_info *info;
	struct video_frame output_frame;
	bool locked;

	info = video_output_get_info(video->video);

	locked = video_output_lock_frame(video->video, &output_frame, count, input_frame->timestamp);
	if (locked) {
		if (video->gpu_conversion) {
			set_gpu_converted_data(&output_frame, input_frame, info);
		} else {
			copy_rgbx_frame(&output_frame, input_frame, info);
		}

		video_output_unlock_frame(video->video);
	}
}

void add_ready_encoder_group(obs_encoder_t *encoder)
{
	obs_weak_encoder_t *weak = obs_encoder_get_weak_encoder(encoder);
	pthread_mutex_lock(&obs->video.encoder_group_mutex);
	da_push_back(obs->video.ready_encoder_groups, &weak);
	pthread_mutex_unlock(&obs->video.encoder_group_mutex);
}

/* How long the pipeline gets to prove itself before lag counts regardless. */
#define STARTUP_GRACE_NS 10000000000ULL

/* Ends the startup grace once the pipeline holds its deadline for a full second
 * of frames, or once the grace lapses. Reports what it withheld either way --
 * a suppressed number that is never disclosed is indistinguishable from a bug. */
static void video_startup_settle(struct obs_core_video *video, uint32_t lagged, uint64_t interval_ns)
{
	video->startup_ontime_streak = lagged ? 0 : video->startup_ontime_streak + 1;

	const bool steady = interval_ns && video->startup_ontime_streak >= (1000000000ULL / interval_ns);
	const bool lapsed = os_gettime_ns() >= video->startup_deadline_ns;
	if (!steady && !lapsed) {
		return;
	}

	video->startup_settled = true;
	blog(LOG_INFO, "Video pipeline settled (%s); %" PRIu32 " frame(s) lost to startup are not counted as lag",
	     steady ? "held the frame deadline for one second" : "startup grace elapsed",
	     video->startup_skipped_frames);
}

static inline void video_sleep(struct obs_core_video *video, uint64_t *p_time, uint64_t interval_ns)
{
	struct obs_vframe_info vframe_info;
	uint64_t cur_time = *p_time;
	uint64_t t = cur_time + interval_ns;
	int count;

	if (os_sleepto_ns(t)) {
		*p_time = t;
		count = 1;
	} else {
		const uint64_t udiff = os_gettime_ns() - cur_time;
		int64_t diff;
		memcpy(&diff, &udiff, sizeof(diff));
		const uint64_t clamped_diff = (diff > (int64_t)interval_ns) ? (uint64_t)diff : interval_ns;
		count = (int)(clamped_diff / interval_ns);
		*p_time = cur_time + interval_ns * count;
	}

	video->total_frames += count;

	const uint32_t lagged = (uint32_t)(count - 1);
	video->lagged_frames_raw += lagged;
	if (video->startup_settled) {
		video->lagged_frames += lagged;
	} else {
		if (!video->startup_deadline_ns) {
			video->startup_deadline_ns = os_gettime_ns() + STARTUP_GRACE_NS;
		}
		video->startup_skipped_frames += lagged;
		video_startup_settle(video, lagged, interval_ns);
	}

	vframe_info.timestamp = cur_time;
	vframe_info.count = count;

	pthread_mutex_lock(&video->encoder_group_mutex);
	for (size_t i = 0; i < video->ready_encoder_groups.num; i++) {
		obs_encoder_t *encoder = obs_weak_encoder_get_encoder(video->ready_encoder_groups.array[i]);
		obs_weak_encoder_release(video->ready_encoder_groups.array[i]);
		if (!encoder) {
			continue;
		}

		if (encoder->encoder_group) {
			struct obs_encoder_group *group = encoder->encoder_group;
			pthread_mutex_lock(&group->mutex);
			if (group->num_encoders_started >= group->encoders.num && !group->start_timestamp) {
				group->start_timestamp = *p_time;
			}
			pthread_mutex_unlock(&group->mutex);
		}
		obs_encoder_release(encoder);
	}
	da_clear(video->ready_encoder_groups);
	pthread_mutex_unlock(&video->encoder_group_mutex);

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *video = obs->video.mixes.array[i];
		bool raw_active = video->raw_was_active;
		bool gpu_active = video->gpu_was_active;

		if (raw_active) {
			deque_push_back(&video->vframe_info_buffer, &vframe_info, sizeof(vframe_info));
		}
		if (gpu_active) {
			deque_push_back(&video->vframe_info_buffer_gpu, &vframe_info, sizeof(vframe_info));
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

/* Composite-timing diagnostics (behind obs->video.render_debug). GPU results
 * resolve one frame late; see debug_resolve_composite_gpu / output_frames. */
static void debug_composite_range_begin(uint8_t slot)
{
	gs_enter_context(obs->video.graphics);
	if (!obs->video.debug_composite_ranges[slot]) {
		obs->video.debug_composite_ranges[slot] = gs_timer_range_create();
	}
	gs_timer_range_begin(obs->video.debug_composite_ranges[slot]);
	gs_leave_context();
}

static void debug_composite_range_end(uint8_t slot)
{
	if (!obs->video.debug_composite_ranges[slot]) {
		return;
	}
	gs_enter_context(obs->video.graphics);
	gs_timer_range_end(obs->video.debug_composite_ranges[slot]);
	gs_leave_context();
}

/* Resolve the GPU composite timers written one frame earlier (the read slot),
 * accumulate their durations, then free them so the slot can be reused. */
static void debug_resolve_composite_gpu(uint8_t write_slot)
{
	const uint8_t read_slot = (write_slot + 1) % NUM_TEXTURES;
	bool disjoint = false;
	bool ready = false;
	uint64_t freq = 0;

	gs_enter_context(obs->video.graphics);

	if (obs->video.debug_composite_ranges[read_slot]) {
		ready = true;
		gs_timer_range_get_data(obs->video.debug_composite_ranges[read_slot], &disjoint, &freq);
	}

	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (!mix) {
			continue;
		}

		gs_timer_t *timer = mix->debug_composite_timers[read_slot];
		if (!timer) {
			continue;
		}

		uint64_t ticks = 0;
		if (ready && !disjoint && freq && gs_timer_get_data(timer, &ticks)) {
			const uint64_t gpu_ns = util_mul_div64(ticks, 1000000000ULL, freq);
			mix->debug_composite_gpu_accum += gpu_ns;
			mix->debug_composite_gpu_count++;
			if (gpu_ns > mix->debug_composite_gpu_max) {
				mix->debug_composite_gpu_max = gpu_ns;
			}
		}

		gs_timer_destroy(timer);
		mix->debug_composite_timers[read_slot] = NULL;
	}

	gs_leave_context();
}

static const char *output_frame_gs_context_name = "gs_context(video->graphics)";
static const char *output_frame_render_video_name = "render_video";
static const char *output_frame_download_frame_name = "download_frame";
static const char *output_frame_gs_flush_name = "gs_flush";
static const char *output_frame_output_video_data_name = "output_video_data";
/* The sub-segments of output_frame, summed across every mix in the frame. Each
 * one is a place the graphics thread can block on the GPU rather than on its own
 * work, and none of them are covered by the per-mix composite figure. */
struct output_frame_timing {
	uint64_t ctx_ns;     /* gs_enter_context -- the global graphics mutex */
	uint64_t flush_ns;   /* gs_flush -- drains the command queue */
	uint64_t dload_ns;   /* download_frame + output_video_data -- staging map */
	uint64_t encwait_ns; /* gs_texture_acquire_sync -- GPU encoder texture release */
};

static inline void output_frame(struct obs_core_video_mix *video, bool debug, bool gpu_debug, uint8_t debug_slot,
				struct output_frame_timing *t)
{
	const bool raw_active = video->raw_was_active;
	const bool gpu_active = video->gpu_was_active;

	int cur_texture = video->cur_texture;
	int prev_texture = cur_texture == 0 ? NUM_TEXTURES - 1 : cur_texture - 1;
	struct video_data frame;
	bool frame_ready = 0;

	memset(&frame, 0, sizeof(struct video_data));

	profile_start(output_frame_gs_context_name);
	/* The loop releases the graphics context again right after gs_begin_frame,
	 * so this is a cold re-acquisition of the global graphics mutex, contended
	 * against every other thread that calls obs_enter_graphics -- and it sits
	 * ahead of debug_cpu_start below, so the per-mix "composite CPU" figure
	 * cannot see it. Timed separately for exactly that reason. */
	const uint64_t ctx_start = os_gettime_ns();
	gs_enter_context(obs->video.graphics);
	t->ctx_ns += os_gettime_ns() - ctx_start;

	uint64_t debug_cpu_start = 0;
	gs_timer_t *debug_timer = NULL;
	if (debug) {
		debug_cpu_start = os_gettime_ns();
	}
	if (gpu_debug) {
		debug_timer = gs_timer_create();
		gs_timer_begin(debug_timer);
	}

	profile_start(output_frame_render_video_name);
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_RENDER_VIDEO, output_frame_render_video_name);
	video->debug_encwait_ns = 0;
	render_video(video, raw_active, gpu_active, cur_texture);
	GS_DEBUG_MARKER_END();
	profile_end(output_frame_render_video_name);

	if (gpu_debug) {
		gs_timer_end(debug_timer);
	}
	t->encwait_ns += video->debug_encwait_ns;
	if (debug) {
		const uint64_t raw_cpu_ns = os_gettime_ns() - debug_cpu_start;
		/* Report drawing, not waiting. The encoder-texture wait happens inside
		 * render_video, so without this the composite figure counts a stall on
		 * the encoder as time spent compositing -- which is how a 17 ms
		 * "composite" spike was once recorded against an idle scene. */
		const uint64_t debug_cpu_ns =
			raw_cpu_ns > video->debug_encwait_ns ? raw_cpu_ns - video->debug_encwait_ns : 0;
		video->debug_composite_cpu_accum += debug_cpu_ns;
		video->debug_composite_cpu_count++;
		if (debug_cpu_ns > video->debug_composite_cpu_max) {
			video->debug_composite_cpu_max = debug_cpu_ns;
		}
	}
	if (gpu_debug) {
		if (video->debug_composite_timers[debug_slot]) {
			gs_timer_destroy(video->debug_composite_timers[debug_slot]);
		}
		video->debug_composite_timers[debug_slot] = debug_timer;
	}

	if (raw_active) {
		profile_start(output_frame_download_frame_name);
		const uint64_t dload_start = os_gettime_ns();
		frame_ready = download_frame(video, prev_texture, &frame);
		t->dload_ns += os_gettime_ns() - dload_start;
		profile_end(output_frame_download_frame_name);
	}

	profile_start(output_frame_gs_flush_name);
	const uint64_t flush_start = os_gettime_ns();
	gs_flush();
	t->flush_ns += os_gettime_ns() - flush_start;
	profile_end(output_frame_gs_flush_name);

	gs_leave_context();
	profile_end(output_frame_gs_context_name);

	if (raw_active && frame_ready) {
		struct obs_vframe_info vframe_info;
		deque_pop_front(&video->vframe_info_buffer, &vframe_info, sizeof(vframe_info));

		frame.timestamp = vframe_info.timestamp;
		profile_start(output_frame_output_video_data_name);
		const uint64_t out_start = os_gettime_ns();
		output_video_data(video, &frame, vframe_info.count);
		t->dload_ns += os_gettime_ns() - out_start;
		profile_end(output_frame_output_video_data_name);
	}

	if (++video->cur_texture == NUM_TEXTURES) {
		video->cur_texture = 0;
	}
}

/* Reports the mixes_mutex acquisition separately from the compositing it
 * guards. The audio thread takes the same lock and holds it across a walk of
 * every mix's active source tree (obs-audio.c), so a graphics frame can lose
 * its whole budget here without a single microsecond of rendering being slow --
 * a stall the two look identical from outside. */
static inline void output_frames(uint64_t *lock_ns, struct output_frame_timing *t)
{
	const uint64_t lock_start = os_gettime_ns();
	pthread_mutex_lock(&obs->video.mixes_mutex);
	*lock_ns = os_gettime_ns() - lock_start;
	memset(t, 0, sizeof(*t));

	const bool debug = os_atomic_load_bool(&obs->video.render_debug);
	const bool gpu_debug = debug && os_atomic_load_bool(&obs->video.render_gpu_debug);
	uint8_t debug_slot = 0;
	if (gpu_debug) {
		obs->video.debug_composite_range_idx = (obs->video.debug_composite_range_idx + 1) % NUM_TEXTURES;
		debug_slot = obs->video.debug_composite_range_idx;
		debug_resolve_composite_gpu(debug_slot);
		debug_composite_range_begin(debug_slot);
	}

	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (mix->view) {
			output_frame(mix, debug, gpu_debug, debug_slot, t);
		} else {
			obs->video.mixes.array[i] = NULL;
			obs_free_video_mix(mix);
			da_erase(obs->video.mixes, i);
			i--;
			num--;
		}
	}

	if (gpu_debug) {
		debug_composite_range_end(debug_slot);
	}

	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

static void clear_base_frame_data(struct obs_core_video_mix *video)
{
	video->texture_rendered = false;
	video->texture_converted = false;
	deque_free(&video->vframe_info_buffer);
	video->cur_texture = 0;
}

static void clear_raw_frame_data(struct obs_core_video_mix *video)
{
	memset(video->textures_copied, 0, sizeof(video->textures_copied));
	deque_free(&video->vframe_info_buffer);
}

static void clear_gpu_frame_data(struct obs_core_video_mix *video)
{
	deque_free(&video->vframe_info_buffer_gpu);
}

extern THREAD_LOCAL bool is_graphics_thread;

/* A single graphics task running this long is a candidate explanation for a
 * missed frame, since it runs inline on the graphics thread. */
#define RENDER_DEBUG_TASK_WARN_NS 2000000ULL

/* Share of one frame's budget a single segment must eat to be reported as the
 * precursor to a miss. Derived from the configured interval rather than fixed,
 * so it does not fire every frame at a lower frame rate. */
#define RENDER_DEBUG_SEGMENT_WARN_PCT 50

/* Ceiling on the early-warning line, which unlike the on-miss line can repeat
 * every frame. blog() on this thread is a synchronous flushed file write, so an
 * ungoverned warning delays the next frame enough to cause the miss it then
 * reports. Suppressed lines are counted and disclosed, never silently dropped. */
#define RENDER_DEBUG_WARN_WINDOW_NS 1000000000ULL
#define RENDER_DEBUG_WARN_MAX_PER_WINDOW 2

/* Indexed by enum obs_graphics_acquirer; order must match. */
static const char *const graphics_acquirer_names[] = {"nobody", "obs_enter_graphics", "obs_display_create",
						      "source teardown", "async texture resize"};

static inline uint32_t debug_now_ms(void)
{
	return (uint32_t)(os_gettime_ns() / 1000000ULL);
}

/* Best-effort record of who holds, or last held, the graphics context away from
 * the graphics thread, so a stalled enter segment can name the obstruction.
 * Written by arbitrary threads and read by the graphics thread with no
 * synchronization across the fields, so a read can pair an acquirer with a
 * mismatched time; that skew is acceptable for a diagnostic.
 *
 * The acquirer is stored as an index into graphics_acquirer_names rather than as
 * a pointer, because there are no pointer-width atomics here and every value the
 * index resolves to is a string literal that outlives the process. A stale or
 * racing read therefore cannot hand the graphics thread a dangling pointer,
 * which would be a far worse failure than the lag being investigated. */
void obs_note_graphics_acquire(enum obs_graphics_acquirer who)
{
	if (!obs || is_graphics_thread) {
		return;
	}

	os_atomic_set_long(&obs->video.debug_acquirer, (long)who);
	os_atomic_set_long(&obs->video.debug_acquire_time_ms, (long)debug_now_ms());
	os_atomic_set_bool(&obs->video.debug_acquirer_held, true);
}

void obs_note_graphics_release(void)
{
	/* gs_enter_context is recursive, and only the outermost leave actually
	 * drops the lock -- which is exactly when gs_get_context goes NULL. */
	if (!obs || is_graphics_thread || gs_get_context()) {
		return;
	}

	os_atomic_set_long(&obs->video.debug_acquire_time_ms, (long)debug_now_ms());
	os_atomic_set_bool(&obs->video.debug_acquirer_held, false);
}

const char *obs_last_graphics_acquirer(uint32_t *age_ms, bool *held)
{
	const long idx = os_atomic_load_long(&obs->video.debug_acquirer);
	const uint32_t then_ms = (uint32_t)os_atomic_load_long(&obs->video.debug_acquire_time_ms);

	*held = os_atomic_load_bool(&obs->video.debug_acquirer_held);
	*age_ms = debug_now_ms() - then_ms;

	if (idx < 0 || (size_t)idx >= sizeof(graphics_acquirer_names) / sizeof(graphics_acquirer_names[0])) {
		return graphics_acquirer_names[OBS_GRAPHICS_ACQUIRER_NONE];
	}

	return graphics_acquirer_names[idx];
}

/* Returns the time spent logging slow tasks, so the caller can keep this
 * diagnostic's own cost out of the tasks segment it reports. */
static uint64_t execute_graphics_tasks(void)
{
	struct obs_core_video *video = &obs->video;
	bool tasks_remaining = true;
	uint64_t log_ns = 0;

	while (tasks_remaining) {
		obs_task_t slow_task = NULL;
		uint64_t slow_ns = 0;

		pthread_mutex_lock(&video->task_mutex);
		if (video->tasks.size) {
			struct obs_task_info info;
			deque_pop_front(&video->tasks, &info, sizeof(info));

			const uint64_t task_start = os_gettime_ns();
			info.task(info.param);
			const uint64_t task_ns = os_gettime_ns() - task_start;

			if (task_ns >= RENDER_DEBUG_TASK_WARN_NS) {
				slow_task = info.task;
				slow_ns = task_ns;
			}
		}
		tasks_remaining = !!video->tasks.size;
		pthread_mutex_unlock(&video->task_mutex);

		if (slow_task && os_atomic_load_bool(&video->render_debug)) {
			const uint64_t log_start = os_gettime_ns();
			blog(LOG_INFO, "[render-debug] graphics task %p took %.1f" NBSP "ms",
			     (void *)(uintptr_t)slow_task, obs_debug_ns_to_ms(slow_ns));
			log_ns += os_gettime_ns() - log_start;
		}
	}

	return log_ns;
}

#ifdef _WIN32

struct winrt_exports {
	void (*winrt_initialize)();
	void (*winrt_uninitialize)();
	struct winrt_disaptcher *(*winrt_dispatcher_init)();
	void (*winrt_dispatcher_free)(struct winrt_disaptcher *dispatcher);
	void (*winrt_capture_thread_start)();
	void (*winrt_capture_thread_stop)();
};

#define WINRT_IMPORT(func)                                        \
	do {                                                      \
		exports->func = os_dlsym(module, #func);          \
		if (!exports->func) {                             \
			success = false;                          \
			blog(LOG_ERROR,                           \
			     "Could not load function '%s' from " \
			     "module '%s'",                       \
			     #func, module_name);                 \
		}                                                 \
	} while (false)

static bool load_winrt_imports(struct winrt_exports *exports, void *module, const char *module_name)
{
	bool success = true;

	WINRT_IMPORT(winrt_initialize);
	WINRT_IMPORT(winrt_uninitialize);
	WINRT_IMPORT(winrt_dispatcher_init);
	WINRT_IMPORT(winrt_dispatcher_free);
	WINRT_IMPORT(winrt_capture_thread_start);
	WINRT_IMPORT(winrt_capture_thread_stop);

	return success;
}

struct winrt_state {
	bool loaded;
	void *winrt_module;
	struct winrt_exports exports;
	struct winrt_disaptcher *dispatcher;
};

static void init_winrt_state(struct winrt_state *winrt)
{
	static const char *const module_name = "libobs-winrt";

	winrt->winrt_module = os_dlopen(module_name);
	winrt->loaded = winrt->winrt_module && load_winrt_imports(&winrt->exports, winrt->winrt_module, module_name);
	winrt->dispatcher = NULL;
	if (winrt->loaded) {
		winrt->exports.winrt_initialize();
		winrt->dispatcher = winrt->exports.winrt_dispatcher_init();

		gs_enter_context(obs->video.graphics);
		winrt->exports.winrt_capture_thread_start();
		gs_leave_context();
	}
}

static void uninit_winrt_state(struct winrt_state *winrt)
{
	if (winrt->winrt_module) {
		if (winrt->loaded) {
			winrt->exports.winrt_capture_thread_stop();
			if (winrt->dispatcher) {
				winrt->exports.winrt_dispatcher_free(winrt->dispatcher);
			}
			winrt->exports.winrt_uninitialize();
		}

		os_dlclose(winrt->winrt_module);
	}
}

#endif // #ifdef _WIN32

static const char *tick_sources_name = "tick_sources";
static const char *render_displays_name = "render_displays";
static const char *output_frame_name = "output_frame";
static const char *enter_context_name = "enter_context";
static const char *graphics_tasks_name = "graphics_tasks";
#ifdef _WIN32
static const char *message_loop_name = "message_loop";
#endif
static inline void update_active_state(struct obs_core_video_mix *video)
{
	const bool raw_was_active = video->raw_was_active;
	const bool gpu_was_active = video->gpu_was_active;
	const bool was_active = video->was_active;

	bool raw_active = os_atomic_load_long(&video->raw_active) > 0;
	const bool gpu_active = os_atomic_load_long(&video->gpu_encoder_active) > 0;
	const bool active = raw_active || gpu_active;

	if (!was_active && active) {
		clear_base_frame_data(video);
	}
	if (!raw_was_active && raw_active) {
		clear_raw_frame_data(video);
	}
	if (!gpu_was_active && gpu_active) {
		clear_gpu_frame_data(video);
	}

	video->gpu_was_active = gpu_active;
	video->raw_was_active = raw_active;
	video->was_active = active;
}

static inline void update_active_states(void)
{
	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		update_active_state(obs->video.mixes.array[i]);
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

static inline bool stop_requested(void)
{
	bool success = true;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		if (!video_output_stopped(obs->video.mixes.array[i]->video)) {
			success = false;
		}
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	return success;
}

static bool debug_emit_source_stats_cb(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(param);

	struct profiler_result res;
	/* tick_max earns its place in the gate: a source can be cheap to render and
	 * still burn a whole frame in its tick callback, and gating on the render
	 * fields alone hides exactly that source. tick is reported as the window max
	 * rather than a sum because it is a spike we are after -- a one-off 130 ms
	 * first tick averages away to nothing across a one-second rollup. */
	if (source_profiler_fill_result(source, &res) && (res.render_sum || res.render_gpu_sum || res.tick_max)) {
		char gpu[48] = "";
		if (os_atomic_load_bool(&obs->video.render_gpu_debug)) {
			snprintf(gpu, sizeof(gpu), ", GPU %.1f" NBSP "us", (double)res.render_gpu_sum / 1000.0);
		}

		blog(LOG_INFO,
		     "[render-debug]   source '%s': tick CPU %.1f" NBSP "us peak, render CPU %.1f" NBSP "us%s",
		     obs_source_get_name(source), (double)res.tick_max / 1000.0, (double)res.render_sum / 1000.0, gpu);
	}
	return true;
}

/* Once-per-second rollup of composite timing collected while render_debug is on.
 * Emits one line per mix (canvas), then per-source render aggregates. The lagged
 * delta is process-wide rather than per-mix, so it repeats on every mix line to
 * keep each line self-contained. */
/* One optional GPU column. Left empty when GPU timing is off, so an unmeasured
 * value can never be read as a measured zero -- the control run for this
 * diagnostic is precisely the one with GPU timing off. */
static void debug_format_gpu_field(char *buf, size_t size, bool measured, const char *label, uint64_t ns,
				   double budget_ns)
{
	if (!measured) {
		*buf = '\0';
		return;
	}

	const double pct = budget_ns > 0.0 ? (double)ns / budget_ns * 100.0 : 0.0;
	snprintf(buf, size, ", %s %.1f" NBSP "us (%.1f%% frame)", label, (double)ns / 1000.0, pct);
}

static void debug_emit_composite_stats(void)
{
	const double fps = obs->video.video_fps;
	const double budget_ns = fps > 0.0 ? 1.0e9 / fps : 0.0;
	const bool gpu_measured = os_atomic_load_bool(&obs->video.render_gpu_debug);
	const char *gpu_note = gpu_measured ? "" : ", GPU timing off";

	const uint32_t lagged_total = obs->video.lagged_frames_raw;
	const uint32_t lagged_delta = lagged_total - obs->video.debug_last_lagged_frames;
	obs->video.debug_last_lagged_frames = lagged_total;

	pthread_mutex_lock(&obs->video.mixes_mutex);
	for (size_t i = 0, num = obs->video.mixes.num; i < num; i++) {
		struct obs_core_video_mix *mix = obs->video.mixes.array[i];
		if (!mix) {
			continue;
		}

		const uint64_t cpu_avg = mix->debug_composite_cpu_count
						 ? mix->debug_composite_cpu_accum / mix->debug_composite_cpu_count
						 : 0;
		const uint64_t gpu_avg = mix->debug_composite_gpu_count
						 ? mix->debug_composite_gpu_accum / mix->debug_composite_gpu_count
						 : 0;
		const uint64_t cpu_max = mix->debug_composite_cpu_max;
		const double cpu_pct = budget_ns > 0.0 ? (double)cpu_avg / budget_ns * 100.0 : 0.0;
		const double cpu_max_pct = budget_ns > 0.0 ? (double)cpu_max / budget_ns * 100.0 : 0.0;

		char gpu_field[64];
		char gpu_max_field[64];
		debug_format_gpu_field(gpu_field, sizeof(gpu_field), gpu_measured, "GPU", gpu_avg, budget_ns);
		debug_format_gpu_field(gpu_max_field, sizeof(gpu_max_field), gpu_measured, "max GPU",
				       mix->debug_composite_gpu_max, budget_ns);

		blog(LOG_INFO,
		     "[render-debug] mix '%s': composite CPU %.1f" NBSP "us (%.1f%% frame)%s, max CPU %.1f" NBSP
		     "us (%.1f%% frame)%s, lagged +%u" NBSP "frames%s",
		     mix->debug_label ? mix->debug_label : "?", (double)cpu_avg / 1000.0, cpu_pct, gpu_field,
		     (double)cpu_max / 1000.0, cpu_max_pct, gpu_max_field, lagged_delta, gpu_note);

		mix->debug_composite_cpu_accum = 0;
		mix->debug_composite_gpu_accum = 0;
		mix->debug_composite_cpu_max = 0;
		mix->debug_composite_gpu_max = 0;
		mix->debug_composite_cpu_count = 0;
		mix->debug_composite_gpu_count = 0;
	}
	pthread_mutex_unlock(&obs->video.mixes_mutex);

	obs_enum_all_sources(debug_emit_source_stats_cb, NULL);
}

/* The single enumeration of the frame's segments: the row list, the count, the
 * format string and the argument list are all generated from it, so adding a
 * segment to the loop is one line here and cannot leave the others behind. The
 * thirteen tile the iteration from its start to video_sleep; the tail and the
 * residual are deliberately not members (see debug_check_frame). */
#define RENDER_DEBUG_SEGMENTS(X)       \
	X("active", seg_active_ns)     \
	X("enter", seg_enter_ns)       \
	X("tick", seg_tick_ns)         \
	X("msg", seg_msg_ns)           \
	X("outlock", seg_outlock_ns)   \
	X("outctx", seg_outctx_ns)     \
	X("output", seg_output_ns)     \
	X("flush", seg_flush_ns)       \
	X("dload", seg_dload_ns)       \
	X("encwait", seg_encwait_ns)        \
	X("displays", seg_displays_ns) \
	X("tasks", seg_tasks_ns)       \
	X("collect", seg_collect_ns)

#define RENDER_DEBUG_SEGMENT_ROW(name, field) {name, context->field},
#define RENDER_DEBUG_SEGMENT_TALLY(name, field) +1
#define RENDER_DEBUG_SEGMENT_FMT(name, field) name " %.1f" NBSP "ms, "
#define RENDER_DEBUG_SEGMENT_ARG(name, field) , obs_debug_ns_to_ms(context->field)

#define RENDER_DEBUG_SEGMENT_COUNT (0 RENDER_DEBUG_SEGMENTS(RENDER_DEBUG_SEGMENT_TALLY))

struct debug_frame_segment {
	const char *name;
	uint64_t ns;
};

static inline uint64_t debug_segment_warn_ns(const struct obs_graphics_context *context)
{
	return (context->interval * RENDER_DEBUG_SEGMENT_WARN_PCT) / 100;
}

/* Only the messages worth recognizing on sight in a lag report -- the ones a
 * display, power, DPI or compositor transition sends, plus the routine traffic
 * frequent enough to be a plausible culprit. Anything else prints as hex, which
 * is enough to look up and enough to tell two culprits apart. */
static const struct {
	uint32_t id;
	const char *name;
} debug_window_messages[] = {
	{0x0002, "WM_DESTROY"},
	{0x0005, "WM_SIZE"},
	{0x000F, "WM_PAINT"},
	{0x0014, "WM_ERASEBKGND"},
	{0x0018, "WM_SHOWWINDOW"},
	{0x001A, "WM_SETTINGCHANGE"},
	{0x0020, "WM_SETCURSOR"},
	{0x0046, "WM_WINDOWPOSCHANGING"},
	{0x0047, "WM_WINDOWPOSCHANGED"},
	{0x007E, "WM_DISPLAYCHANGE"},
	{0x0084, "WM_NCHITTEST"},
	{0x0113, "WM_TIMER"},
	{0x0200, "WM_MOUSEMOVE"},
	{0x0218, "WM_POWERBROADCAST"},
	{0x0219, "WM_DEVICECHANGE"},
	{0x02E0, "WM_DPICHANGED"},
	{0x031E, "WM_DWMCOMPOSITIONCHANGED"},
};

#ifdef _WIN32
/* Names the windows the graphics thread owns. A SendMessage from any other
 * thread must target one of these, and that wait is serviced inside
 * PeekMessage where no dispatch timer can see it -- so this list is the
 * difference between a measured stall and an unattributable one. Emitted once,
 * on the first slow pump, because the windows are created lazily and a probe at
 * thread start would find an empty set. */
static BOOL CALLBACK debug_log_thread_window(HWND hwnd, LPARAM param)
{
	UNUSED_PARAMETER(param);

	char cls[64] = "";
	GetClassNameA(hwnd, cls, (int)sizeof cls);
	blog(LOG_INFO, "[render-debug] graphics thread owns hwnd 0x%llx class '%s'",
	     (unsigned long long)(uintptr_t)hwnd, cls);
	return TRUE;
}
#endif

static const char *debug_window_message_name(uint32_t id, char *buf, size_t size)
{
	for (size_t i = 0; i < sizeof(debug_window_messages) / sizeof(debug_window_messages[0]); i++) {
		if (debug_window_messages[i].id == id) {
			return debug_window_messages[i].name;
		}
	}

	snprintf(buf, size, "0x%04X", id);
	return buf;
}

static size_t debug_fill_segments(const struct obs_graphics_context *context,
				  struct debug_frame_segment out[RENDER_DEBUG_SEGMENT_COUNT])
{
	const struct debug_frame_segment segments[] = {RENDER_DEBUG_SEGMENTS(RENDER_DEBUG_SEGMENT_ROW)};

	memcpy(out, segments, sizeof(segments));
	return sizeof(segments) / sizeof(segments[0]);
}

static void debug_emit_frame_segments(const struct obs_graphics_context *context, const char *head, uint64_t span_ns)
{
	struct debug_frame_segment segments[RENDER_DEBUG_SEGMENT_COUNT];
	const size_t num = debug_fill_segments(context, segments);
	char acquirer[160] = "";
	char pump[224] = "";
	char msg_name[16];
	uint64_t accounted = 0;

	for (size_t i = 0; i < num; i++) {
		accounted += segments[i].ns;
	}

	/* The parts are disjoint sub-intervals of the span, so this cannot go
	 * negative; the clamp is only so a future edit that breaks that
	 * invariant prints zero rather than an underflowed 18-exabyte residual. */
	const uint64_t residual = span_ns > accounted ? span_ns - accounted : 0;

	/* Only worth naming when the graphics thread actually waited on the
	 * context; otherwise the record just describes ordinary traffic. */
	if (context->seg_enter_ns >= debug_segment_warn_ns(context)) {
		uint32_t age_ms = 0;
		bool held = false;
		const char *who = obs_last_graphics_acquirer(&age_ms, &held);
		snprintf(acquirer, sizeof(acquirer),
			 held ? ", graphics context held by %s, taken %u" NBSP "ms earlier"
			      : ", graphics context last held by %s, released %u" NBSP "ms earlier",
			 who, age_ms);
	}

	/* Same rule as the acquirer above: name the handler only when the pump is
	 * actually where the frame went, so ordinary traffic stays unannotated. */
	if (context->seg_msg_ns >= debug_segment_warn_ns(context)) {
		const uint64_t peek_ns = context->seg_msg_ns > context->msg_dispatch_ns
						 ? context->seg_msg_ns - context->msg_dispatch_ns
						 : 0;
		snprintf(pump, sizeof(pump),
			 ", pump dispatched %u" NBSP "msg in %.1f" NBSP "ms (worst %s to hwnd 0x%llx at %.1f" NBSP
			 "ms), %.1f" NBSP "ms across %u" NBSP "PeekMessage (worst single %.1f" NBSP "ms)",
			 context->msg_count, obs_debug_ns_to_ms(context->msg_dispatch_ns),
			 debug_window_message_name(context->msg_worst_id, msg_name, sizeof msg_name),
			 (unsigned long long)context->msg_worst_hwnd, obs_debug_ns_to_ms(context->msg_worst_ns),
			 obs_debug_ns_to_ms(peek_ns), context->msg_peek_calls,
			 obs_debug_ns_to_ms(context->msg_peek_worst_ns));
	}

	blog(LOG_INFO,
	     "[render-debug] %s: total %.1f" NBSP
	     "ms -- " RENDER_DEBUG_SEGMENTS(RENDER_DEBUG_SEGMENT_FMT) "residual %.1f" NBSP "ms; prev tail %.1f" NBSP
								      "ms%s%s",
	     head, obs_debug_ns_to_ms(span_ns) RENDER_DEBUG_SEGMENTS(RENDER_DEBUG_SEGMENT_ARG),
	     obs_debug_ns_to_ms(residual), obs_debug_ns_to_ms(context->seg_tail_ns), acquirer, pump);
}

/* Whether the early-warning line may emit now. Counts what it turns away so the
 * next line that does get through can disclose the total. */
static bool debug_warn_allowed(struct obs_graphics_context *context, uint64_t now_ns)
{
	if (now_ns - context->warn_window_start_ns >= RENDER_DEBUG_WARN_WINDOW_NS) {
		context->warn_window_start_ns = now_ns;
		context->warn_in_window = 0;
	}

	if (context->warn_in_window >= RENDER_DEBUG_WARN_MAX_PER_WINDOW) {
		context->warn_suppressed++;
		return false;
	}

	context->warn_in_window++;
	return true;
}

/* Reports the frame that just ran: a miss when video_sleep found the deadline
 * already gone, otherwise a single overlong segment as an early warning. The
 * miss line is never rate limited -- those are rare and losing one costs more
 * than emitting it. The residual and the tail are excluded from the warning
 * trigger because both carry this diagnostic's own logging cost, so warning on
 * them would be self-sustaining; both are still printed on every line. */
static void debug_check_frame(struct obs_graphics_context *context, uint32_t lost_frames, uint64_t span_ns,
			      uint64_t now_ns)
{
	char head[160];
	char suppressed[48] = "";

	/* Built before this frame's own rate-limit decision, so it reports the
	 * backlog rather than including the line about to be printed. Both exits
	 * below clear the counter, so a backlog cannot outlive the next line of
	 * either kind. */
	if (context->warn_suppressed) {
		snprintf(suppressed, sizeof(suppressed), " (%u" NBSP "similar suppressed)", context->warn_suppressed);
	}

	if (lost_frames) {
		snprintf(head, sizeof(head), "frame overran: lost %u" NBSP "frames%s", lost_frames, suppressed);
		context->warn_suppressed = 0;
		debug_emit_frame_segments(context, head, span_ns);
		return;
	}

	struct debug_frame_segment segments[RENDER_DEBUG_SEGMENT_COUNT];
	const size_t num = debug_fill_segments(context, segments);
	const struct debug_frame_segment *worst = &segments[0];

	for (size_t i = 1; i < num; i++) {
		if (segments[i].ns > worst->ns) {
			worst = &segments[i];
		}
	}

	if (worst->ns < debug_segment_warn_ns(context) || !debug_warn_allowed(context, now_ns)) {
		return;
	}

	snprintf(head, sizeof(head), "slow segment '%s' %.1f" NBSP "ms%s", worst->name, obs_debug_ns_to_ms(worst->ns),
		 suppressed);
	context->warn_suppressed = 0;

	debug_emit_frame_segments(context, head, span_ns);
}

bool obs_graphics_thread_loop(struct obs_graphics_context *context)
{
	uint64_t frame_start = os_gettime_ns();
	uint64_t frame_time_ns;
	uint64_t seg_start;

	/* The segment timers here and below are unconditional, unlike the logging
	 * they feed. The clock reads cost on the order of 0.001% of a 16.67 ms
	 * frame, and gating them would mean a lag event is only ever captured
	 * when the debug flag happened to be armed before the frame that lost
	 * it. */
	seg_start = frame_start;
	update_active_states();
	context->seg_active_ns = os_gettime_ns() - seg_start;

	profile_start(context->video_thread_name);
	source_profiler_frame_begin();

	profile_start(enter_context_name);
	seg_start = os_gettime_ns();
	gs_enter_context(obs->video.graphics);
	gs_begin_frame();
	gs_leave_context();
	context->seg_enter_ns = os_gettime_ns() - seg_start;
	profile_end(enter_context_name);

	profile_start(tick_sources_name);
	seg_start = os_gettime_ns();
	context->last_time = tick_sources(obs->video.video_time, context->last_time);
	context->seg_tick_ns = os_gettime_ns() - seg_start;
	profile_end(tick_sources_name);

#ifdef _WIN32
	profile_start(message_loop_name);
	seg_start = os_gettime_ns();
	context->msg_worst_ns = 0;
	context->msg_dispatch_ns = 0;
	context->msg_worst_hwnd = 0;
	context->msg_worst_id = 0;
	context->msg_count = 0;
	context->msg_peek_worst_ns = 0;
	context->msg_peek_calls = 0;
	MSG msg;
	/* A few clock reads per message and per peek. A frame that pumps a hundred
	 * of them pays microseconds, which buys the identity of whichever half
	 * overran -- and it is PeekMessage, not the handlers: every observed stall
	 * reports dispatch at 0.0 ms. Sent messages from other threads are serviced
	 * inside PeekMessage itself, so they are invisible to DispatchMessage
	 * timing; timing the peeks is the only way to see them from here. */
	for (;;) {
		const uint64_t peek_start = os_gettime_ns();
		const BOOL got = PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
		const uint64_t peek_ns = os_gettime_ns() - peek_start;
		context->msg_peek_calls++;
		if (peek_ns > context->msg_peek_worst_ns) {
			context->msg_peek_worst_ns = peek_ns;
		}
		if (!got) {
			break;
		}

		const uint64_t msg_start = os_gettime_ns();
		TranslateMessage(&msg);
		DispatchMessage(&msg);
		const uint64_t msg_ns = os_gettime_ns() - msg_start;
		context->msg_dispatch_ns += msg_ns;
		if (msg_ns > context->msg_worst_ns) {
			context->msg_worst_ns = msg_ns;
			context->msg_worst_hwnd = (uintptr_t)msg.hwnd;
			context->msg_worst_id = (uint32_t)msg.message;
		}
		context->msg_count++;
	}
	context->seg_msg_ns = os_gettime_ns() - seg_start;
	if (!context->msg_windows_logged && context->seg_msg_ns >= debug_segment_warn_ns(context)) {
		context->msg_windows_logged = true;
		EnumThreadWindows(GetCurrentThreadId(), debug_log_thread_window, 0);
	}
	profile_end(message_loop_name);
#else
	context->seg_msg_ns = 0;
	context->msg_worst_ns = 0;
	context->msg_count = 0;
#endif

	source_profiler_render_begin();
	profile_start(output_frame_name);
	seg_start = os_gettime_ns();
	struct output_frame_timing oft;
	output_frames(&context->seg_outlock_ns, &oft);
	context->seg_outctx_ns = oft.ctx_ns;
	context->seg_flush_ns = oft.flush_ns;
	context->seg_dload_ns = oft.dload_ns;
	context->seg_encwait_ns = oft.encwait_ns;
	context->seg_output_ns = (os_gettime_ns() - seg_start) - context->seg_outlock_ns - oft.ctx_ns - oft.flush_ns -
				 oft.dload_ns - oft.encwait_ns;
	profile_end(output_frame_name);

	profile_start(render_displays_name);
	seg_start = os_gettime_ns();
	render_displays();
	context->seg_displays_ns = os_gettime_ns() - seg_start;
	profile_end(render_displays_name);
	source_profiler_render_end();

	profile_start(graphics_tasks_name);
	seg_start = os_gettime_ns();
	const uint64_t task_log_ns = execute_graphics_tasks();
	context->seg_tasks_ns = (os_gettime_ns() - seg_start) - task_log_ns;
	profile_end(graphics_tasks_name);

	frame_time_ns = os_gettime_ns() - frame_start;

	/* source_profiler_frame_collect opens its own profiler node, and does a
	 * GPU query readback under the graphics context, so it is timed here
	 * rather than left outside the accounting as its position after
	 * frame_time_ns would otherwise leave it. */
	seg_start = os_gettime_ns();
	source_profiler_frame_collect();
	context->seg_collect_ns = os_gettime_ns() - seg_start;

	profile_end(context->video_thread_name);

	profile_reenable_thread();

	/* The span the breakdown reconciles against: the whole iteration up to
	 * the sleep. frame_time_ns stops short of the collect above and is left
	 * that way because obs publishes it as video_avg_frame_time_ns. */
	const uint64_t span_ns = os_gettime_ns() - frame_start;

	video_sleep(&obs->video, &obs->video.video_time, context->interval);

	const uint64_t tail_start = os_gettime_ns();

	/* video_sleep is where lagged_frames moves, so a growth across the call
	 * above indicts the frame whose segments were just measured. */
	const uint32_t lagged_now = obs->video.lagged_frames_raw;
	if (os_atomic_load_bool(&obs->video.render_debug)) {
		debug_check_frame(context, lagged_now - context->last_lagged_frames, span_ns, tail_start);
	}
	context->last_lagged_frames = lagged_now;

	context->frame_time_total_ns += frame_time_ns;
	context->fps_total_ns += (obs->video.video_time - context->last_time);
	context->fps_total_frames++;

	if (context->fps_total_ns >= 1000000000ULL) {
		obs->video.video_fps =
			(double)context->fps_total_frames / ((double)context->fps_total_ns / 1000000000.0);
		obs->video.video_avg_frame_time_ns = context->frame_time_total_ns / (uint64_t)context->fps_total_frames;

		if (os_atomic_load_bool(&obs->video.render_debug)) {
			debug_emit_composite_stats();
		}

		context->frame_time_total_ns = 0;
		context->fps_total_ns = 0;
		context->fps_total_frames = 0;
	}

	const bool keep_going = !stop_requested();

	/* Everything after video_sleep -- the rollup, stop_requested and this
	 * diagnostic's own logging -- runs inside the NEXT frame's deadline, so
	 * it is carried forward and reported there rather than charged to
	 * nothing. */
	context->seg_tail_ns = os_gettime_ns() - tail_start;

	return keep_going;
}

void *obs_graphics_thread(void *param)
{
#ifdef _WIN32
	struct winrt_state winrt;
	init_winrt_state(&winrt);
#endif // #ifdef _WIN32

	is_graphics_thread = true;

	const uint64_t interval = obs->video.video_frame_interval_ns;

	obs->video.video_time = os_gettime_ns();

	os_set_thread_name("libobs: graphics thread");

	const char *video_thread_name = profile_store_name(obs_get_profiler_name_store(),
							   "obs_graphics_thread(%g" NBSP "ms)", interval / 1000000.);
	profile_register_root(video_thread_name, interval);

	srand((unsigned int)time(NULL));

	struct obs_graphics_context context;
	context.interval = interval;
	context.frame_time_total_ns = 0;
	context.fps_total_ns = 0;
	context.fps_total_frames = 0;
	context.last_time = 0;
	context.video_thread_name = video_thread_name;
	context.seg_active_ns = 0;
	context.seg_enter_ns = 0;
	context.seg_tick_ns = 0;
	context.seg_msg_ns = 0;
	context.seg_outlock_ns = 0;
	context.seg_outctx_ns = 0;
	context.seg_output_ns = 0;
	context.seg_flush_ns = 0;
	context.seg_dload_ns = 0;
	context.seg_encwait_ns = 0;
	context.seg_displays_ns = 0;
	context.seg_tasks_ns = 0;
	context.seg_collect_ns = 0;
	context.seg_tail_ns = 0;
	context.msg_worst_ns = 0;
	context.msg_dispatch_ns = 0;
	context.msg_peek_worst_ns = 0;
	context.msg_peek_calls = 0;
	context.msg_windows_logged = false;
	context.msg_worst_hwnd = 0;
	context.msg_worst_id = 0;
	context.msg_count = 0;
	context.last_lagged_frames = obs->video.lagged_frames_raw;
	context.warn_window_start_ns = os_gettime_ns();
	context.warn_in_window = 0;
	context.warn_suppressed = 0;

#ifdef __APPLE__
	while (obs_graphics_thread_loop_autorelease(&context))
#else
	while (obs_graphics_thread_loop(&context))
#endif
		;

#ifdef _WIN32
	uninit_winrt_state(&winrt);
#endif

	UNUSED_PARAMETER(param);
	return NULL;
}
