#include "frontend_callbacks.hpp"

#include <util/config-file.h>

#include <windows.h>

config_t *FrontendCallbacks::EnsureConfig()
{
	std::lock_guard<std::mutex> lock(configMutex);
	if (configTried) {
		return config;
	}
	configTried = true;

	char tempPath[MAX_PATH] = {0};
	DWORD n = GetTempPathA(MAX_PATH, tempPath);
	std::string path;
	if (n > 0 && n < MAX_PATH) {
		path = std::string(tempPath) + "braidcast-frontend.ini";
	} else {
		path = "braidcast-frontend.ini";
	}

	config = config_create(path.c_str());
	return config;
}

FrontendCallbacks::~FrontendCallbacks()
{
	if (config) {
		config_close(config);
		config = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Inert stubs (typed-zero) except where a real body is noted.
// ---------------------------------------------------------------------------

void *FrontendCallbacks::obs_frontend_get_main_window(void)
{
	return nullptr;
}
void *FrontendCallbacks::obs_frontend_get_main_window_handle(void)
{
	return nullptr;
}
void *FrontendCallbacks::obs_frontend_get_system_tray(void)
{
	return nullptr;
}

void FrontendCallbacks::obs_frontend_get_scenes(struct obs_frontend_source_list *sources)
{
	(void)sources; // leave the caller-init'd list empty
}
obs_source_t *FrontendCallbacks::obs_frontend_get_current_scene(void)
{
	return obs_get_output_source(0); // already addref'd by libobs
}
void FrontendCallbacks::obs_frontend_set_current_scene(obs_source_t *scene)
{
	(void)scene;
}

void FrontendCallbacks::obs_frontend_get_transitions(struct obs_frontend_source_list *sources)
{
	(void)sources;
}
obs_source_t *FrontendCallbacks::obs_frontend_get_current_transition(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_set_current_transition(obs_source_t *transition)
{
	(void)transition;
}
int FrontendCallbacks::obs_frontend_get_transition_duration(void)
{
	return 0;
}
void FrontendCallbacks::obs_frontend_set_transition_duration(int duration)
{
	(void)duration;
}
void FrontendCallbacks::obs_frontend_release_tbar(void) {}
int FrontendCallbacks::obs_frontend_get_tbar_position(void)
{
	return 0;
}
void FrontendCallbacks::obs_frontend_set_tbar_position(int position)
{
	(void)position;
}

void FrontendCallbacks::obs_frontend_get_scene_collections(std::vector<std::string> &strings)
{
	(void)strings;
}
char *FrontendCallbacks::obs_frontend_get_current_scene_collection(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_set_current_scene_collection(const char *collection)
{
	(void)collection;
}
bool FrontendCallbacks::obs_frontend_add_scene_collection(const char *name)
{
	(void)name;
	return false;
}

void FrontendCallbacks::obs_frontend_get_profiles(std::vector<std::string> &strings)
{
	(void)strings;
}
char *FrontendCallbacks::obs_frontend_get_current_profile(void)
{
	return nullptr;
}
char *FrontendCallbacks::obs_frontend_get_current_profile_path(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_set_current_profile(const char *profile)
{
	(void)profile;
}
void FrontendCallbacks::obs_frontend_create_profile(const char *name)
{
	(void)name;
}
void FrontendCallbacks::obs_frontend_duplicate_profile(const char *name)
{
	(void)name;
}
void FrontendCallbacks::obs_frontend_delete_profile(const char *profile)
{
	(void)profile;
}

void FrontendCallbacks::obs_frontend_streaming_start(void) {}
void FrontendCallbacks::obs_frontend_streaming_stop(void) {}
bool FrontendCallbacks::obs_frontend_streaming_active(void)
{
	return false;
}

void FrontendCallbacks::obs_frontend_recording_start(void) {}
void FrontendCallbacks::obs_frontend_recording_stop(void) {}
bool FrontendCallbacks::obs_frontend_recording_active(void)
{
	return false;
}
void FrontendCallbacks::obs_frontend_recording_pause(bool pause)
{
	(void)pause;
}
bool FrontendCallbacks::obs_frontend_recording_paused(void)
{
	return false;
}
bool FrontendCallbacks::obs_frontend_recording_split_file(void)
{
	return false;
}
bool FrontendCallbacks::obs_frontend_recording_add_chapter(const char *name)
{
	(void)name;
	return false;
}

void FrontendCallbacks::obs_frontend_replay_buffer_start(void) {}
void FrontendCallbacks::obs_frontend_replay_buffer_save(void) {}
void FrontendCallbacks::obs_frontend_replay_buffer_stop(void) {}
bool FrontendCallbacks::obs_frontend_replay_buffer_active(void)
{
	return false;
}

void *FrontendCallbacks::obs_frontend_add_tools_menu_qaction(const char *name)
{
	(void)name;
	return nullptr;
}
void FrontendCallbacks::obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data)
{
	(void)name;
	(void)callback;
	(void)private_data;
}

bool FrontendCallbacks::obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget)
{
	(void)id;
	(void)title;
	(void)widget;
	return false;
}
void FrontendCallbacks::obs_frontend_remove_dock(const char *id)
{
	(void)id;
}
bool FrontendCallbacks::obs_frontend_add_custom_qdock(const char *id, void *dock)
{
	(void)id;
	(void)dock;
	return false;
}

void FrontendCallbacks::obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	eventCbs.push_back({callback, private_data});
}
void FrontendCallbacks::obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	for (auto it = eventCbs.begin(); it != eventCbs.end(); ++it) {
		if (it->cb == callback && it->data == private_data) {
			eventCbs.erase(it);
			break;
		}
	}
}

obs_output_t *FrontendCallbacks::obs_frontend_get_streaming_output(void)
{
	return nullptr;
}
obs_output_t *FrontendCallbacks::obs_frontend_get_recording_output(void)
{
	return nullptr;
}
obs_output_t *FrontendCallbacks::obs_frontend_get_replay_buffer_output(void)
{
	return nullptr;
}

config_t *FrontendCallbacks::obs_frontend_get_profile_config(void)
{
	return EnsureConfig();
}
config_t *FrontendCallbacks::obs_frontend_get_app_config(void)
{
	return EnsureConfig();
}
config_t *FrontendCallbacks::obs_frontend_get_user_config(void)
{
	return EnsureConfig();
}

void FrontendCallbacks::obs_frontend_open_projector(const char *type, int monitor, const char *geometry,
						    const char *name)
{
	(void)type;
	(void)monitor;
	(void)geometry;
	(void)name;
}
void FrontendCallbacks::obs_frontend_save(void) {}
void FrontendCallbacks::obs_frontend_defer_save_begin(void) {}
void FrontendCallbacks::obs_frontend_defer_save_end(void) {}
void FrontendCallbacks::obs_frontend_add_save_callback(obs_frontend_save_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	saveCbs.push_back({callback, private_data});
}
void FrontendCallbacks::obs_frontend_remove_save_callback(obs_frontend_save_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	for (auto it = saveCbs.begin(); it != saveCbs.end(); ++it) {
		if (it->cb == callback && it->data == private_data) {
			saveCbs.erase(it);
			break;
		}
	}
}

void FrontendCallbacks::obs_frontend_add_preload_callback(obs_frontend_save_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	preloadCbs.push_back({callback, private_data});
}
void FrontendCallbacks::obs_frontend_remove_preload_callback(obs_frontend_save_cb callback, void *private_data)
{
	std::lock_guard<std::mutex> lock(cbMutex);
	for (auto it = preloadCbs.begin(); it != preloadCbs.end(); ++it) {
		if (it->cb == callback && it->data == private_data) {
			preloadCbs.erase(it);
			break;
		}
	}
}

void FrontendCallbacks::obs_frontend_push_ui_translation(obs_frontend_translate_ui_cb translate)
{
	(void)translate;
}
void FrontendCallbacks::obs_frontend_pop_ui_translation(void) {}

obs_service_t *FrontendCallbacks::obs_frontend_get_streaming_service(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_set_streaming_service(obs_service_t *service)
{
	(void)service;
}
void FrontendCallbacks::obs_frontend_save_streaming_service() {}

bool FrontendCallbacks::obs_frontend_preview_program_mode_active(void)
{
	return false;
}
void FrontendCallbacks::obs_frontend_set_preview_program_mode(bool enable)
{
	(void)enable;
}
void FrontendCallbacks::obs_frontend_preview_program_trigger_transition(void) {}

bool FrontendCallbacks::obs_frontend_preview_enabled(void)
{
	return false;
}
void FrontendCallbacks::obs_frontend_set_preview_enabled(bool enable)
{
	(void)enable;
}

obs_source_t *FrontendCallbacks::obs_frontend_get_current_preview_scene(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_set_current_preview_scene(obs_source_t *scene)
{
	(void)scene;
}

void FrontendCallbacks::on_load(obs_data_t *settings)
{
	std::vector<SaveCb> snapshot;
	{
		std::lock_guard<std::mutex> lock(cbMutex);
		snapshot = saveCbs;
	}
	for (auto &s : snapshot) {
		if (s.cb) {
			s.cb(settings, false, s.data);
		}
	}
}
void FrontendCallbacks::on_preload(obs_data_t *settings)
{
	std::vector<SaveCb> snapshot;
	{
		std::lock_guard<std::mutex> lock(cbMutex);
		snapshot = preloadCbs;
	}
	for (auto &s : snapshot) {
		if (s.cb) {
			s.cb(settings, false, s.data);
		}
	}
}
void FrontendCallbacks::on_save(obs_data_t *settings)
{
	std::vector<SaveCb> snapshot;
	{
		std::lock_guard<std::mutex> lock(cbMutex);
		snapshot = saveCbs;
	}
	for (auto &s : snapshot) {
		if (s.cb) {
			s.cb(settings, true, s.data);
		}
	}
}
void FrontendCallbacks::on_event(enum obs_frontend_event event)
{
	std::vector<EventCb> snapshot;
	{
		std::lock_guard<std::mutex> lock(cbMutex);
		snapshot = eventCbs;
	}
	for (auto &e : snapshot) {
		if (e.cb) {
			e.cb(event, e.data);
		}
	}
}

void FrontendCallbacks::obs_frontend_take_screenshot() {}
void FrontendCallbacks::obs_frontend_take_source_screenshot(obs_source_t *source)
{
	(void)source;
}

obs_output_t *FrontendCallbacks::obs_frontend_get_virtualcam_output(void)
{
	return nullptr;
}
void FrontendCallbacks::obs_frontend_start_virtualcam(void) {}
void FrontendCallbacks::obs_frontend_stop_virtualcam(void) {}
bool FrontendCallbacks::obs_frontend_virtualcam_active(void)
{
	return false;
}

void FrontendCallbacks::obs_frontend_reset_video(void) {}

void FrontendCallbacks::obs_frontend_open_source_properties(obs_source_t *source)
{
	(void)source;
}
void FrontendCallbacks::obs_frontend_open_source_filters(obs_source_t *source)
{
	(void)source;
}
void FrontendCallbacks::obs_frontend_open_source_interaction(obs_source_t *source)
{
	(void)source;
}
void FrontendCallbacks::obs_frontend_open_sceneitem_edit_transform(obs_sceneitem_t *item)
{
	(void)item;
}

char *FrontendCallbacks::obs_frontend_get_current_record_output_path(void)
{
	return nullptr;
}
const char *FrontendCallbacks::obs_frontend_get_locale_string(const char *string)
{
	return string;
}

bool FrontendCallbacks::obs_frontend_is_theme_dark(void)
{
	return false;
}

char *FrontendCallbacks::obs_frontend_get_last_recording(void)
{
	return nullptr;
}
char *FrontendCallbacks::obs_frontend_get_last_screenshot(void)
{
	return nullptr;
}
char *FrontendCallbacks::obs_frontend_get_last_replay(void)
{
	return nullptr;
}

void FrontendCallbacks::obs_frontend_add_undo_redo_action(const char *name, const undo_redo_cb undo,
							  const undo_redo_cb redo, const char *undo_data,
							  const char *redo_data, bool repeatable)
{
	(void)name;
	(void)undo;
	(void)redo;
	(void)undo_data;
	(void)redo_data;
	(void)repeatable;
}

obs_canvas_t *FrontendCallbacks::obs_frontend_add_canvas(const char *name, obs_video_info *ovi, int flags)
{
	(void)name;
	(void)ovi;
	(void)flags;
	return nullptr;
}
bool FrontendCallbacks::obs_frontend_remove_canvas(obs_canvas_t *canvas)
{
	(void)canvas;
	return false;
}
void FrontendCallbacks::obs_frontend_get_canvases(obs_frontend_canvas_list *canvas_list)
{
	(void)canvas_list;
}

void FrontendCallbacks::obs_frontend_copy_sceneitem(obs_sceneitem_t *item)
{
	(void)item;
}
bool FrontendCallbacks::obs_frontend_can_paste_sceneitem(bool duplicate)
{
	(void)duplicate;
	return false;
}
void FrontendCallbacks::obs_frontend_paste_sceneitem(obs_scene_t *scene, bool duplicate)
{
	(void)scene;
	(void)duplicate;
}
