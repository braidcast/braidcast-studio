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

// The ingest protocol the profile's service targets ("HLS", "RTMPS", "RTMP", ...), as
// rtmp-services records it in the profile settings; empty when the profile carries none.
// A provider that CREATES an ingest resource server-side has to declare the same protocol
// the output will actually push, so this is read at go-live rather than assumed. Marshals
// to TID_UI and blocks, exactly like the writeback above.
std::string ReadProfileIngestProtocol(const std::string &profileUuid);

#endif // OBS_MULTISTREAM_FRONTEND_INGEST_WRITEBACK_HPP_
