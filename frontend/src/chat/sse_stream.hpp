#ifndef OBS_MULTISTREAM_FRONTEND_CHAT_SSE_STREAM_HPP_
#define OBS_MULTISTREAM_FRONTEND_CHAT_SSE_STREAM_HPP_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// An incremental parser for the text/event-stream wire format (server-sent events), fed
// from an HTTP streaming read's onChunk callback. The client half of SSE: the overlay
// module is an SSE server and shares no code with this.
//
// Framing is byte-oriented and buffered across chunk boundaries, so an event that splits
// mid-line reassembles before it is parsed -- including mid-UTF8, since no continuation
// byte can carry the '\n' this splits on.
namespace Chat {

// One dispatched event. `name` is empty for a default-typed event (`data:` lines with no
// `event:` field, which is what Facebook's live-comment stream sends). `id` and `retryMs`
// are the last values the stream advised rather than per-event fields, matching the spec,
// which treats both as connection state; `retryMs` is -1 until a server advises one.
struct SseEvent {
	std::string name;
	std::string data;
	std::string id;
	long retryMs = -1;
};

class SseStream {
public:
	// Feed one wire chunk; append every event it completed to `out`. False once the
	// buffer ceiling below is crossed: the stream is not framing and the caller must drop
	// the connection rather than keep feeding it.
	bool Push(std::string_view chunk, std::vector<SseEvent> &out);

	// Whether a comment line (": ping" and friends) arrived since the last call, clearing
	// the flag. A keepalive carries no data but proves the connection is alive, which is
	// the only thing separating a quiet chat from a dead socket.
	bool TakeKeepalive()
	{
		const bool seen = keepalive_;
		keepalive_ = false;
		return seen;
	}

private:
	// The ceiling on unparsed bytes -- the line still being assembled plus the data of the
	// event being accumulated. A server that opens the connection and then never sends a
	// boundary must not be able to grow this without limit; 1 MiB is orders of magnitude
	// above any real event and still small enough to discard on sight.
	static constexpr size_t kMaxBytes = 1024 * 1024;

	void HandleLine(std::string_view line, std::vector<SseEvent> &out);

	std::string line_; // the line still being assembled
	std::string data_; // `data:` values of the event being accumulated, joined with '\n'
	std::string name_;
	std::string id_;
	long retryMs_ = -1;
	bool keepalive_ = false;
};

} // namespace Chat

#endif // OBS_MULTISTREAM_FRONTEND_CHAT_SSE_STREAM_HPP_
