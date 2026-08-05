#ifndef OBS_MULTISTREAM_FRONTEND_INGEST_WRITEBACK_HPP_
#define OBS_MULTISTREAM_FRONTEND_INGEST_WRITEBACK_HPP_

#include <string>

// Write the RTMP ingest server + stream key into the stream profile identified by
// `profileUuid`. The profile store is UI-thread-owned, so this marshals the write
// to TID_UI and BLOCKS until it completes (callers rely on the key being present
// before they trigger streaming.start). Safe from a worker thread; runs inline if
// already on TID_UI. `server` may be empty (key-only). Returns true iff the profile
// was found and updated.
bool WriteIngestToProfile(const std::string &profileUuid, const std::string &server, const std::string &key);

// The ingest protocol the stream profile `profileUuid` currently targets ("RTMPS", "RTMP",
// "HLS", ...), or "" when the profile is gone or names none. Marshals to TID_UI and BLOCKS
// for the same reason the write above does -- the profile store is UI-thread-owned, and a
// provider asking this question runs on a worker.
//
// Exists because a platform resource can be protocol-SPECIFIC: YouTube's liveStream carries
// a cdn.ingestionType fixed at creation, so a provider has to know which one this profile
// needs before it decides whether a remembered stream is still usable.
std::string ReadProfileIngestProtocol(const std::string &profileUuid);

#endif // OBS_MULTISTREAM_FRONTEND_INGEST_WRITEBACK_HPP_
