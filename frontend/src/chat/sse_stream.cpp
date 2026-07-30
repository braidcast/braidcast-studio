#include "sse_stream.hpp"

#include <utility>

namespace Chat {

namespace {

// A field line splits at its FIRST colon, and one leading space of the value is dropped --
// the `field: value` convention, where that space is the separator rather than content. A
// line carrying no colon is a field name with an empty value.
void SplitField(std::string_view line, std::string_view &field, std::string_view &value)
{
	const size_t colon = line.find(':');
	if (colon == std::string_view::npos) {
		field = line;
		value = std::string_view();
		return;
	}
	field = line.substr(0, colon);
	value = line.substr(colon + 1);
	if (!value.empty() && value.front() == ' ') {
		value.remove_prefix(1);
	}
}

// A reconnection advisory is digits only; anything else is ignored rather than guessed at.
bool ParseRetryMs(std::string_view value, long &out)
{
	if (value.empty()) {
		return false;
	}
	long ms = 0;
	for (const char c : value) {
		if (c < '0' || c > '9') {
			return false;
		}
		ms = ms * 10 + (c - '0');
	}
	out = ms;
	return true;
}

} // namespace

bool SseStream::Push(std::string_view chunk, std::vector<SseEvent> &out)
{
	for (const char c : chunk) {
		if (c != '\n') {
			line_.push_back(c);
			if (line_.size() + data_.size() > kMaxBytes) {
				return false;
			}
			continue;
		}
		// CRLF and bare LF both end a line. A lone CR also does in the spec, but it
		// cannot be recognized without holding the byte back to see whether an LF
		// follows -- and no server this reads emits one -- so it is trimmed here
		// instead.
		std::string_view line(line_);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		HandleLine(line, out);
		line_.clear();
	}
	return true;
}

void SseStream::HandleLine(std::string_view line, std::vector<SseEvent> &out)
{
	if (line.empty()) {
		// The blank line is the event boundary. An event with no data is not dispatched,
		// so the stream's leading blank lines and a run of keepalives yield nothing; the
		// event type resets with the data while `id`/`retry` persist as connection state.
		if (!data_.empty()) {
			SseEvent ev;
			ev.name = name_;
			ev.id = id_;
			ev.retryMs = retryMs_;
			ev.data = std::move(data_);
			// Each data line appended a '\n'; the last one is separator, not content,
			// which is what joins several `data:` lines into one payload.
			ev.data.pop_back();
			out.push_back(std::move(ev));
		}
		data_.clear();
		name_.clear();
		return;
	}
	if (line.front() == ':') {
		keepalive_ = true;
		return;
	}
	std::string_view field;
	std::string_view value;
	SplitField(line, field, value);
	if (field == "data") {
		data_.append(value);
		data_.push_back('\n');
	} else if (field == "event") {
		name_.assign(value);
	} else if (field == "id") {
		id_.assign(value);
	} else if (field == "retry") {
		ParseRetryMs(value, retryMs_);
	}
}

} // namespace Chat
