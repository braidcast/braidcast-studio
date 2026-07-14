#include "overlay_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h> // Sleep (winsock2.h already set _WINSOCKAPI_, so no winsock v1)

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../log.hpp"
#include "util/paths.hpp"    // RundirRoot
#include "overlay_store.hpp" // Overlay::Store(), Widget, WidgetUrl

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
constexpr DWORD kSseSendTimeoutMs = 3000;      // I3: bounded send so a stuck reader can't hold sseMutex_
constexpr DWORD kHeaderRecvTimeoutMs = 10000;  // I1: backstop so a silent client can't park a thread
constexpr DWORD kResponseSendTimeoutMs = 3000; // bounded plain-HTTP send so a stuck reader can't park a thread
constexpr size_t kMaxSseConnections = 64;      // ceiling on concurrent live SSE streams; excess rejected 503

struct ReasonPhrase {
	int status;
	const char *phrase;
};

// Data-driven reason map (not a switch) so adding a status is a one-line edit.
constexpr std::array<ReasonPhrase, 7> kReasons = {{
	{200, "OK"},
	{403, "Forbidden"},
	{404, "Not Found"},
	{405, "Method Not Allowed"},
	{413, "Payload Too Large"},
	{500, "Internal Server Error"},
	{503, "Service Unavailable"},
}};

const char *ReasonFor(int status)
{
	for (const auto &r : kReasons) {
		if (r.status == status) {
			return r.phrase;
		}
	}
	return "OK";
}

// Absolute path to the offline bundle root (copy of scheme.cpp::BundleRoot).
std::string BundleRoot()
{
	return RundirRoot() + "/data/braidcast/web";
}

// Map a file extension to a MIME type (copy of scheme.cpp::ContentTypeForPath).
std::string ContentTypeForPath(const std::string &path)
{
	static const std::vector<std::pair<std::string, std::string>> kTypes = {
		{".html", "text/html"},        {".htm", "text/html"},        {".js", "text/javascript"},
		{".mjs", "text/javascript"},   {".css", "text/css"},         {".json", "application/json"},
		{".svg", "image/svg+xml"},     {".png", "image/png"},        {".jpg", "image/jpeg"},
		{".jpeg", "image/jpeg"},       {".gif", "image/gif"},        {".ico", "image/x-icon"},
		{".woff", "font/woff"},        {".woff2", "font/woff2"},     {".ttf", "font/ttf"},
		{".wasm", "application/wasm"}, {".map", "application/json"}, {".txt", "text/plain"},
		{".mp3", "audio/mpeg"},        {".ogg", "audio/ogg"},        {".wav", "audio/wav"},
		{".webp", "image/webp"},
	};
	size_t dot = path.find_last_of('.');
	if (dot != std::string::npos) {
		std::string ext = path.substr(dot);
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(::tolower(c)); });
		for (const auto &[suffix, type] : kTypes) {
			if (ext == suffix) {
				return type;
			}
		}
	}
	return "application/octet-stream";
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
	std::ifstream f(full, std::ios::binary);
	if (!f) {
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	ctype = ContentTypeForPath(rel);
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
void WriteResponse(SOCKET sock, int status, const std::string &ctype, const std::string &body)
{
	std::string head = "HTTP/1.1 " + std::to_string(status) + " " + ReasonFor(status) + "\r\n";
	head += "Content-Type: " + ctype + "\r\n";
	head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	head += "Connection: close\r\n";
	head += "Access-Control-Allow-Origin: *\r\n";
	head += "\r\n";
	const std::string out = head + body;
	SendAll(sock, out.data(), out.size());
}

// Build the served widget document (spec shell): user CSS in <style>, user HTML in
// <body>, then window.__OVERLAY__, the runtime, then the user JS.
std::string AssembleDocument(const Widget &w, int port)
{
	json fieldData = json::object();
	for (const json &f : w.fields) {
		if (!f.contains("key")) {
			continue;
		}
		json value = f.value("value", json(nullptr));
		// An uploaded asset field stores the portable, token-less "assets/<file>" as its
		// persisted value. Rewrite ONLY the injected copy to the absolute tokenized URL the
		// server actually serves (/w/<id>/assets/<file>?t=<token>); a bare "assets/<file>"
		// would resolve against /w/ (no <base>) and 404, and lacks the required token. Match
		// the prefix so it works regardless of the field's declared type. w.fields (the
		// persisted value) is left untouched so it survives token/port changes.
		if (value.is_string()) {
			const std::string s = value.get<std::string>();
			if (s.rfind("assets/", 0) == 0) {
				value = "/w/" + w.id + "/" + s + "?t=" + w.token;
			}
		}
		fieldData[f["key"].get<std::string>()] = std::move(value);
	}
	const json overlay = json{{"id", w.id}, {"token", w.token}, {"port", port}, {"fields", fieldData}};
	std::string doc = "<!doctype html><html><head><meta charset=\"utf-8\">\n<style>\n";
	doc += w.css;
	doc += "\n</style></head><body>\n";
	doc += w.html;
	doc += "\n<script>window.__OVERLAY__=" + overlay.dump() + ";</script>\n";
	doc += "<script src=\"/runtime.js?t=" + w.token + "\"></script>\n";
	doc += "<script>\n" + w.js + "\n</script>\n</body></html>";
	return doc;
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
void OverlayServer::BroadcastFrame(const std::string &frame)
{
	std::vector<std::pair<std::string, uintptr_t>> targets;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		for (auto &[wid, socks] : sockets_) {
			for (uintptr_t s : socks) {
				targets.emplace_back(wid, s);
			}
		}
		++broadcastDepth_;
	}

	std::vector<std::pair<std::string, uintptr_t>> dead;
	for (auto &[wid, s] : targets) {
		if (!SendAll((SOCKET)s, frame.data(), frame.size())) {
			dead.emplace_back(wid, s);
		}
	}

	std::vector<uintptr_t> toClose;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		for (auto &[wid, s] : dead) {
			// Re-check membership by handle: RunSse may have dropped (and deferred the
			// close of) this socket while we were unlocked. Only shutdown() one still
			// registered, so we never touch an fd another path is already tearing down.
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
}

void OverlayServer::Broadcast(const Events::NormalizedEvent &ev)
{
	BroadcastFrame("data: " + ev.ToJson().dump() + "\n\n");
}

// Named `chat` event so widgets can select it independently of the default `message`
// (alert) stream; body matches chat.message post-`RouteEmit` (the `event` key stripped).
void OverlayServer::BroadcastChat(const nlohmann::json &chatMsg)
{
	BroadcastFrame("event: chat\ndata: " + chatMsg.dump() + "\n\n");
}

void OverlayServer::BroadcastTo(const std::string &widgetId, const Events::NormalizedEvent &ev)
{
	const std::string frame = "data: " + ev.ToJson().dump() + "\n\n";
	std::lock_guard<std::mutex> lock(sseMutex_);
	auto it = sockets_.find(widgetId);
	if (it == sockets_.end()) {
		return;
	}
	std::vector<uintptr_t> dead;
	for (uintptr_t s : it->second) {
		if (!SendAll((SOCKET)s, frame.data(), frame.size())) {
			dead.push_back(s);
		}
	}
	for (uintptr_t s : dead) {
		it->second.erase(s); // `it` stays valid: erasing set elements, not map entries
		shutdown((SOCKET)s, SD_BOTH);
	}
	if (it->second.empty()) {
		sockets_.erase(it);
	}
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
			WriteResponse(sock, 413, "text/plain", "header too large");
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
		WriteResponse(sock, 405, "text/plain", "malformed request");
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

	// Route table (order: most specific first). Data list, not a switch, so a new
	// top-level widget-type route is a one-line add. The handler owns socket close;
	// the SSE handler keeps the socket open for its stream lifetime.
	struct Route {
		const char *prefix;
		bool exact;
		void (OverlayServer::*handler)(uintptr_t, const std::string &, const std::string &);
	};
	static const std::array<Route, 2> kRoutes = {{
		{"/runtime.js", true, &OverlayServer::ServeRuntime},
		{"/w/", false, &OverlayServer::ServeWidget},
	}};
	for (const auto &r : kRoutes) {
		const bool match = r.exact ? (path == r.prefix) : (path.rfind(r.prefix, 0) == 0);
		if (match) {
			(this->*r.handler)(clientSocket, path, token);
			return;
		}
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::ServeRuntime(uintptr_t clientSocket, const std::string &, const std::string &token)
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
	if (!ReadFileGuarded(BundleRoot() + "/overlay", "runtime.js", body, ctype)) {
		WriteResponse(sock, 404, "text/plain", "runtime not found");
		CloseClient(clientSocket);
		return;
	}
	WriteResponse(sock, 200, ctype, body);
	CloseClient(clientSocket);
}

void OverlayServer::ServeWidget(uintptr_t clientSocket, const std::string &path, const std::string &token)
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
	if (token != w->token) {
		WriteResponse(sock, 403, "text/plain", "forbidden");
		CloseClient(clientSocket);
		return;
	}

	if (action.empty() || action == "/") {
		WriteResponse(sock, 200, "text/html", AssembleDocument(*w, port_));
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
		WriteResponse(sock, 200, ctype, body);
		CloseClient(clientSocket);
		return;
	}
	WriteResponse(sock, 404, "text/plain", "not found");
	CloseClient(clientSocket);
}

void OverlayServer::RunSse(uintptr_t clientSocket, const std::string &widgetId)
{
	const SOCKET sock = (SOCKET)clientSocket;
	// Bounded send so a stuck reader (full send buffer) can't hold sseMutex_ in
	// Broadcast; a timed-out send is treated as a failed send -> the socket is dropped.
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&kSseSendTimeoutMs, sizeof(kSseSendTimeoutMs));

	bool atCapacity = false;
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		// Cap concurrent live streams so a runaway client (or a page reload storm) can't
		// exhaust threads/fds. Count under the lock, before committing this connection.
		size_t total = 0;
		for (const auto &[wid, socks] : sockets_) {
			total += socks.size();
		}
		if (total >= kMaxSseConnections) {
			atCapacity = true;
		} else {
			const char *head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
					   "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
					   "Access-Control-Allow-Origin: *\r\n\r\n";
			if (send(sock, head, (int)strlen(head), 0) <= 0) {
				CloseClient(clientSocket); // not yet registered in sockets_; just close+deregister
				return;
			}
			// Initial comment so EventSource fires onopen promptly.
			send(sock, ": connected\n\n", 13, 0);
			sockets_[widgetId].insert(clientSocket);
		}
	}
	if (atCapacity) {
		HostLog("[overlay] SSE connection rejected: at capacity (" + std::to_string(kMaxSseConnections) +
			" streams)");
		WriteResponse(sock, 503, "text/plain", "overlay server at capacity");
		CloseClient(clientSocket);
		return;
	}

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
				std::lock_guard<std::mutex> lock(sseMutex_);
				auto it = sockets_.find(widgetId);
				if (it == sockets_.end() || !it->second.count(clientSocket)) {
					break; // dropped by Broadcast/Stop (shutdown will also break recv)
				}
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
	{
		std::lock_guard<std::mutex> lock(sseMutex_);
		auto it = sockets_.find(widgetId);
		if (it != sockets_.end()) {
			it->second.erase(clientSocket);
			if (it->second.empty()) {
				sockets_.erase(it);
			}
		}
		if (broadcastDepth_ > 0) {
			deferredCloseSse_.insert(clientSocket);
			return;
		}
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
