#ifndef OBS_MULTISTREAM_FRONTEND_FILTER_PREVIEW_HPP_
#define OBS_MULTISTREAM_FRONTEND_FILTER_PREVIEW_HPP_

#include <windows.h>

#include <memory>
#include <string>

struct obs_source;
typedef struct obs_source obs_source_t;

class OverlaySurface;

// The Filters dialog's live preview: the source being filtered, rendered with its
// whole filter chain applied, into a native overlay positioned over a placeholder
// element inside the dialog.
//
// Why a native overlay rather than the dialog simply not hiding the main preview:
// the preview is a child HWND the OS composites ABOVE the CEF browser, so a modal
// can only be visible while the preview is suspended (previewGate.svelte.ts). The
// dialog therefore cannot show the main preview -- but it can own a second overlay
// of its own, positioned inside its own bounds, which is the same arrangement
// upstream's OBSBasicFilters uses (its own OBSQTDisplay next to the filter list).
//
// One preview at a time: the Filters dialog is a singleton mount in the main
// window, so a second Open replaces the first rather than stacking.
//
// UAF discipline, inherited from InteractWindow: the source is addref'd for the
// preview's whole lifetime and released only AFTER the display is destroyed.
class FilterPreview {
public:
	// host: the top-level window the overlay is parented to (the main window -- the
	// Filters dialog is mounted only there; DetachedApp renders a single dock and no
	// dialogs). instance: the module HINSTANCE.
	FilterPreview(HWND host, HINSTANCE instance);
	~FilterPreview();

	FilterPreview(const FilterPreview &) = delete;
	FilterPreview &operator=(const FilterPreview &) = delete;

	// Bind the preview to `source` (the filtered parent, addref'd here; the caller
	// keeps its own ref). Replaces any previous binding. Returns false + `error`
	// when the source cannot be previewed -- audio-only, or a type that does not
	// render standalone -- which is the caller's cue to omit the preview pane
	// entirely rather than show a black box.
	//
	// No overlay exists yet after Open: the first SetRect creates it, so the UI
	// owns the geometry.
	bool Open(obs_source_t *source, std::string &error);

	// Position/size the overlay (device pixels, host-client coords). No-op when no
	// source is bound.
	void SetRect(int x, int y, int cx, int cy);

	// Hide the overlay, keeping the binding: the placeholder is clipped (scrolled out
	// of the dialog, or off the viewport), so the overlay would otherwise paint over
	// whatever is now in its place.
	void Hide();

	// Drop the overlay and the binding: display first, then the source ref. Safe to
	// call with nothing open. Called on dialog close, on the bound source being
	// removed, and at app teardown.
	void Close();

	// Whether an overlay with a live obs_display is currently bound. Used by the
	// smoke self-test.
	bool HasDisplayForTest() const;

	// The bound source's uuid, empty when nothing is bound. Lets the source-removed
	// handler confirm it is closing the right binding.
	const std::string &SourceUuid() const { return sourceUuid_; }

	// Close in response to the bound source's "remove" signal and tell the UI, so
	// the dialog collapses its preview pane instead of leaving an empty hole. UI
	// thread (the signal handler defers here).
	void OnSourceRemoved();

	// The addref-pinned source the draw callback reads. Defined in the .cpp so this
	// header stays free of libobs types; named here only so the .cpp's callback can
	// name it. Incomplete outside that TU.
	struct State;

private:
	HWND host_;
	HINSTANCE instance_;
	State *state_;
	std::unique_ptr<OverlaySurface> overlay_;
	obs_source_t *source_ = nullptr; // addref'd while bound
	std::string sourceUuid_;
	bool showRefHeld_ = false; // VideoGate showing hold on the bound source
};

// Process-wide accessor to the single live FilterPreview so the bridge methods
// (filterPreview.*) and the source-removed signal can reach it without threading a
// pointer through. Set by main.cpp after libobs is up and cleared before teardown;
// bridge, main and the deferred signal handler share the UI thread.
namespace FilterPreviewHost {
void SetInstance(FilterPreview *fp);
FilterPreview *Instance();
} // namespace FilterPreviewHost

#endif // OBS_MULTISTREAM_FRONTEND_FILTER_PREVIEW_HPP_
