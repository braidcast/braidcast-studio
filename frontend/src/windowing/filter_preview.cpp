// bridge.hpp pulls in the CEF headers, whose CefDOMNode declares methods like
// GetNextSibling that <windows.h> would otherwise macro-clobber. Include it (and
// thus CEF) before any Windows header so CEF parses clean.
#include "bridge.hpp"
#include "event_names.hpp"

#include "filter_preview.hpp"
#include "overlay_surface.hpp"
#include "source_render.hpp"
#include "multistream/VideoGate.hpp"

#include <obs.h>

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"

#include <string>
#include <utility>

#include "log.hpp"

// Draw-callback state: only the addref-pinned source, set on the UI thread before a
// display is created and cleared after the display is gone. The render thread reads
// it lock-free; the source outlives the display, so the read is safe.
struct FilterPreview::State {
	obs_source_t *source = nullptr;
};

namespace {

FilterPreview *g_instance = nullptr;

// UI-thread half of the "remove" signal: close the preview if it is still bound to
// the removed source. Reached only through CefPostTask, so it never runs inside the
// signal dispatch -- releasing our ref there could destroy the source, and its
// signal handler, mid-iteration.
void CloseForRemovedSource(std::string uuid)
{
	FilterPreview *fp = FilterPreviewHost::Instance();
	if (fp && !uuid.empty() && fp->SourceUuid() == uuid) {
		fp->OnSourceRemoved();
	}
}

// The bound source was deleted (or its collection swapped). Fires from libobs on an
// unspecified thread, so it reads the removed source's uuid off the calldata (both
// the pointer and the uuid are immutable for the call) rather than touching any
// UI-thread state, and defers the close itself.
void OnSourceRemovedSignal(void * /*data*/, calldata_t *cd)
{
	auto *removed = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	const char *uuid = removed ? obs_source_get_uuid(removed) : nullptr;
	if (!uuid) {
		return;
	}
	CefPostTask(TID_UI, base::BindOnce(&CloseForRemovedSource, std::string(uuid)));
}

// Whether a source can be previewed standalone. Mirrors upstream OBSBasicFilters:
// it shows its preview only for a source carrying OBS_SOURCE_VIDEO, and only draws
// into it for an input or a scene -- a transition or a filter has no standalone
// composite to render.
bool IsPreviewable(obs_source_t *source)
{
	if (!source) {
		return false;
	}
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
		return false;
	}
	const obs_source_type type = obs_source_get_type(source);
	return type == OBS_SOURCE_TYPE_INPUT || type == OBS_SOURCE_TYPE_SCENE;
}

// Draw callback: fired by libobs once per frame on the render thread. cx/cy are the
// overlay HWND's pixel size. Rendering the PARENT source is what makes this a filter
// preview at all -- obs_source_video_render walks the source's filter chain, so every
// edit in the dialog lands in the next frame.
void RenderFilterPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *state = static_cast<FilterPreview::State *>(data);
	SourceRender::Letterboxed(state->source, cx, cy);
}

} // namespace

FilterPreview::FilterPreview(HWND host, HINSTANCE instance) : host_(host), instance_(instance), state_(new State()) {}

FilterPreview::~FilterPreview()
{
	Close();
	delete state_;
}

bool FilterPreview::Open(obs_source_t *source, std::string &error)
{
	// Unconditionally first: a rebind to a source with nothing to preview must still
	// drop the previous binding, or the dialog would keep showing the old source.
	Close();

	if (!source) {
		error = "no source to preview";
		return false;
	}
	if (!IsPreviewable(source)) {
		error = "source has no video to preview";
		return false;
	}
	if (!host_) {
		error = "no host window for the preview overlay";
		return false;
	}

	// Our OWN ref for the binding's whole life (the caller still owns + releases its
	// ref), released last in Close().
	obs_source_t *pinned = obs_source_get_ref(source);
	if (!pinned) {
		error = "failed to acquire source reference";
		return false;
	}
	source_ = pinned;
	const char *uuid = obs_source_get_uuid(pinned);
	sourceUuid_ = uuid ? uuid : "";
	state_->source = pinned;

	// The dialog previews a source directly rather than through a canvas preview, so
	// the video gate would see no consumer and stop it capturing the moment Main goes
	// idle -- a black, frozen preview exactly when the user is trying to judge a
	// filter. Register as a consumer before any display exists; balanced in Close().
	VideoGate::IncShowing(source_);
	showRefHeld_ = true;

	// Track the source's lifetime: a delete while the dialog is open must not leave
	// the overlay rendering (and our hard ref keeping alive) a removed source.
	// Disconnected in Close() before the ref is dropped.
	if (signal_handler_t *handler = obs_source_get_signal_handler(source_)) {
		signal_handler_connect(handler, "remove", OnSourceRemovedSignal, this);
	}

	// The overlay HWND + display are created on the first SetRect, so the dialog owns
	// the geometry and an unpositioned preview costs nothing.
	overlay_ = std::make_unique<OverlaySurface>(host_, instance_, RenderFilterPreview, state_, nullptr,
						    "filter-preview");

	const char *name = obs_source_get_name(source_);
	HostLog("[filter-preview] opened for '" + std::string(name ? name : "") + "'");
	return true;
}

void FilterPreview::SetRect(int x, int y, int cx, int cy)
{
	if (overlay_) {
		overlay_->SetRect(x, y, cx, cy);
	}
}

void FilterPreview::Hide()
{
	if (overlay_) {
		overlay_->Hide();
	}
}

void FilterPreview::Close()
{
	// Display first: the draw callback must be gone before anything it reads can be
	// freed. Then the gate hold (while the source is still alive to decrement
	// against), then the signal, then our ref LAST.
	overlay_.reset();

	if (showRefHeld_ && source_) {
		VideoGate::DecShowing(source_);
	}
	showRefHeld_ = false;

	if (source_) {
		if (signal_handler_t *sh = obs_source_get_signal_handler(source_)) {
			signal_handler_disconnect(sh, "remove", OnSourceRemovedSignal, this);
		}
		obs_source_release(source_);
		source_ = nullptr;
		state_->source = nullptr;
		HostLog("[filter-preview] closed");
	}
	sourceUuid_.clear();
}

void FilterPreview::OnSourceRemoved()
{
	Close();
	Bridge::EmitEvent(EventNames::kFilterPreviewClosed, Bridge::json::object());
}

bool FilterPreview::HasDisplayForTest() const
{
	return overlay_ && overlay_->HasDisplay();
}

namespace FilterPreviewHost {

void SetInstance(FilterPreview *fp)
{
	g_instance = fp;
}

FilterPreview *Instance()
{
	return g_instance;
}

} // namespace FilterPreviewHost
