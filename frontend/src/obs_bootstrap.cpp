#include "obs_bootstrap.hpp"

#include <obs.h>

#include <windows.h>

#include <string>

#include "log.hpp"
#include "paths.hpp"

bool ObsBootstrap::Start()
{
	if (!obs_startup("en-US", nullptr, nullptr)) {
		HostLog("[obs] obs_startup failed");
		return false;
	}
	HostLog("[obs] obs_startup ok");

	const std::string root = RundirRoot();
	obs_add_data_path((root + "/data/libobs/").c_str());

	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = 1920;
	ovi.base_height = 1080;
	ovi.output_width = 1920;
	ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		HostLog("[obs] obs_reset_video failed, code=" + std::to_string(rv));
		return false;
	}
	HostLog("[obs] obs_reset_video ok (1920x1080@60, D3D11)");

	obs_audio_info oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		HostLog("[obs] obs_reset_audio failed");
		return false;
	}
	HostLog("[obs] obs_reset_audio ok (48kHz stereo)");

	// 4.1.1 loads no plugins. obs_post_load_modules still runs so libobs
	// finalizes the (empty) module set cleanly, matching the normal init order.
	obs_post_load_modules();
	HostLog("[obs] core up (no plugins; shell skeleton)");

	return true;
}

void ObsBootstrap::Stop()
{
	// Deferred source destruction can cascade across the destruction-task
	// thread; a single drain races obs_shutdown and can re-enter
	// obs_source_destroy on an already-released source. The Qt frontend
	// (OBSBasic::ClearSceneData) drains in a loop until no more work is spawned
	// before obs_shutdown -- mirror that even though the skeleton owns no scene
	// yet, so the order is correct once 4.1.2+ add sources.
	while (obs_wait_for_destroy_queue()) {
	}

	obs_shutdown();

	// Same counter the legacy frontend prints. Nonzero == fixed libobs static
	// residuals (no per-run growth), not host-introduced leaks.
	HostLog("[obs] leaks: " + std::to_string(bnum_allocs()));
	HostLog("[obs] shutdown complete");
}
