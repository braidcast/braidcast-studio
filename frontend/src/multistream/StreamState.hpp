#pragma once

#include "MultistreamEngine.hpp"

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

// The broadcast-state projection: whether anything is going out, when it started, and where
// to. ONE object, fanned to both consumers by the bridge's EmitStreamingChanged -- the
// `streaming.changed` bridge event and the overlay server's named `stream` SSE channel -- so
// an uptime rendered on stream can never disagree with the app's.
//
// `startedAt` is WALL-CLOCK epoch milliseconds, derived HERE from the outputs' uptimes. The
// engine measures uptime off os_gettime_ns, which is monotonic-since-boot: it names no
// instant outside this process, and the consumer is an uptime widget in a separate CEF
// process that can only compute now - startedAt. Sending the monotonic reading would be the
// same trap channel_stats_poller documents for audienceUpdatedNs. `nowMs` is the wall clock
// read alongside `outputs`, so the conversion leaves nothing on the wire that could be diffed
// against the wrong clock.
//
// It is null -- never 0 -- until an output has signalled start (OutputStats::started). A
// Connecting output has no start, and a zero epoch renders as an uptime of decades. An output
// that has started has one even while its uptime still reads 0: the go-live snapshot is taken
// within a millisecond of the start, and the overlay's broadcast window opens at the first
// frame that carries a start (Overlay::BroadcastTally), so a null there leaves it shut for the
// whole broadcast.
//
// `anyLive` is MultistreamEngine::AnyLive(), kept apart from `outputs`: an output live under a
// binding that was disabled mid-broadcast counts as live but enumerates nowhere, and "live to
// nothing we can name" must not read as "not live".
nlohmann::json StreamStateJson(const std::vector<MultistreamEngine::OutputStats> &outputs, bool anyLive, int64_t nowMs);
