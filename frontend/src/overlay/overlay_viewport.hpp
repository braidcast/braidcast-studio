#ifndef OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VIEWPORT_HPP_
#define OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VIEWPORT_HPP_

#include <cstdint>

struct obs_source;
typedef struct obs_source obs_source_t;
struct obs_scene_item;
typedef struct obs_scene_item obs_sceneitem_t;

// Viewport auto-follow for braidcast_overlay sources: the scene item IS the size.
//
// A browser source lays its page out at the width/height in its settings and the scene
// item then SCALES that rendered bitmap, so dragging the corner handles magnifies pixels
// instead of re-laying-out the page. The overlay widget templates scale with their source
// rectangle, which makes the viewport the thing that matters -- so the viewport has to
// follow the box the user actually drags. This module is that follow, and it is
// unconditional for braidcast_overlay. Generic browser_source is untouched.
//
// Everything here is UI-thread only. It reads item->bounds / item->crop / item->scale and
// obs_source_get_width/height, all of which the UI thread owns; it never reads box_scale
// or box_transform, which the graphics thread writes unguarded.
namespace Overlay {

// Below this the page is not written at all and the last good viewport is kept. A 1x1
// page can permanently wreck a widget's DOM (zero-size flex containers, media queries
// latched at the wrong breakpoint) and it does NOT recover when the box grows back,
// whereas a stretched last-good bitmap always does.
inline constexpr uint32_t kMinViewportPx = 8;

// The obs-browser width/height property range (obs-browser-plugin.cpp).
inline constexpr uint32_t kMaxViewportPx = 8192;

// Trailing coalesce window for the programmatic (bridge) commit paths, which can fire
// per keystroke in a numeric Transform field. The preview drag path needs none -- it
// already commits once, at drag end.
inline constexpr int64_t kCommitCoalesceMs = 200;

// Convert `item` to OBS_BOUNDS_STRETCH with its bounds set to the box it occupies right
// now, so its on-canvas size stops depending on the source size. Pixel-identical and
// idempotent: an item already in a bounds mode is left alone, and for a NONE-mode item
// OBS_BOUNDS_STRETCH reproduces the same box_scale, the same origin and the same flip.
// This is what makes the viewport write safe -- without it, growing the page would grow
// every item drawing that source. Returns true when the item was actually converted,
// which is scene-collection state the caller has to persist.
bool PinItemToBounds(obs_sceneitem_t *item);

// Pin every scene item bound to `src`, across the main canvas and every named canvas,
// recursing into groups. Runs as one pass BEFORE any viewport write so no sibling's box
// can jump when the source size changes. True when at least one item was converted.
//
// Also the step that makes an overlay's geometry safe to record in an undo entry: an
// OBS_BOUNDS_NONE box is a function of the page size, which is the very thing this
// feature moves, so a state captured before the pin describes a box that no longer
// reproduces once the page has been resized.
bool PinAllItems(obs_source_t *src);

// The viewport `src` needs to cover every box it is drawn in: the per-axis maximum, in
// CANVAS pixels, of (|bounds| x ancestor scale + crop) over every item bound to it. The
// crop term is unscaled because libobs subtracts the crop from the page before scaling
// what is left into the box; the bounds term is scaled because a group child's box is in
// the group's coordinate space, not the canvas's.
//
// Reads item->bounds, so it is only meaningful once the items are pinned -- call
// PinAllItems (or CommitForSource, which does both) first. Returns false, leaving the
// outputs untouched, when there is no item, when the numbers are not finite, or when the
// result is below kMinViewportPx on either axis.
//
// KNOWN GAP -- nested scenes. Groups are exact: a group item has exactly one parent, so
// its children carry a single well-defined ancestor scale. A nested SCENE does not -- the
// same scene can be an item of several scenes at several different scales, so there is no
// one factor to apply. An overlay inside a nested scene is therefore measured in that
// scene's own coordinates, and renders upscaled wherever its parent item is scaled up.
bool ComputeViewport(obs_source_t *src, uint32_t &outWidth, uint32_t &outHeight);

// Write `width`x`height` into `src`'s settings as its page size, clamped to
// [1, kMaxViewportPx]. Undo-suppressed.
//
// REFUSES unless the overlay's URL would resolve right now (Overlay::
// IsOverlayUrlResolvable): braidcast_overlay re-resolves its URL from the overlay proc on
// every update, and an empty resolve makes the URL fall back to about:blank, compare
// unequal, and send BrowserSource::Update down the destroy-and-recreate path -- the
// overlay would go blank on air. A width/height-only change against a resolvable overlay
// is reload-free: obs-browser assigns the members and posts an async resize, so the DOM
// and the widget's JS state survive.
//
// Returns true only when the stored page size actually changed, so a repeat commit costs
// nothing -- no update, no save, no event.
bool WriteViewport(obs_source_t *src, uint32_t width, uint32_t height);

// Pin every item bound to `src`, then size its page to cover them. The whole feature in
// one call. Re-entrant-safe and a fixpoint: the target is a pure function of (bounds,
// crop), neither of which this touches, so a second pass computes the same numbers and
// obs-browser's own unchanged-size early-return makes it free.
void CommitForSource(obs_source_t *src);

// CommitForSource for the programmatic bridge paths, where a single user gesture (a
// numeric Transform field) produces a burst of transforms.
//
// The pin and the page size are applied IMMEDIATELY -- the caller records its undo
// AFTER-state as soon as this returns, so that state has to already carry the new
// viewport. Only the expensive tail coalesces: the scene-collection disk write and the
// sceneItems-changed event are deferred by kCommitCoalesceMs, keyed by `src`'s uuid, so a
// burst pays for them once instead of once per keystroke.
void CommitForSourceDebounced(obs_source_t *src);

// CommitForSource for every overlay source in the process. For canvas resets: bounds in
// relative coordinates scale with the canvas height, so the viewport has to follow.
void CommitAll();

// Drop every pending coalesced publish without running it. Called during teardown, ahead
// of the authoritative scene-collection save and the scene unbind that follows it: a
// publish that ran after that unbind would write a gutted collection over the user's real
// one. Nothing is lost -- the pin and the page size were already applied synchronously,
// and the teardown save persists them.
void CancelPendingCommits();

} // namespace Overlay

#endif // OBS_MULTISTREAM_FRONTEND_OVERLAY_OVERLAY_VIEWPORT_HPP_
