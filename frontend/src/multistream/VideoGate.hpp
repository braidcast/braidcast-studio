#pragma once

#include <obs.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

// Stops the Default ("Main") canvas's scene tree from capturing video while
// nothing consumes that composite -- no enabled output binding, no open preview,
// projector or virtual camera -- and restores it as soon as a consumer appears.
//
// Suppression is obs_source_set_video_gated: a per-source flag this module owns
// outright, never a borrowed show_refs decrement. libobs has no per-holder
// showing refcount, so a decrement taken on another holder's behalf cannot be
// attributed back when that holder releases. Owning the flag is also what makes
// the reconcile total: the gated set is recomputed from scratch on every call,
// so a missed event is a stale flag the next sweep corrects rather than a
// refcount that can never recover.
//
// Audio is untouched. Mixing is gated by activate_refs (libobs/obs-audio.c),
// which nothing here writes, so an idle Main keeps playing.
//
// Every entry point runs on the CEF UI thread (bridge dispatch, window
// messages, the stats sampler), except the teardown path, which runs on the main
// thread after the CEF loop has returned. There is no concurrent caller, so the
// state below is unsynchronized.
namespace VideoGate {

// Visits one root source and the canvas it composites for; the source is
// borrowed for the duration of the call.
using RootVisitor = std::function<void(const std::string &canvasUuid, obs_source_t *root)>;

// uuid -> strong ref. Held only for the tick that walked it, so the graph cannot
// change under a consumer; anything kept across ticks stores uuids or weak refs.
using SourceSet = std::map<std::string, OBSSource>;

enum class RootKind {
	Main,        // channel 0, the Default canvas's composite
	Canvas,      // channel 0 of an active non-Default canvas
	ShowingRoot, // a frontend showing holder (thumbnail, projector, Multiview cell)
};

struct Root {
	RootKind kind;
	std::string canvasUuid; // empty for a ShowingRoot, which composites for no canvas
	SourceSet sources;      // the root and its active tree
};

// Every root that renders right now, each with its active tree. One walk serves
// every consumer of a sweep, so the gate and the capture-rate sampler cannot
// disagree about what reaches what.
std::vector<Root> WalkRoots();

// Whether gating runs at all: enabled, and the Default canvas's consumer
// predicate registered. While it is not, ReconcileWith needs no walk.
bool Armed();

// Recompute the gated set from a walk. Idempotent, and cheap enough to call on
// every consumer change as well as from the periodic sweep.
void ReconcileWith(const std::vector<Root> &roots);

// ReconcileWith(WalkRoots()), skipping the walk when the gate is off.
void Reconcile();

// "Does the Default canvas still have a consumer": CanvasRuntime::DefaultIsActive.
// Injected rather than re-derived so the predicate has exactly one definition.
void SetMainActivePredicate(std::function<bool()> fn);

// Visits channel 0 of every ACTIVE non-Default canvas. Those trees composite
// independently of Main, so anything they reach must stay ungated.
void SetCanvasRootEnumerator(std::function<void(const RootVisitor &)> fn);

// The Default canvas's uuid, which the Main root reports. Injected for the same
// reason as the predicate: the canvas store owns that answer.
void SetMainCanvasUuid(std::function<std::string()> fn);

// obs_source_inc_showing / obs_source_dec_showing plus registration of the
// source as a gate root. The frontend's explicit showing holders go through
// these instead of calling libobs directly, so the set of roots cannot drift
// from the set of holds.
void IncShowing(obs_source_t *source);
void DecShowing(obs_source_t *source);

// Ungate everything and drop all injected state. Called from CanvasRuntime's
// teardown, while the sources are still alive.
void Shutdown();

} // namespace VideoGate
