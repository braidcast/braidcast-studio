#include "overlay_server.hpp"
#include "util/fnv1a.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h> // Sleep (winsock2.h already set _WINSOCKAPI_, so no winsock v1)

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../log.hpp"
#include "util/file_util.hpp"      // FileUtil::ReadBinaryFile
#include "util/http_status.hpp"    // Http::ReasonFor
#include "util/string_util.hpp"    // StringUtil::ToLower
#include "util/web_bundle.hpp"     // WebBundle::Root, WebBundle::ContentTypeForPath
#include "../events/event_hub.hpp" // Events::Store() -- the persisted event history
#include "overlay_store.hpp"       // Overlay::Store(), Widget, WidgetUrl
#include "overlay_template.hpp"    // Overlay::AcceptsReplay

#pragma comment(lib, "ws2_32.lib")

namespace Overlay {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxHeaderBytes = 16 * 1024;      // 16 KB header block (mirrors mcp)
constexpr size_t kMaxAssetBytes = 8 * 1024 * 1024; // asset response cap (spec)
constexpr int kPreferredPort = 43000;              // persisted default; tried first for stable URLs
// Scattered scan bands (base, +kBandSize each): a single OS-reserved block (Hyper-V/
// WSL/Docker reserve large contiguous ranges) can't kill the feature. 5 x 50 = 250.
constexpr int kBandSize = 50;
constexpr std::array<int, 5> kScanBands = {43000, 47000, 51000, 55000, 59000};
constexpr DWORD kSseRecvTimeoutMs = 15000;     // keepalive cadence
constexpr DWORD kSseSendTimeoutMs = 3000;      // I3: bound on one SSE send so a stuck reader can't park a sender
constexpr DWORD kHeaderRecvTimeoutMs = 10000;  // I1: backstop so a silent client can't park a thread
constexpr DWORD kResponseSendTimeoutMs = 3000; // bounded plain-HTTP send so a stuck reader can't park a thread
constexpr size_t kMaxSseConnections = 64;      // ceiling on concurrent live SSE streams; excess rejected 503
constexpr size_t kMaxBackfillEvents = 200;     // ceiling on the connect-time event replay

// Freshness for an uploaded widget asset. A day, because the URL AssembleDocument mints
// carries the widget revision, and OverlayStore::AddAsset -- the only writer of these bytes
// -- bumps that revision under the same lock as the write. So the cache key cannot outlive
// the bytes it names: a re-upload at the same filename still moves the URL. Freshness only
// has to outlast a broadcast, and a day is well past that. Not `immutable`: the ETag
// revalidation below is the backstop if a future writer ever appears that does not bump,
// and `immutable` would tell the client not to check even on a reload.
constexpr int kAssetMaxAgeSeconds = 86400;

// The fields.json `type` whose value is an audio file the page plays. The editor's half of
// the same registry is `frontend/web/src/lib/overlays/fieldTypes.ts`; nothing links them,
// so a rename there must be made here too. Listing these URLs in the bootstrap is what lets
// the runtime decode a widget's sounds once at load rather than building, fetching and
// decoding a fresh media element per alert -- and it covers a FORKED widget for free,
// because a fork carries its own schema through the same Resolve().
constexpr const char *kSoundFieldType = "sound-upload";

// Read one file under an absolute root, rejecting ".." (copy of scheme.cpp guard).
bool ReadFileGuarded(const std::string &root, const std::string &rel, std::string &out, std::string &ctype)
{
	if (rel.empty() || rel.find("..") != std::string::npos) {
		return false;
	}
	std::string full = root + "/" + rel;
	for (char &c : full) {
		if (c == '/') {
			c = '\\';
		}
	}
	if (!FileUtil::ReadBinaryFile(full, out)) {
		return false;
	}
	ctype = WebBundle::ContentTypeForPath(rel);
	return true;
}

// Split "path?query" and return the "t" query value (may be ""). Tokens are hex, so
// no URL-decoding is needed.
std::string QueryToken(const std::string &pathWithQuery, std::string &pathOut)
{
	const size_t q = pathWithQuery.find('?');
	pathOut = q == std::string::npos ? pathWithQuery : pathWithQuery.substr(0, q);
	if (q == std::string::npos) {
		return std::string();
	}
	const std::string query = pathWithQuery.substr(q + 1);
	size_t pos = 0;
	while (pos < query.size()) {
		const size_t amp = query.find('&', pos);
		const std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		const size_t eq = pair.find('=');
		if (eq != std::string::npos && pair.substr(0, eq) == "t") {
			return pair.substr(eq + 1);
		}
		if (amp == std::string::npos) {
			break;
		}
		pos = amp + 1;
	}
	return std::string();
}

// One request header's value, or "" when absent. `headerBlock` is the CRLF-joined block
// HandleConnection already split off, request line included -- a name split out of the
// request line always contains a space, so it can never match a header name. `lowerName`
// must be lowercase; header names are case-insensitive on the wire and Chromium sends them
// lowercased over HTTP/1.1 only by convention.
std::string HeaderValue(const std::string &headerBlock, const char *lowerName)
{
	size_t pos = 0;
	while (pos <= headerBlock.size()) {
		const size_t eol = headerBlock.find("\r\n", pos);
		const std::string line =
			headerBlock.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		const size_t colon = line.find(':');
		if (colon != std::string::npos) {
			if (StringUtil::ToLower(line.substr(0, colon)) == lowerName) {
				std::string value = line.substr(colon + 1);
				const size_t first = value.find_first_not_of(" \t");
				const size_t last = value.find_last_not_of(" \t");
				return first == std::string::npos ? std::string()
								  : value.substr(first, last - first + 1);
			}
		}
		if (eol == std::string::npos) {
			break;
		}
		pos = eol + 2;
	}
	return std::string();
}

// A strong ETag for a body we have already read: FNV-1a 64 over the bytes, quoted.
//
// Content-derived rather than mtime-derived deliberately. An asset is replaced in place at
// the same path by OverlayStore::AddAsset, and a filesystem timestamp has a granularity a
// fast replace can land inside -- which would hand a client a validator that says "still
// the same file" about different bytes. Hashing what we are about to serve cannot say that.
std::string StrongETag(const std::string &body)
{
	const uint64_t h = Fnv1a64(body);
	static const char *kHex = "0123456789abcdef";
	std::string out = "\"";
	for (int shift = 60; shift >= 0; shift -= 4) {
		out += kHex[(h >> shift) & 0xf];
	}
	out += '"';
	return out;
}

// Whether an If-None-Match value selects `etag`. Accepts "*", a single tag, and the
// comma-separated list form, and tolerates the weak "W/" prefix a proxy may add -- our own
// tag is strong, and for a GET the weak comparison is the one RFC 9110 specifies anyway.
bool ETagMatches(const std::string &ifNoneMatch, const std::string &etag)
{
	size_t pos = 0;
	while (pos < ifNoneMatch.size()) {
		const size_t comma = ifNoneMatch.find(',', pos);
		std::string tag = ifNoneMatch.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		const size_t first = tag.find_first_not_of(" \t");
		const size_t last = tag.find_last_not_of(" \t");
		if (first != std::string::npos) {
			tag = tag.substr(first, last - first + 1);
			if (tag.rfind("W/", 0) == 0) {
				tag = tag.substr(2);
			}
			if (tag == "*" || tag == etag) {
				return true;
			}
		}
		if (comma == std::string::npos) {
			break;
		}
		pos = comma + 1;
	}
	return false;
}

// Run a closure when the scope ends, however it ends. Local and minimal because this tree
// has no scope-guard helper and exactly one place needs one: a bookkeeping counter that must
// come back down on an exception as well as on the normal path. The destructor is noexcept,
// so a closure that throws (locking, allocating) terminates instead of propagating -- the
// trade is deliberate: a leaked counter strands every parked SSE socket for the process
// lifetime, and nothing here can recover from a failed lock anyway.
template<typename F> class ScopeExit {
public:
	explicit ScopeExit(F f) : f_(std::move(f)) {}
	~ScopeExit() { f_(); }
	ScopeExit(const ScopeExit &) = delete;
	ScopeExit &operator=(const ScopeExit &) = delete;

private:
	F f_;
};

// Blocking best-effort write of an entire buffer; false on any send failure.
bool SendAll(SOCKET sock, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		const int chunk = (int)std::min<size_t>(len - sent, 64 * 1024);
		const int n = send(sock, data + sent, chunk, 0);
		if (n <= 0) {
			return false;
		}
		sent += (size_t)n;
	}
	return true;
}

// Write a complete HTTP/1.1 response with Connection: close (mirrors mcp WriteResponse).
//
// `extraHeaders` is appended verbatim and must be whole CRLF-terminated header lines (or
// empty). It exists so the asset route can attach its cache policy without every other
// caller growing a header argument it has no answer for -- the default is what every route
// sent before there was one.
//
// `suppressBody` writes the head alone while still advertising `body`'s length, which is
// what a 304 needs: RFC 9110 SS15.4.5 forbids a message body on a 304, and SS8.6 forbids a
// Content-Length that disagrees with the one a 200 for the same resource would have carried.
// Passing the representation and dropping only the write satisfies both, and it is the one
// form of decoupling needed -- every other caller sends its body and is unchanged on the wire.
void WriteResponse(SOCKET sock, int status, const std::string &ctype, const std::string &body,
		   const std::string &extraHeaders = std::string(), bool suppressBody = false)
{
	std::string head = "HTTP/1.1 " + std::to_string(status) + " " + Http::ReasonFor(status) + "\r\n";
	head += "Content-Type: " + ctype + "\r\n";
	head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	head += "Connection: close\r\n";
	head += "Access-Control-Allow-Origin: *\r\n";
	head += extraHeaders;
	head += "\r\n";
	const std::string out = suppressBody ? head : head + body;
	SendAll(sock, out.data(), out.size());
}

// Build the served widget document (spec shell): user CSS in <style>, user HTML in
// <body>, then window.__OVERLAY__, the runtime, then the user JS.
std::string AssembleDocument(const Widget &w, int port)
{
	// One resolution for the whole document: a stock widget's schema and its markup both
	// come off disk, and taking them together is what keeps a template shipped with a new
	// field from reaching this page as markup from one read and a schema from another.
	const ResolvedWidget resolved = Resolve(w);
	// Every key that schema declares, at the widget's override or the schema's default.
	json fieldData = MergeSettings(resolved.schema, w.settings);
	// The keys the rewrite below actually turned into a served URL. Collected rather than
	// re-derived from the value afterwards, so "the runtime may fetch this" means exactly
	// "this server serves it" by construction.
	std::vector<std::string> servedKeys;
	for (auto it = fieldData.begin(); it != fieldData.end(); ++it) {
		// An uploaded asset field stores the portable, token-less "assets/<file>" as its
		// persisted value. Rewrite ONLY the injected copy to the absolute tokenized URL the
		// server actually serves (/w/<id>/assets/<file>?t=<token>&r=<rev>); a bare
		// "assets/<file>" would resolve against /w/ (no <base>) and 404, and lacks the
		// required token. Match the prefix so it works regardless of the field's declared
		// type. The stored setting is left untouched so it survives token/port changes.
		//
		// `r` is the widget revision, and it is what makes this URL safe to cache for a
		// long time. AddAsset replaces an upload IN PLACE at the same filename, so without
		// it a re-upload under the same name keeps the same URL and a browser source that
		// reloaded would re-read its own still-fresh cache entry and play the OLD bytes.
		// AddAsset is the only writer of those bytes and bumps the revision itself, under
		// the same lock as the write, so that cannot happen: changing the bytes changes
		// this URL. What this does NOT do is reload a source already on a scene --
		// overlays.uploadAsset sweeps nothing -- so that source keeps its old document and
		// its old sound until something else reloads it. Every document assembled from the
		// write onwards names the new bytes, which is the whole claim here.
		if (!it->is_string()) {
			continue;
		}
		const std::string s = it->get<std::string>();
		if (s.rfind("assets/", 0) == 0) {
			*it = "/w/" + w.id + "/" + s + "?t=" + w.token + "&r=" + std::to_string(w.rev);
			servedKeys.push_back(it.key());
		}
	}
	// Every sound this widget could play, as the tokenized URLs the rewrite above just
	// produced, so the runtime can decode them at load. Read off the schema rather than
	// guessed from the value, because only the schema knows a string is audio.
	//
	// Restricted to keys the rewrite handled. A widget with no sound configured has an empty
	// value and is simply not listed -- but so is a fork whose fields.json defaults a sound
	// field to an absolute http(s) URL. A media element plays a cross-origin sound without
	// CORS; fetch does not, so preloading one would spend a request to earn a CORS failure
	// and a log line on every page load, then fall back to the element and play it correctly
	// anyway. Leaving it off the list is what keeps that path quiet and working.
	json sounds = json::array();
	if (resolved.schema.is_array()) {
		for (const json &f : resolved.schema) {
			if (!f.is_object() || f.value("type", std::string()) != kSoundFieldType) {
				continue;
			}
			const std::string key = f.value("key", std::string());
			if (std::find(servedKeys.begin(), servedKeys.end(), key) == servedKeys.end()) {
				continue;
			}
			const auto valueIt = fieldData.find(key);
			if (valueIt == fieldData.end() || !valueIt->is_string()) {
				continue;
			}
			// Two fields can point at one upload; decoding it twice would just evict
			// something else from the runtime's cache.
			if (std::find(sounds.begin(), sounds.end(), *valueIt) == sounds.end()) {
				sounds.push_back(*valueIt);
			}
		}
	}
	const json overlay =
		json{{"id", w.id}, {"token", w.token}, {"port", port}, {"fields", fieldData}, {"sounds", sounds}};
	std::string doc = "<!doctype html><html><head><meta charset=\"utf-8\">\n<style>\n";
	doc += resolved.css;
	doc += "\n</style></head><body>\n";
	doc += resolved.html;
	doc += "\n<script>window.__OVERLAY__=" + overlay.dump() + ";</script>\n";
	doc += "<script src=\"/runtime.js?t=" + w.token + "\"></script>\n";
	doc += "<script>\n" + resolved.js + "\n</script>\n</body></html>";
	return doc;
}

// One SSE frame on the wire: unnamed (the default `message` channel every alert box
// consumes) and named. The only two places that framing is spelled out, so a sender added
// later cannot hand-roll a third variant that drifts from them.
std::string DataFrame(const json &body)
{
	return "data: " + body.dump() + "\n\n";
}

std::string NamedFrame(const char *eventName, const json &body)
{
	return "event: " + std::string(eventName) + "\ndata: " + body.dump() + "\n\n";
}

// The current broadcast's events, oldest-first, as one `backfill` frame. A widget that
// accumulates a running total from the event stream (a goal bar) otherwise restarts from
// its configured seed every time its source is recreated -- a reload, a scene-collection
// switch -- and the donations it had already counted leave the bar.
//
// Bounded twice, because this is the FIRST thing written to a socket that has not proven
// it can read yet: by the broadcast's own start, since a previous stream's events are not
// this stream's progress, and by kMaxBackfillEvents, so a long broadcast cannot hand a
// connecting client an unbounded write.
//
// An empty array is a real answer -- "this broadcast has produced nothing yet" -- and is
// still sent, so a consumer can tell it apart from getting no backfill at all.
std::string BuildBackfillFrame(int64_t sinceMs)
{
	const std::vector<Events::NormalizedEvent> history = Events::Store().List(); // newest-first
	std::vector<const Events::NormalizedEvent *> picked;
	for (const Events::NormalizedEvent &ev : history) {
		if (ev.ts < sinceMs) {
			continue;
		}
		if (picked.size() >= kMaxBackfillEvents) {
			break;
		}
		picked.push_back(&ev);
	}
	json arr = json::array();
	for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
		arr.push_back((*it)->ToJson());
	}
	return NamedFrame("backfill", json{{"events", std::move(arr)}});
}

// The BroadcastFrame widgetFilter for events.replay: a widget whose id no longer resolves
// (deleted mid-broadcast) is excluded the same as one whose type does not accept a replay.
//
// TypeOf rather than Get: this runs once per connected widget on the broadcast path, and
// Get would copy the whole Widget -- a fork's html/css/js included -- under the store's
// mutex, which its mutators hold across a disk Save(). BroadcastFrame calls this with
// sseMutex_ released, so a save merely delays the filter rather than stalling every SSE
// channel behind it.
bool WidgetAcceptsReplay(const std::string &widgetId)
{
	const std::optional<std::string> type = Store().TypeOf(widgetId);
	return type.has_value() && AcceptsReplay(*type);
}

} // namespace

// ---- Broadcast --------------------------------------------------------------

// Snapshot the live socket handles under sseMutex_, then send OUTSIDE the lock so a
// single slow/dead client (bounded by SO_SNDTIMEO) can't hold the mutex and stall
// delivery to every other client (head-of-line blocking). Dead sockets are pruned on a
// re-lock. A failed send drops the socket from the registry and shutdown()s it (NOT
// close): the owning RunSse is the sole closer of its fd, so shutdown unblocks that
// thread's recv without freeing the fd -- the OS can't recycle the fd value onto a NEW
// connection and have this thread later close the wrong socket (the fd-reuse hazard).
// broadcastDepth_ (incremented while unlocked) makes RunSse defer its own closesocket()
// so an in-flight send here can never land on a recycled fd.
//
// Returns how many WIDGETS took the frame, not how many sockets: one widget open in both
// the editor preview and a Browser Source is two sockets and one widget, and "delivered to
// 2" would be read as two overlays on stream. A widget counts once as long as any of its
// sockets took the frame. A targeted send that answers 0 is the only way a caller can tell
// "nothing is subscribed to that widget" apart from a delivery, since filtering by widget
// id leaves no other trace.
//
// widgetFilter is applied with sseMutex_ RELEASED. It reads the widget store, whose own
// mutex OverlayStore::Create/Update/Delete hold across a disk Save(); asking it under
// sseMutex_ would put every SSE channel -- chat, viewer counts, live events, keepalives,
// teardown -- behind an overlay save for the length of that write. The snapshot is what the
// lock is for, and nothing else here needs it.
size_t OverlayServer::BroadcastFrame(const std::string &frame, const std::string *onlyWidgetId,
				     bool (*widgetFilter)(const std::string &))
{
	// Grouped by widget rather than flattened, so the filter is asked once per widget
	// instead of once per socket, and a widget's sockets can answer as one delivery.
	std::vector<std::pair<std::string, std::vector<uintptr_t>>> targets;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		for (auto &[wid, socks] : sockets_) {
			if (onlyWidgetId && wid != *onlyWidgetId) {
				continue;
			}
			targets.emplace_back(wid, std::vector<uintptr_t>(socks.begin(), socks.end()));
		}
		++broadcastDepth_;
	}

	// Everything below runs with broadcastDepth_ raised, so the epilogue is a scope guard
	// rather than straight-line code. widgetFilter reads the widget store and allocates
	// (Store().TypeOf returns a std::string), and an exception escaping this region would
	// leave the depth raised permanently: it would never reach 0 again, deferredCloseSse_
	// would never drain, and every SSE fd RunSse parks from then on would leak for the life
	// of the process. The counter is the one thing here that cannot be allowed to leak.
	std::vector<std::pair<std::string, uintptr_t>> dead;
	const ScopeExit epilogue([&] {
		std::vector<uintptr_t> toClose;
		{
			std::lock_guard<std::mutex> lock(sseMutex_);
			for (auto &[wid, s] : dead) {
				// Re-check membership by handle: RunSse may have dropped (and
				// deferred the close of) this socket while we were unlocked. Only
				// shutdown() one still registered, so we never touch an fd another
				// path is already tearing down.
				auto it = sockets_.find(wid);
				if (it == sockets_.end() || !it->second.count(s)) {
					continue;
				}
				it->second.erase(s);
				if (it->second.empty()) {
					sockets_.erase(it);
				}
				shutdown((SOCKET)s, SD_BOTH);
			}
			if (--broadcastDepth_ == 0 && !deferredCloseSse_.empty()) {
				toClose.assign(deferredCloseSse_.begin(), deferredCloseSse_.end());
				deferredCloseSse_.clear();
			}
		}
		// Now that no broadcast is mid-send, it is safe to close the fds RunSse parked.
		for (uintptr_t s : toClose) {
			CloseClient(s);
		}
	});

	size_t delivered = 0;
	for (const auto &[wid, socks] : targets) {
		if (widgetFilter && !widgetFilter(wid)) {
			continue;
		}
		bool took = false;
		for (uintptr_t s : socks) {
			if (SendAll((SOCKET)s, frame.data(), frame.size())) {
				took = true;
			} else {
				dead.emplace_back(wid, s);
			}
		}
		// The sends that failed are counted out: a socket the frame could not be
		// written to did not receive it, and it is on its way out of the registry.
		if (took) {
			++delivered;
		}
	}
	return delivered;
}

size_t OverlayServer::LiveSseCount() const
{
	size_t n = 0;
	for (const auto &entry : sockets_) {
		n += entry.second.size();
	}
	return n;
}

size_t OverlayServer::Broadcast(const Events::NormalizedEvent &ev, bool replay)
{
	json body = ev.ToJson();
	if (replay) {
		// Set here rather than on NormalizedEvent itself: the flag marks how THIS
		// broadcast went out, not a property of the stored event, so it never persists
		// and never reaches the UI event feed's own copy of the same JSON.
		body["replay"] = true;
	}
	// A replay is gated to widget TYPES that accept one (AcceptsReplay) so `delivered`
	// means "a widget that can show this got it", not "some socket got a frame it was
	// always going to ignore" -- a live event has no such promise to keep and still
	// reaches every open widget, same as before. Either way the count is of widgets, so
	// the same alert box open in the editor preview and in a Browser Source is one.
	const size_t delivered = replay ? BroadcastFrame(DataFrame(body), nullptr, WidgetAcceptsReplay)
					: BroadcastFrame(DataFrame(body));
	if (delivered == 0) {
		// Ungated: an alert that reached the server and went nowhere is otherwise
		// indistinguishable from one that fired, and events are rare enough that saying
		// so every time costs nothing.
		HostLog("[overlay] event " + ev.type + " (" + ev.platform + ") delivered to 0 widgets" +
			(replay ? " (replay)" : ""));
	} else {
		DBG(LogCat::Overlay, "event %s (%s) delivered to %zu widget(s)%s", ev.type.c_str(), ev.platform.c_str(),
		    delivered, replay ? " (replay)" : "");
	}
	return delivered;
}

// Named `chat` event so widgets can select it independently of the default `message`
// (alert) stream; body matches the chat.message the bridge emits (the `event` key
// stripped by the chat hub's emit). Called on the chat transport worker, never TID_UI.
void OverlayServer::BroadcastChat(const nlohmann::json &chatMsg)
{
	BroadcastFrame(NamedFrame("chat", chatMsg));
}

// Named `viewers` event for the same reason `chat` is named: an unnamed frame lands on every
// widget's default `message` handler, which is the alert stream. Body is the poller's
// `viewers.changed` payload dumped as-is -- no reshaping, so a null or a missing
// perDestination row still means "did not answer" rather than zero. Called on the poll
// worker, never TID_UI.
void OverlayServer::BroadcastViewers(const nlohmann::json &viewers)
{
	BroadcastFrame(NamedFrame("viewers", viewers));
}

// Build a named frame, keep it as this channel's replay copy, then send it. The keep is a
// plain map write under sseMutex_; the send is BroadcastFrame's own snapshot-under-lock /
// send-unlocked path, so the lock is still never held across a blocking send.
void OverlayServer::BroadcastStateFrame(const char *eventName, const nlohmann::json &body)
{
	const std::string frame = NamedFrame(eventName, body);
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		replayFrames_[eventName] = frame;
	}
	BroadcastFrame(frame);
}

// Named `channels` event for the same reason `viewers` is named: an unnamed frame lands on
// every widget's default `message` handler, which is the alert stream. Body is the poller's
// `channels.stats` payload dumped as-is -- an account missing from perAccount was never read,
// a hidden or -1 entry is a withheld number, and neither is a zero. Called on the poll worker,
// never TID_UI.
void OverlayServer::BroadcastChannelStats(const nlohmann::json &stats)
{
	BroadcastStateFrame("channels", stats);
}

// Named `stream` event, for the same reason the three above are named. Body is the bridge's
// `streaming.changed` payload dumped as-is -- a null startedAt means no output has reported a
// start yet, never a zero epoch, and an empty `destinations` under active is a broadcast going
// out nowhere rather than a broadcast that ended. Called on TID_UI (the transition seam that
// owns the store reads); the frame is a few hundred bytes and fires only at a transition, not
// on a poll cadence.
void OverlayServer::BroadcastStreamState(const nlohmann::json &state)
{
	// This frame is the only place the broadcast's start time is known, so it is also
	// where the backfill window is set. A null startedAt under an active broadcast leaves
	// it at 0: no output has reported a start, so there is no window to replay over.
	int64_t startedAt = 0;
	if (state.is_object() && state.value("active", false)) {
		const auto it = state.find("startedAt");
		if (it != state.end() && it->is_number_integer()) {
			startedAt = it->get<int64_t>();
		}
	}
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		streamStartedAtMs_ = startedAt;
	}
	BroadcastStateFrame("stream", state);
}

size_t OverlayServer::BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev)
{
	return BroadcastFrame(DataFrame(ev.ToJson()), &widgetId);
}

// Deliberately NOT BroadcastStateFrame: that keeps the frame for replay and, for `stream`,
// also moves the backfill window. A preview fired from the editor would then be the state
// a real browser source picks up when it connects mid-broadcast.
size_t OverlayServer::SendTestFrame(const std::string &widgetId, const char *eventName, const nlohmann::json &body)
{
	return BroadcastFrame(NamedFrame(eventName, body), &widgetId);
}

// ---- Lifecycle --------------------------------------------------------------

OverlayServer::~OverlayServer()
{
	Stop();
}

bool OverlayServer::BindOn(int port)
{
	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		lastError_ = "socket() failed (" + std::to_string(WSAGetLastError()) + ")";
		return false;
	}
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 ONLY
	addr.sin_port = htons((unsigned short)port);
	if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		lastError_ = "bind(127.0.0.1:" + std::to_string(port) + ") failed (" +
			     std::to_string(WSAGetLastError()) + ")";
		closesocket(listenSock);
		return false;
	}
	if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
		lastError_ = "listen() failed (" + std::to_string(WSAGetLastError()) + ")";
		closesocket(listenSock);
		return false;
	}
	// Read back the actual port so a port-0 (OS-ephemeral) bind reports its assignment.
	sockaddr_in bound = {};
	int len = sizeof(bound);
	if (getsockname(listenSock, (sockaddr *)&bound, &len) == 0) {
		port_ = ntohs(bound.sin_port);
	} else {
		port_ = port;
	}
	listenSocket_ = (uintptr_t)listenSock;
	return true;
}

bool OverlayServer::Start()
{
	if (running_.load()) {
		return true;
	}
	lastError_.clear();

	WSADATA wsaData;
	const int wsaRc = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaRc != 0) {
		lastError_ = "WSAStartup failed (" + std::to_string(wsaRc) + ")";
		HostLog("[overlay] " + lastError_);
		return false;
	}
	wsaUp_ = true;

	// Persist-first for stable URLs: reuse the previously-bound port if it still binds,
	// else scan the scattered bands so a reserved block can't kill the feature.
	const int preferred = Store().Port() > 0 ? Store().Port() : kPreferredPort;
	bool bound = BindOn(preferred);
	for (int base = 0; !bound && base < (int)kScanBands.size(); ++base) {
		for (int off = 0; off < kBandSize; ++off) {
			const int p = kScanBands[base] + off;
			if (p == preferred) {
				continue; // already tried above
			}
			if (BindOn(p)) {
				bound = true;
				break;
			}
		}
	}
	if (!bound) {
		lastError_ = "no free port in any scan band (43000/47000/51000/55000/59000, 50 each)";
		HostLog("[overlay] " + lastError_);
		WSACleanup();
		wsaUp_ = false;
		return false;
	}

	portChanged_ = (port_ != preferred);
	Store().SetPort(port_);
	running_.store(true);
	acceptThread_ = std::thread(&OverlayServer::AcceptLoop, this);
	HostLog("[overlay] server listening on 127.0.0.1:" + std::to_string(port_) +
		(portChanged_ ? " (port changed)" : ""));
	return true;
}

bool OverlayServer::StartForTest(int port, int *boundPort)
{
	if (running_.load()) {
		if (boundPort) {
			*boundPort = port_;
		}
		return true;
	}
	lastError_.clear();

	WSADATA wsaData;
	const int wsaRc = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaRc != 0) {
		lastError_ = "WSAStartup failed (" + std::to_string(wsaRc) + ")";
		HostLog("[overlay] " + lastError_);
		return false;
	}
	wsaUp_ = true;

	if (!BindOn(port)) {
		HostLog("[overlay] StartForTest bind failed: " + lastError_);
		WSACleanup();
		wsaUp_ = false;
		return false;
	}
	if (boundPort) {
		*boundPort = port_;
	}
	running_.store(true);
	acceptThread_ = std::thread(&OverlayServer::AcceptLoop, this);
	return true;
}

void OverlayServer::Stop()
{
	if (!running_.exchange(false)) {
		// Not running; still balance a stray WSAStartup / join a late accept thread.
		if (acceptThread_.joinable()) {
			acceptThread_.join();
		}
		{
			std::lock_guard<std::mutex> lock(threadsMutex_);
			for (auto &c : threads_) {
				if (c.thread.joinable()) {
					c.thread.join();
				}
			}
			threads_.clear();
		}
		if (wsaUp_) {
			WSACleanup();
			wsaUp_ = false;
		}
		return;
	}

	// Close the listen socket to unblock accept().
	if (listenSocket_ != ~uintptr_t(0)) {
		closesocket((SOCKET)listenSocket_);
		listenSocket_ = ~uintptr_t(0);
	}
	// Join the accept thread first so it can spawn no more connections and every
	// accepted fd is already registered in clientSockets_ (inserted synchronously there).
	if (acceptThread_.joinable()) {
		acceptThread_.join();
	}
	// shutdown() (NOT close) every live client socket: this unblocks each owner thread's
	// parked recv/send without freeing the fd, so the owner remains the sole closer and
	// no fd is recycled mid-teardown. Each thread then closes its own fd on the way out.
	{
		std::lock_guard<std::mutex> lock(clientsMutex_);
		for (uintptr_t s : clientSockets_) {
			shutdown((SOCKET)s, SD_BOTH);
		}
	}
	// Join the connection threads (each closed its own fd + deregistered as it unwound).
	{
		std::lock_guard<std::mutex> lock(threadsMutex_);
		for (auto &c : threads_) {
			if (c.thread.joinable()) {
				c.thread.join();
			}
		}
		threads_.clear();
	}
	if (wsaUp_) {
		WSACleanup();
		wsaUp_ = false;
	}
	HostLog("[overlay] server stopped");
}

// ---- Accept + routing -------------------------------------------------------

void OverlayServer::ReapFinishedThreads()
{
	std::lock_guard<std::mutex> lock(threadsMutex_);
	for (auto it = threads_.begin(); it != threads_.end();) {
		if (it->done->load()) {
			if (it->thread.joinable()) {
				it->thread.join();
			}
			it = threads_.erase(it);
		} else {
			++it;
		}
	}
}

void OverlayServer::AcceptLoop()
{
	while (running_.load()) {
		ReapFinishedThreads();
		SOCKET client = accept((SOCKET)listenSocket_, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (!running_.load()) {
				break;
			}
			Sleep(10); // avoid a 100% busy-loop if accept keeps failing (e.g. WSAEMFILE)
			continue;
		}
		// Register the fd synchronously (before spawning) so a Stop() after this thread
		// joins can shutdown() every accepted connection.
		{
			std::lock_guard<std::mutex> lock(clientsMutex_);
			clientSockets_.insert((uintptr_t)client);
		}
		auto done = std::make_shared<std::atomic<bool>>(false);
		std::thread t([this, client, done]() {
			HandleConnection((uintptr_t)client);
			done->store(true);
		});
		std::lock_guard<std::mutex> lock(threadsMutex_);
		threads_.push_back(Conn{std::move(t), done});
	}
}

void OverlayServer::HandleConnection(uintptr_t clientSocket)
{
	const SOCKET sock = (SOCKET)clientSocket;

	// Bounded header read: a client that connects but never sends a full "\r\n\r\n" would
	// otherwise park this thread forever and hang Stop()'s join. Stop() also shutdown()s
	// the fd, but the timeout is the standalone backstop.
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&kHeaderRecvTimeoutMs, sizeof(kHeaderRecvTimeoutMs));
	// Bounded response send: a client that stops reading mid-response must not park this
	// thread in SendAll forever (the SSE path resets this to its own timeout below).
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&kResponseSendTimeoutMs,
		   sizeof(kResponseSendTimeoutMs));

	// Read the header block until "\r\n\r\n", capping total size.
	std::string buffer;
	char temp[4096];
	size_t headerEnd = std::string::npos;
	while (true) {
		const int n = recv(sock, temp, (int)sizeof(temp), 0);
		if (n <= 0) {
			CloseClient(clientSocket);
			return;
		}
		buffer.append(temp, (size_t)n);
		headerEnd = buffer.find("\r\n\r\n");
		if (headerEnd != std::string::npos) {
			break;
		}
		if (buffer.size() > kMaxHeaderBytes) {
			WriteResponse(sock, 431, "text/plain", "header too large");
			CloseClient(clientSocket);
			return;
		}
	}

	const std::string headerBlock = buffer.substr(0, headerEnd);
	const size_t lineEnd = headerBlock.find("\r\n");
	const std::string requestLine = lineEnd == std::string::npos ? headerBlock : headerBlock.substr(0, lineEnd);

	const size_t sp1 = requestLine.find(' ');
	const size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos) {
		WriteResponse(sock, 400, "text/plain", "malformed request");
		CloseClient(clientSocket);
		return;
	}
	const std::string method = requestLine.substr(0, sp1);
	const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
	if (method != "GET") {
		WriteResponse(sock, 405, "text/plain", "method not allowed");
		CloseClient(clientSocket);
		return;
	}

	std::string path;
	const std::string token = QueryToken(target, path);
	// Read here rather than in the handler: this is the only scope that still holds the
	// request's headers, and the asset route needs the client's validator to answer 304.
	const std::string ifNoneMatch = HeaderValue(headerBlock, "if-none-match");

	// Route table (order: most specific first). Data list, not a switch, so a new
	// top-level widget-type route is a one-line add. The handler owns socket close;
	// the SSE handler keeps the socket open for its stream lifetime.
	struct Route {
		const char *prefix;
		bool exact;
		void (OverlayServer::*handler)(uintptr_t, const std::string &, const std::string &,
					       const std::string &);
	};
	static const std::array<Route, 2> kRoutes = {{
		{"/runtime.js", true, &OverlayServer::ServeRuntime},
		{"/w/", false, &OverlayServer::ServeWidget},
	}};
	for (const auto &r : kRoutes) {
		const bool match = r.exact ? (path == r.prefix) : (path.rfind(r.prefix, 0) == 0);
		if (match) {
			(this->*r.handler)(clientSocket, path, token, ifNoneMatch);
			return;
		}
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::ServeRuntime(uintptr_t clientSocket, const std::string &, const std::string &token,
				 const std::string &ifNoneMatch)
{
	const SOCKET sock = (SOCKET)clientSocket;
	// runtime.js is non-sensitive, but keep the uniform token guard: accept if the
	// token matches ANY widget (or if there are none, e.g. a fresh install).
	const std::vector<Widget> widgets = Store().List();
	bool ok = widgets.empty();
	for (const Widget &w : widgets) {
		if (w.token == token) {
			ok = true;
			break;
		}
	}
	if (!ok) {
		WriteResponse(sock, 403, "text/plain", "forbidden");
		CloseClient(clientSocket);
		return;
	}
	std::string body;
	std::string ctype;
	if (!ReadFileGuarded(WebBundle::Root() + "/overlay", "runtime.js", body, ctype)) {
		WriteResponse(sock, 404, "text/plain", "runtime not found");
		CloseClient(clientSocket);
		return;
	}
	// Same validator the asset route uses, for the same reason: without one Chromium
	// cannot cache this even heuristically, so every widget load and every editor preview
	// rebuild refetched the whole runtime. The bytes only change on a rebuild, which a
	// content-derived ETag notices by itself.
	const std::string etag = StrongETag(body);
	const std::string cacheHeaders =
		"Cache-Control: private, max-age=" + std::to_string(kAssetMaxAgeSeconds) + "\r\nETag: " + etag + "\r\n";
	if (ETagMatches(ifNoneMatch, etag)) {
		WriteResponse(sock, 304, ctype, body, cacheHeaders, /*suppressBody=*/true);
		CloseClient(clientSocket);
		return;
	}
	WriteResponse(sock, 200, ctype, body, cacheHeaders);
	CloseClient(clientSocket);
}

void OverlayServer::ServeWidget(uintptr_t clientSocket, const std::string &path, const std::string &token,
				const std::string &ifNoneMatch)
{
	const SOCKET sock = (SOCKET)clientSocket;
	const std::string rest = path.substr(3); // after "/w/"
	const size_t slash = rest.find('/');
	const std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
	const std::string action = slash == std::string::npos ? std::string() : rest.substr(slash);
	if (id.empty()) {
		WriteResponse(sock, 404, "text/plain", "not found");
		CloseClient(clientSocket);
		return;
	}

	std::optional<Widget> w = Store().Get(id);
	if (!w) {
		WriteResponse(sock, 404, "text/plain", "no such overlay");
		CloseClient(clientSocket);
		return;
	}
	// A widget with no token is not a widget anyone may read: an absent ?t= arrives here
	// as the empty string and would otherwise compare equal to it.
	if (token.empty() || token != w->token) {
		WriteResponse(sock, 403, "text/plain", "forbidden");
		CloseClient(clientSocket);
		return;
	}

	if (action.empty() || action == "/") {
		// Never cacheable: the document is assembled per request and embeds the widget's
		// current token, port and resolved field values, so a reused copy could carry a
		// rotated token or the settings the owner just changed away from.
		WriteResponse(sock, 200, "text/html", AssembleDocument(*w, port_), "Cache-Control: no-store\r\n");
		CloseClient(clientSocket);
		return;
	}
	if (action == "/events") {
		RunSse(clientSocket, id); // owns the socket; closes it itself on the way out
		return;
	}
	if (action.rfind("/assets/", 0) == 0) {
		const std::string file = action.substr(std::strlen("/assets/"));
		std::string body;
		std::string ctype;
		if (!ReadFileGuarded(Store().AssetsDir(id), file, body, ctype)) {
			WriteResponse(sock, 404, "text/plain", "asset not found");
			CloseClient(clientSocket);
			return;
		}
		if (body.size() > kMaxAssetBytes) {
			WriteResponse(sock, 413, "text/plain", "asset too large");
			CloseClient(clientSocket);
			return;
		}
		// The only cacheable route. Without a validator Chromium cannot cache this even
		// heuristically, so every play of an alert sound refetched, re-demuxed and
		// re-decoded the clip -- which is what made alerts stutter.
		const std::string etag = StrongETag(body);
		const std::string cacheHeaders = "Cache-Control: private, max-age=" +
						 std::to_string(kAssetMaxAgeSeconds) + "\r\nETag: " + etag + "\r\n";
		if (ETagMatches(ifNoneMatch, etag)) {
			// The representation is passed so Content-Length still names what a 200
			// would have sent (RFC 9110 SS8.6); only the body write is suppressed.
			WriteResponse(sock, 304, ctype, body, cacheHeaders, /*suppressBody=*/true);
			CloseClient(clientSocket);
			return;
		}
		WriteResponse(sock, 200, ctype, body, cacheHeaders);
		CloseClient(clientSocket);
		return;
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::RunSse(uintptr_t clientSocket, const std::string &widgetId)
{
	const SOCKET sock = (SOCKET)clientSocket;
	// Bounded send so a stuck reader (full send buffer) can't park a broadcast pass or
	// this thread indefinitely; a timed-out send is a failed send -> the socket is dropped.
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&kSseSendTimeoutMs, sizeof(kSseSendTimeoutMs));

	bool atCapacity = false;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		// Cap concurrent live streams so a runaway client (or a page reload storm) can't
		// exhaust threads/fds. Count live + reserved under the lock and reserve a slot,
		// so the handshake below runs WITHOUT holding sseMutex_ (its sends are blocking,
		// bounded by SO_SNDTIMEO, and must never stall broadcasts).
		const size_t total = ssePending_ + LiveSseCount();
		if (total >= kMaxSseConnections) {
			atCapacity = true;
		} else {
			++ssePending_;
		}
	}
	if (atCapacity) {
		HostLog("[overlay] SSE connection rejected: at capacity (" + std::to_string(kMaxSseConnections) +
			" streams)");
		WriteResponse(sock, 503, "text/plain", "overlay server at capacity");
		CloseClient(clientSocket);
		return;
	}

	// The handshake must complete BEFORE the socket is registered: once it is in
	// sockets_ a concurrent broadcast may write frames, and no frame may precede the
	// HTTP header on the wire. Until registration the reserved slot keeps the capacity
	// count honest.
	const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
			   "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
			   "Access-Control-Allow-Origin: *\r\n\r\n";
	const bool headOk = send(sock, head, (int)strlen(head), 0) > 0;
	if (headOk) {
		// Initial comment so EventSource fires onopen promptly.
		send(sock, ": connected\n\n", 13, 0);
		// The state channels' last frames, copied under the lock and sent outside it.
		// Sent BEFORE registering: a broadcast landing in the gap is merely late,
		// whereas replaying after registration could deliver a stale frame on top of a
		// fresher one.
		//
		// Only a channel whose frames carry STATE replays, and it replays only its
		// latest. Audience totals poll on a ~15 minute cadence and stream state changes
		// only at a transition, so a browser source that loads in between would render
		// nothing until the next one -- an uptime widget would sit blank for the rest of
		// a broadcast. A viewer count is the counter-example: it stops being true the
		// instant a broadcast ends, so replaying one would assert an audience that is no
		// longer watching.
		//
		// Chat and events are moments rather than state, so neither replays as itself.
		// Events are still summarizable, though -- a running total over the current
		// broadcast is state even when the individual events are not -- so they reach a
		// connecting client as the separate, bounded `backfill` frame instead, which no
		// consumer of the moment-by-moment stream sees.
		std::vector<std::string> replay;
		int64_t since = 0;
		{
			std::lock_guard<std::mutex> lock(sseMutex_);
			replay.reserve(replayFrames_.size() + 1);
			for (const auto &[eventName, frame] : replayFrames_) {
				replay.push_back(frame);
			}
			since = streamStartedAtMs_;
		}
		// Built outside sseMutex_: it reads the event store, whose own lock must never be
		// taken under this one.
		if (since > 0) {
			replay.push_back(BuildBackfillFrame(since));
		}
		for (const std::string &frame : replay) {
			send(sock, frame.c_str(), (int)frame.size(), 0);
		}
	}
	size_t live = 0;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		--ssePending_;
		if (headOk) {
			sockets_[widgetId].insert(clientSocket);
			live = LiveSseCount();
		}
	}
	if (!headOk) {
		CloseClient(clientSocket); // never registered in sockets_; just close+deregister
		return;
	}
	// Logged outside sseMutex_: blog() writes to the session log, and the broadcast path
	// must never wait on that lock behind an I/O call.
	DBG(LogCat::Overlay, "SSE connected widget=%s (%zu live)", widgetId.c_str(), live);

	// The 15s recv timeout drives the keepalive; recv also detects a client close.
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&kSseRecvTimeoutMs, sizeof(kSseRecvTimeoutMs));
	char buf[512];
	while (running_.load()) {
		const int n = recv(sock, buf, sizeof(buf), 0);
		if (n == 0) {
			break; // client closed
		}
		if (n == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAETIMEDOUT) {
				{
					std::lock_guard<std::mutex> lock(sseMutex_);
					auto it = sockets_.find(widgetId);
					if (it == sockets_.end() || !it->second.count(clientSocket)) {
						break; // dropped by Broadcast/Stop (shutdown will also break recv)
					}
				}
				// Ping OUTSIDE sseMutex_: a stuck reader blocks this send for up to
				// kSseSendTimeoutMs, which must never stall broadcasts. Safe unlocked --
				// this thread owns the fd, and Broadcast/Stop only ever shutdown() it
				// (a concurrent drop just makes this send fail -> break).
				if (send(sock, ": ping\n\n", 8, 0) <= 0) {
					break;
				}
				continue;
			}
			break; // real error / shutdown() by Stop/Broadcast
		}
		// Ignore any client->server bytes.
	}
	// This thread owns the fd: deregister from the SSE registry, then close it exactly
	// once. Broadcast/Stop only ever shutdown() it, so no other thread closes this fd --
	// EXCEPT while a broadcast is mid-send (broadcastDepth_ > 0): its snapshot may still
	// hold this handle, so park the fd in deferredCloseSse_ and let the last broadcast to
	// finish close it, so no in-flight send lands on a recycled fd.
	size_t remaining = 0;
	bool deferClose = false;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		auto it = sockets_.find(widgetId);
		if (it != sockets_.end()) {
			it->second.erase(clientSocket);
			if (it->second.empty()) {
				sockets_.erase(it);
			}
		}
		remaining = LiveSseCount();
		if (broadcastDepth_ > 0) {
			deferredCloseSse_.insert(clientSocket);
			deferClose = true;
		}
	}
	DBG(LogCat::Overlay, "SSE closed widget=%s (%zu live)", widgetId.c_str(), remaining);
	if (deferClose) {
		return;
	}
	CloseClient(clientSocket);
}

void OverlayServer::CloseClient(uintptr_t sock)
{
	bool present = false;
	{
		std::lock_guard<std::mutex> lock(clientsMutex_);
		present = clientSockets_.erase(sock) > 0;
	}
	if (present) {
		closesocket((SOCKET)sock); // erase-guard => this fd is closed exactly once
	}
}

OverlayServer &Server()
{
	static OverlayServer server;
	return server;
}

} // namespace Overlay
