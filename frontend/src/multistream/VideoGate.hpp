#pragma once

#include <obs.h>

#include <functional>

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

// Visits one root source; the source is borrowed for the duration of the call.
using RootVisitor = std::function<void(obs_source_t *)>;

// Recompute the gated set from the live source graph. Idempotent, and cheap
// enough to call on every consumer change as well as from the periodic sweep.
void Reconcile();

// "Does the Default canvas still have a consumer": CanvasRuntime::DefaultIsActive.
// Injected rather than re-derived so the predicate has exactly one definition.
void SetMainActivePredicate(std::function<bool()> fn);

// Visits channel 0 of every ACTIVE non-Default canvas. Those trees composite
// independently of Main, so anything they reach must stay ungated.
void SetCanvasRootEnumerator(std::function<void(const RootVisitor &)> fn);

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
