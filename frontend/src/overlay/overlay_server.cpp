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
#include "util/time_util.hpp"      // TimeUtil::NowMs
#include "overlay_assets.hpp"      // Overlay::MaxAssetBytes
#include "overlay_scopes.hpp"      // Overlay::BuildServedData, LibraryRoot, IsSafeLibraryPath
#include "overlay_store.hpp"       // Overlay::Store(), Widget, WidgetUrl
#include "overlay_template.hpp"    // Overlay::AcceptsReplay, Overlay::CountsEvents

#pragma comment(lib, "ws2_32.lib")

namespace Overlay {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxHeaderBytes = 16 * 1024; // 16 KB header block (mirrors mcp)
constexpr int kPreferredPort = 43000;         // persisted default; tried first for stable URLs
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

// Freshness for an uploaded widget asset and for the runtime. A day, because every URL
// AssembleDocument mints for them moves with the bytes: an asset URL carries the widget
// revision, which OverlayStore::AddAsset -- the only writer of those bytes -- bumps under the
// same lock as the write, and the runtime URL carries the runtime's content hash. So the
// cache key cannot outlive the bytes it names: a re-upload at the same filename, or a rebuilt
// runtime, still moves the URL. Freshness only
// has to outlast a broadcast, and a day is well past that. Not `immutable`: the ETag
// revalidation below is the backstop if a future writer ever appears that does not bump,
// and `immutable` would tell the client not to check even on a reload.
constexpr int kAssetMaxAgeSeconds = 86400;

// The bundled library (kLibraryRoutePrefix) is cached the same day, but its URLs carry no
// version, so once that is up its freshness rests on ETag revalidation alone; a library id
// never changes its sound, which is what makes a day safe.

// "</" inside a JSON string is legal JSON and would still close the <script> element it is
// inlined into -- a message a user typed as "</script>" would end the bootstrap early.
// "<\/" is the same string to a JSON parser.
std::string ScriptSafeJson(const json &value)
{
	std::string out = value.dump();
	size_t pos = 0;
	while ((pos = out.find("</", pos)) != std::string::npos) {
		out.replace(pos, 2, "<\\/");
		pos += 3;
	}
	return out;
}

// The cap a stored upload is served under: its record's kind, looked up by the served
// basename. A file no record names -- one left behind by a failed replace -- gets the
// default cap, which is what every file was held to before kinds had their own.
size_t AssetCapFor(const Widget &w, const std::string &file)
{
	for (const json &a : w.assets) {
		if (a.is_object() && a.value("file", std::string()) == file) {
			return MaxAssetBytes(a.value("kind", std::string()));
		}
	}
	return kDefaultAssetMaxBytes;
}

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

// FNV-1a 64 over a body we have already read, as 16 lowercase hex digits.
//
// Content-derived rather than mtime-derived deliberately. An asset is replaced in place at
// the same path by OverlayStore::AddAsset, and a filesystem timestamp has a granularity a
// fast replace can land inside -- which would hand a client a validator that says "still
// the same file" about different bytes. Hashing what we are about to serve cannot say that.
std::string ContentHash(const std::string &body)
{
	const uint64_t h = Fnv1a64(body);
	static const char *kHex = "0123456789abcdef";
	std::string out;
	for (int shift = 60; shift >= 0; shift -= 4) {
		out += kHex[(h >> shift) & 0xf];
	}
	return out;
}

// The strong ETag for a body: its ContentHash, quoted.
std::string StrongETag(const std::string &body)
{
	return "\"" + ContentHash(body) + "\"";
}

// The shipped overlay runtime, read from the web bundle. The one reader both routes use, so
// the version AssembleDocument stamps on the runtime URL is the hash of the bytes
// ServeRuntime then sends.
bool ReadRuntime(std::string &body, std::string &ctype)
{
	return ReadFileGuarded(WebBundle::Root() + "/overlay", "runtime.js", body, ctype);
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

// What a Range header asks of a body: all of it, one byte span of it, or a span it does not
// have.
enum class RangeVerdict { Whole, Part, Unsatisfiable };

// A run of ASCII digits as a number; false for anything else, and for more digits than any
// body this server holds could need.
bool ParseDigits(const std::string &s, size_t &out)
{
	constexpr size_t kMaxDigits = 15;
	if (s.empty() || s.size() > kMaxDigits) {
		return false;
	}
	out = 0;
	for (char c : s) {
		if (c < '0' || c > '9') {
			return false;
		}
		out = out * 10 + static_cast<size_t>(c - '0');
	}
	return true;
}

// One "bytes=first-last", "bytes=first-" or "bytes=-suffix" span of a body of `size` bytes,
// as inclusive [first, last]. A header this does not understand -- another unit, several
// spans, a malformed one -- is answered with the whole body, which RFC 9110 SS14.2 allows a
// server to do for any Range it chooses not to honour.
RangeVerdict ParseByteRange(const std::string &header, size_t size, size_t &first, size_t &last)
{
	constexpr char kUnit[] = "bytes=";
	if (header.rfind(kUnit, 0) != 0) {
		return RangeVerdict::Whole;
	}
	const std::string spec = header.substr(sizeof(kUnit) - 1);
	const size_t dash = spec.find('-');
	if (dash == std::string::npos || spec.find(',') != std::string::npos) {
		return RangeVerdict::Whole;
	}
	const std::string from = spec.substr(0, dash);
	const std::string to = spec.substr(dash + 1);
	size_t a = 0;
	size_t b = 0;
	if (from.empty()) {
		if (!ParseDigits(to, b)) {
			return RangeVerdict::Whole;
		}
		if (b == 0 || size == 0) {
			return RangeVerdict::Unsatisfiable;
		}
		first = b >= size ? 0 : size - b;
		last = size - 1;
		return RangeVerdict::Part;
	}
	if (!ParseDigits(from, a) || (!to.empty() && !ParseDigits(to, b))) {
		return RangeVerdict::Whole;
	}
	if (a >= size) {
		return RangeVerdict::Unsatisfiable;
	}
	last = to.empty() ? size - 1 : std::min(b, size - 1);
	if (last < a) {
		return RangeVerdict::Whole;
	}
	first = a;
	return RangeVerdict::Part;
}

// Send `body` as a cacheable representation: a strong ETag and `Cache-Control: <scope>,
// max-age=<day>`, answered 304 when the client's validator already names these bytes. The
// one implementation for every cacheable route, so they cannot drift apart on the parts a
// naive 304 gets wrong.
//
// A single byte range is honoured (206), because Chromium's media stack reads a <video> in
// spans and will not seek, or reliably loop, a WebM from a server that only ever sends it
// whole. If-Range is honoured too: a range asked against bytes that have since changed gets
// the new body whole rather than a span of it spliced onto the old one.
void WriteCacheable(SOCKET sock, const std::string &ctype, const std::string &body, const char *cacheScope,
		    const RequestHeaders &request)
{
	const std::string etag = StrongETag(body);
	const std::string cacheHeaders = "Cache-Control: " + std::string(cacheScope) +
					 ", max-age=" + std::to_string(kAssetMaxAgeSeconds) + "\r\nETag: " + etag +
					 "\r\nAccept-Ranges: bytes\r\n";
	if (ETagMatches(request.ifNoneMatch, etag)) {
		// The representation is passed so Content-Length still names what a 200 would have
		// sent (RFC 9110 SS8.6); only the body write is suppressed.
		WriteResponse(sock, 304, ctype, body, cacheHeaders, /*suppressBody=*/true);
		return;
	}
	const bool rangeApplies = !request.range.empty() && (request.ifRange.empty() || request.ifRange == etag);
	size_t first = 0;
	size_t last = 0;
	const RangeVerdict verdict = rangeApplies ? ParseByteRange(request.range, body.size(), first, last)
						  : RangeVerdict::Whole;
	if (verdict == RangeVerdict::Part) {
		WriteResponse(sock, 206, ctype, body.substr(first, last - first + 1),
			      cacheHeaders + "Content-Range: bytes " + std::to_string(first) + "-" +
				      std::to_string(last) + "/" + std::to_string(body.size()) + "\r\n");
		return;
	}
	if (verdict == RangeVerdict::Unsatisfiable) {
		WriteResponse(sock, 416, "text/plain", std::string(),
			      cacheHeaders + "Content-Range: bytes */" + std::to_string(body.size()) + "\r\n");
		return;
	}
	WriteResponse(sock, 200, ctype, body, cacheHeaders);
}

// Build the served widget document (spec shell): user CSS in <style>, user HTML in
// <body>, then window.__OVERLAY__, the runtime, then the user JS.
std::string AssembleDocument(const Widget &w, int port)
{
	// One resolution for the whole document: a stock widget's schema and its markup both
	// come off disk, and taking them together is what keeps a template shipped with a new
	// field from reaching this page as markup from one read and a schema from another.
	const ResolvedWidget resolved = Resolve(w);
	const ServedData served = BuildServedData(w, resolved);
	json overlay = json{{"id", w.id},
			    {"token", w.token},
			    {"port", port},
			    {"fields", served.fields},
			    {"sounds", served.sounds}};
	if (!served.scopes.is_null()) {
		overlay["scopes"] = served.scopes;
	}
	std::string doc = "<!doctype html><html><head><meta charset=\"utf-8\">\n<style>\n";
	doc += resolved.css;
	doc += "\n</style></head><body>\n";
	doc += resolved.html;
	doc += "\n<script>window.__OVERLAY__=" + ScriptSafeJson(overlay) + ";</script>\n";
	// `v` is the runtime's content hash. The runtime is cached for a day, but this document
	// is not cached at all and inlines a template written against one runtime; without `v`
	// a rebuild that changed both would pair the new template with the day-old runtime, and
	// a hook the template calls would not exist yet. Moving the URL with the bytes makes the
	// runtime a page loads the one it was assembled beside. ServeRuntime reads only `t`, so
	// `v` is a pure cache key.
	std::string runtimeUrl = "/runtime.js?t=" + w.token;
	std::string runtimeBody;
	std::string runtimeType;
	if (ReadRuntime(runtimeBody, runtimeType)) {
		runtimeUrl += "&v=" + ContentHash(runtimeBody);
	}
	doc += "<script src=\"" + runtimeUrl + "\"></script>\n";
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

// The BroadcastFrame widgetFilter for a moved tally window: only a widget that counts events
// is sent one, as only such a widget is sent one on connect. TypeOf for the reason above.
bool WidgetCountsEvents(const std::string &widgetId)
{
	const std::optional<std::string> type = Store().TypeOf(widgetId);
	return type.has_value() && CountsEvents(*type);
}

// A frame a preview test sends, marked so a page can tell it from the real thing.
json AsTest(json body)
{
	if (body.is_object()) {
		body["test"] = true;
	}
	return body;
}

} // namespace

// ---- Broadcast --------------------------------------------------------------

// Snapshot the live socket handles under sseMutex_, then send OUTSIDE the lock so a single
// slow/dead client (bounded by SO_SNDTIMEO) can't hold the mutex and stall delivery to
// every other client (head-of-line blocking). Each send holds that socket's send mutex, so
// a frame never interleaves with another thread's on the wire, and one sent to a socket
// still being handed its replay waits for the replay to finish. Dead sockets are pruned on
// a re-lock. A failed send drops the socket from the registry and shutdown()s it (NOT
// close): the owning RunSse is the sole closer of its fd, so shutdown unblocks that
// thread's recv without freeing the fd -- the OS can't recycle the fd value onto a NEW
// connection and have this thread later close the wrong socket (the fd-reuse hazard).
// broadcastDepth_ (incremented while unlocked) makes RunSse defer its own closesocket() so
// an in-flight send here can never land on a recycled fd.
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
	std::vector<std::pair<std::string, std::vector<std::pair<uintptr_t, SendMutex>>>> targets;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		for (auto &[wid, socks] : sockets_) {
			if (onlyWidgetId && wid != *onlyWidgetId) {
				continue;
			}
			targets.emplace_back(wid,
					     std::vector<std::pair<uintptr_t, SendMutex>>(socks.begin(), socks.end()));
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
		for (const auto &[s, sendMutex] : socks) {
			bool sent = false;
			{
				std::lock_guard<std::mutex> sending(*sendMutex);
				sent = SendAll((SOCKET)s, frame.data(), frame.size());
			}
			if (sent) {
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
	// Counted before it is sent, so a page that registers in between and reads the tally
	// finds it either counted there or on its way to the page, never in neither (RunSse).
	// A replay is a second showing of an event already counted.
	if (!replay) {
		tally_.Add(ev);
	}
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

// Called on the chat transport worker right after the op redacted the ring, so it lands
// behind every `chat` line that destination's read worker admitted before it. That includes a
// Twitch local echo, which the read worker admits on its USERSTATE; only an echo the hub sends
// itself (no nonce, or too many sends in flight) goes out from the send worker, unordered.
void OverlayServer::BroadcastChatModeration(const nlohmann::json &op)
{
	BroadcastFrame(NamedFrame("moderation", op));
}

// Called on the chat transport worker, after the store dropped the messages it names.
void OverlayServer::BroadcastEventRedaction(const nlohmann::json &ids)
{
	BroadcastFrame(NamedFrame("eventredaction", ids));
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
	// This frame is the only place the broadcast's start time is known, so it is what moves
	// the tally's window -- latched, so a start that drifts while live moves nothing, and a
	// null startedAt (no output has reported a start yet) opens nothing.
	const bool active = state.is_object() && state.value("active", false);
	int64_t startedAt = 0;
	if (active) {
		const auto it = state.find("startedAt");
		if (it != state.end() && it->is_number_integer()) {
			startedAt = it->get<int64_t>();
		}
	}
	const std::optional<json> moved = tally_.OnStreamState(active, startedAt, TimeUtil::NowMs());
	BroadcastStateFrame("stream", state);
	// A page sees a window move only here, as the server's own figure for it. An event sent
	// on another thread can land either side of this frame; the page keeps one that lands
	// before it and dedupes one the frame already counted by its recentIds.
	if (moved) {
		BroadcastFrame(NamedFrame("tally", *moved), nullptr, WidgetCountsEvents);
	}
}

void OverlayServer::SaveTallyIfDue()
{
	tally_.SaveIfDue(TimeUtil::NowMs());
}

size_t OverlayServer::BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev)
{
	return BroadcastFrame(DataFrame(AsTest(ev.ToJson())), &widgetId);
}

// Deliberately NOT BroadcastStateFrame: that keeps the frame for replay, and for `stream`
// BroadcastStreamState would also move the tally's window. A preview fired from the editor
// would then be the state a real browser source picks up when it connects mid-broadcast.
size_t OverlayServer::SendTestFrame(const std::string &widgetId, const char *eventName, const nlohmann::json &body)
{
	return BroadcastFrame(NamedFrame(eventName, AsTest(body)), &widgetId);
}

void OverlayServer::SetRegisteredObserverForTest(std::function<void(const std::string &widgetId)> observer)
{
	std::lock_guard<std::mutex> lock(sseMutex_);
	registeredObserver_ = std::move(observer);
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
	tally_.Open(BroadcastTally::FilePath());
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
	// The tally's trailing save: events since the last one are otherwise only in memory.
	tally_.Flush();
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
	RequestHeaders headers;
	headers.ifNoneMatch = HeaderValue(headerBlock, "if-none-match");
	headers.range = HeaderValue(headerBlock, "range");
	headers.ifRange = HeaderValue(headerBlock, "if-range");

	// Route table (order: most specific first). Data list, not a switch, so a new
	// top-level widget-type route is a one-line add. The handler owns socket close;
	// the SSE handler keeps the socket open for its stream lifetime.
	struct Route {
		const char *prefix;
		bool exact;
		void (OverlayServer::*handler)(uintptr_t, const std::string &, const std::string &,
					       const RequestHeaders &);
	};
	static const std::array<Route, 3> kRoutes = {{
		{"/runtime.js", true, &OverlayServer::ServeRuntime},
		{"/w/", false, &OverlayServer::ServeWidget},
		{kLibraryRoutePrefix, false, &OverlayServer::ServeLibrary},
	}};
	for (const auto &r : kRoutes) {
		const bool match = r.exact ? (path == r.prefix) : (path.rfind(r.prefix, 0) == 0);
		if (match) {
			(this->*r.handler)(clientSocket, path, token, headers);
			return;
		}
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::ServeRuntime(uintptr_t clientSocket, const std::string &, const std::string &token,
				 const RequestHeaders &headers)
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
	if (!ReadRuntime(body, ctype)) {
		WriteResponse(sock, 404, "text/plain", "runtime not found");
		CloseClient(clientSocket);
		return;
	}
	// Same validator the asset route uses, for the same reason: without one Chromium
	// cannot cache this even heuristically, so every widget load and every editor preview
	// rebuild refetched the whole runtime. The bytes only change on a rebuild, and the `v`
	// AssembleDocument puts on this URL moves with them, so a fresh entry is never a stale one.
	WriteCacheable(sock, ctype, body, "private", headers);
	CloseClient(clientSocket);
}

void OverlayServer::ServeWidget(uintptr_t clientSocket, const std::string &path, const std::string &token,
				const RequestHeaders &headers)
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
		RunSse(clientSocket, id, CountsEvents(w->type)); // owns the socket; closes it itself on the way out
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
		if (body.size() > AssetCapFor(*w, file)) {
			WriteResponse(sock, 413, "text/plain", "asset too large");
			CloseClient(clientSocket);
			return;
		}
		// Without a validator Chromium cannot cache this even heuristically, so every play
		// of an alert sound refetched, re-demuxed and re-decoded the clip -- which is what
		// made alerts stutter.
		WriteCacheable(sock, ctype, body, "private", headers);
		CloseClient(clientSocket);
		return;
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::ServeLibrary(uintptr_t clientSocket, const std::string &path, const std::string &,
				 const RequestHeaders &headers)
{
	const SOCKET sock = (SOCKET)clientSocket;
	const std::string rel = path.substr(std::strlen(kLibraryRoutePrefix));
	std::string body;
	std::string ctype;
	if (!IsSafeLibraryPath(rel) || !ReadFileGuarded(LibraryRoot(), rel, body, ctype)) {
		WriteResponse(sock, 404, "text/plain", "not found");
		CloseClient(clientSocket);
		return;
	}
	// Public: the same bytes for every widget and every client, unlike an upload.
	WriteCacheable(sock, ctype, body, "public", headers);
	CloseClient(clientSocket);
}

void OverlayServer::RunSse(uintptr_t clientSocket, const std::string &widgetId, bool tally)
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
	// count honest, and this thread is the only one that can reach the socket.
	const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
			   "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
			   "Access-Control-Allow-Origin: *\r\n\r\n";
	// Initial comment so EventSource fires onopen promptly.
	const char *connected = ": connected\n\n";
	const bool headOk = send(sock, head, (int)strlen(head), 0) > 0 &&
			    send(sock, connected, (int)strlen(connected), 0) > 0;
	if (!headOk) {
		{
			std::lock_guard<std::mutex> lock(sseMutex_);
			--ssePending_;
		}
		CloseClient(clientSocket); // never registered in sockets_; just close+deregister
		return;
	}

	// Then the state this page starts from: the state channels' last frames, the backfill
	// and, for a widget that counts events, the tally.
	//
	// Only a channel whose frames carry STATE replays, and it replays only its latest.
	// Audience totals poll on a ~15 minute cadence and stream state changes only at a
	// transition, so a browser source that loads in between would render nothing until the
	// next one -- an uptime widget would sit blank for the rest of a broadcast. A viewer count
	// is the counter-example: it stops being true the instant a broadcast ends, so replaying
	// one would assert an audience that is no longer watching.
	//
	// Chat and events are moments rather than state, so neither replays as itself. Events
	// are still summarizable, though -- a running total over the current broadcast is state
	// even when the individual events are not -- so they reach a connecting client as the
	// separate, bounded `backfill` frame instead, which no consumer of the moment-by-moment
	// stream sees. A widget that COUNTS events gets `tally` last: the broadcast tally's
	// totals for the current broadcast or -- off air -- the most recent one, so a reload
	// neither resets the count nor drops the finished broadcast's figure. It always comes,
	// with a null window before any broadcast was recorded, so the page can tell "nothing to
	// rebuild" from "not heard yet".
	//
	// The socket is registered FIRST, then everything is read, with this socket's send mutex
	// held from before registration until the replay is out. So an event or a transition
	// broadcast after a read is not lost in a gap: it reaches this socket, queued behind the
	// replay by the send mutex. One broadcast just before a read can be in both: a counting
	// page dedupes such an event by the tally's recentIds, a backfill reader by the event's
	// id, and a state frame simply arrives twice. A state frame is never older than the
	// replay it follows: each state channel is sent from one thread, so a frame's send has
	// started before the next frame replaces it in replayFrames_.
	const SendMutex sendMutex = std::make_shared<std::mutex>();
	std::unique_lock<std::mutex> sending(*sendMutex);
	std::vector<std::string> replay;
	size_t live = 0;
	std::function<void(const std::string &)> registered;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		--ssePending_;
		sockets_[widgetId].emplace(clientSocket, sendMutex);
		live = LiveSseCount();
		registered = registeredObserver_;
		replay.reserve(replayFrames_.size() + 2);
		for (const auto &[eventName, frame] : replayFrames_) {
			replay.push_back(frame);
		}
	}
	if (registered) {
		registered(widgetId);
	}
	// Read outside sseMutex_: the event store's and the tally's own locks are never taken
	// under it.
	const int64_t since = tally_.OpenSince();
	if (since > 0) {
		replay.push_back(BuildBackfillFrame(since));
	}
	if (tally) {
		replay.push_back(NamedFrame("tally", tally_.Snapshot()));
	}
	bool replayed = true;
	for (const std::string &frame : replay) {
		if (!SendAll(sock, frame.data(), frame.size())) {
			replayed = false;
			break;
		}
	}
	sending.unlock();
	// Logged outside sseMutex_: blog() writes to the session log, and the broadcast path
	// must never wait on that lock behind an I/O call.
	DBG(LogCat::Overlay, "SSE connected widget=%s (%zu live)", widgetId.c_str(), live);

	// The 15s recv timeout drives the keepalive; recv also detects a client close.
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&kSseRecvTimeoutMs, sizeof(kSseRecvTimeoutMs));
	char buf[512];
	while (replayed && running_.load()) {
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
				// kSseSendTimeoutMs, which must never stall broadcasts to other sockets.
				// Under this socket's send mutex like every other write to it. This thread
				// owns the fd, and Broadcast/Stop only ever shutdown() it (a concurrent drop
				// just makes this send fail -> break).
				bool pinged = false;
				{
					std::lock_guard<std::mutex> pinging(*sendMutex);
					pinged = send(sock, ": ping\n\n", 8, 0) > 0;
				}
				if (!pinged) {
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
