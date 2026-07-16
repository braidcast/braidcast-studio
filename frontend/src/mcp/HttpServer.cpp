#include "mcp/HttpServer.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>

#include "log.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace Mcp {

namespace {

// Caps so a malformed/hostile client cannot exhaust memory.
constexpr size_t kMaxHeaderBytes = 16 * 1024;     // 16 KB header block
constexpr size_t kMaxBodyBytes = 1 * 1024 * 1024; // 1 MB body

// Bounded recv so a client that connects but never sends data can't park the accept
// thread (and thus Stop()'s join) forever. Stop() also shutdown()s the live client
// socket directly for an immediate unblock; this is the standalone backstop for the
// case where the server keeps running (mirrors overlay_server's kHeaderRecvTimeoutMs).
constexpr DWORD kHeaderRecvTimeoutMs = 10000;

struct ReasonPhrase {
	int status;
	const char *phrase;
};

// Data-driven reason map (not a switch) so adding a status is a one-line edit.
constexpr std::array<ReasonPhrase, 11> kReasons = {{
	{200, "OK"},
	{202, "Accepted"},
	{400, "Bad Request"},
	{401, "Unauthorized"},
	{404, "Not Found"},
	{405, "Method Not Allowed"},
	{413, "Payload Too Large"},
	{431, "Request Header Fields Too Large"},
	{500, "Internal Server Error"},
	{504, "Gateway Timeout"},
	{200, "OK"}, // sentinel/duplicate to keep the array size stable on edits
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

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
	return s;
}

std::string Trim(const std::string &s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) {
		++b;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
		--e;
	}
	return s.substr(b, e - b);
}

// Write a full HTTP/1.1 response with Connection: close, then return. Best-effort
// send; partial-write failures just close the socket below.
void WriteResponse(SOCKET sock, const HttpResponse &resp)
{
	std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " " + ReasonFor(resp.status) + "\r\n";
	head += "Content-Type: " + resp.contentType + "\r\n";
	head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
	head += "Connection: close\r\n";
	head += "\r\n";

	std::string out = head + resp.body;
	size_t sent = 0;
	while (sent < out.size()) {
		const int chunk = (int)std::min<size_t>(out.size() - sent, 64 * 1024);
		const int n = send(sock, out.data() + sent, chunk, 0);
		if (n <= 0) {
			break;
		}
		sent += (size_t)n;
	}
}

void WriteSimple(SOCKET sock, int status, const std::string &jsonBody)
{
	HttpResponse resp;
	resp.status = status;
	resp.body = jsonBody;
	WriteResponse(sock, resp);
}

} // namespace

HttpServer::~HttpServer()
{
	Stop();
}

bool HttpServer::Start(int port, HttpHandler handler)
{
	if (running_.load()) {
		return true;
	}
	lastError_.clear();

	WSADATA wsaData;
	const int wsaRc = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaRc != 0) {
		lastError_ = "WSAStartup failed (" + std::to_string(wsaRc) + ")";
		HostLog("[mcp] http: " + lastError_);
		return false;
	}
	wsaUp_ = true;

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET) {
		lastError_ = "socket() failed (" + std::to_string(WSAGetLastError()) + ")";
		HostLog("[mcp] http: " + lastError_);
		WSACleanup();
		wsaUp_ = false;
		return false;
	}

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 ONLY
	addr.sin_port = htons((unsigned short)port);

	if (bind(listenSock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		lastError_ = "bind(127.0.0.1:" + std::to_string(port) + ") failed (" +
			     std::to_string(WSAGetLastError()) + ", port in use?)";
		HostLog("[mcp] http: " + lastError_);
		closesocket(listenSock);
		WSACleanup();
		wsaUp_ = false;
		return false;
	}

	if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
		lastError_ = "listen() failed (" + std::to_string(WSAGetLastError()) + ")";
		HostLog("[mcp] http: " + lastError_);
		closesocket(listenSock);
		WSACleanup();
		wsaUp_ = false;
		return false;
	}

	listenSocket_ = (uintptr_t)listenSock;
	handler_ = std::move(handler);
	running_.store(true);
	acceptThread_ = std::thread(&HttpServer::AcceptLoop, this);
	return true;
}

void HttpServer::Stop()
{
	if (!running_.exchange(false)) {
		// Not running; still balance a stray WSAStartup if Start aborted late.
		if (acceptThread_.joinable()) {
			acceptThread_.join();
		}
		if (wsaUp_) {
			WSACleanup();
			wsaUp_ = false;
		}
		return;
	}

	// Close the listen socket to unblock accept(), then join.
	if (listenSocket_ != ~uintptr_t(0)) {
		closesocket((SOCKET)listenSocket_);
		listenSocket_ = ~uintptr_t(0);
	}
	// shutdown() (NOT close) any client socket HandleConnection is currently parked on:
	// this unblocks its recv without freeing the fd, so HandleConnection remains the sole
	// closer once it returns (mirrors overlay_server's Stop()).
	{
		std::lock_guard<std::mutex> lock(clientMutex_);
		if (clientSocket_ != ~uintptr_t(0)) {
			shutdown((SOCKET)clientSocket_, SD_BOTH);
		}
	}
	if (acceptThread_.joinable()) {
		acceptThread_.join();
	}
	if (wsaUp_) {
		WSACleanup();
		wsaUp_ = false;
	}
}

void HttpServer::AcceptLoop()
{
	while (running_.load()) {
		SOCKET client = accept((SOCKET)listenSocket_, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			// Either we're shutting down (listen socket closed) or a transient
			// error; loop re-checks running_ and exits cleanly if stopped.
			if (!running_.load()) {
				break;
			}
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(clientMutex_);
			clientSocket_ = (uintptr_t)client;
		}
		HandleConnection((uintptr_t)client);
		{
			std::lock_guard<std::mutex> lock(clientMutex_);
			clientSocket_ = ~uintptr_t(0);
		}
		closesocket(client);
	}
}

void HttpServer::HandleConnection(uintptr_t clientSocket)
{
	const SOCKET sock = (SOCKET)clientSocket;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&kHeaderRecvTimeoutMs, sizeof(kHeaderRecvTimeoutMs));

	// 1) Read the header block until "\r\n\r\n", capping total size.
	std::string buffer;
	char temp[4096];
	size_t headerEnd = std::string::npos;
	while (true) {
		const int n = recv(sock, temp, (int)sizeof(temp), 0);
		if (n <= 0) {
			// Connection closed/error before a full header; nothing to answer.
			return;
		}
		buffer.append(temp, (size_t)n);
		headerEnd = buffer.find("\r\n\r\n");
		if (headerEnd != std::string::npos) {
			break;
		}
		if (buffer.size() > kMaxHeaderBytes) {
			WriteSimple(sock, 431, "{\"error\":\"header too large\"}");
			return;
		}
	}

	const std::string headerBlock = buffer.substr(0, headerEnd);
	std::string rest = buffer.substr(headerEnd + 4); // bytes already read past headers

	// 2) Parse the request line + headers.
	size_t lineEnd = headerBlock.find("\r\n");
	const std::string requestLine = lineEnd == std::string::npos ? headerBlock : headerBlock.substr(0, lineEnd);

	HttpRequest req;
	{
		const size_t sp1 = requestLine.find(' ');
		const size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
		if (sp1 == std::string::npos || sp2 == std::string::npos) {
			WriteSimple(sock, 400, "{\"error\":\"malformed request line\"}");
			return;
		}
		req.method = requestLine.substr(0, sp1);
		req.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
	}

	size_t contentLength = 0;
	bool hasContentLength = false;
	{
		size_t pos = lineEnd == std::string::npos ? std::string::npos : lineEnd + 2;
		while (pos != std::string::npos && pos < headerBlock.size()) {
			const size_t nextEnd = headerBlock.find("\r\n", pos);
			const std::string line = headerBlock.substr(
				pos, nextEnd == std::string::npos ? std::string::npos : nextEnd - pos);
			pos = nextEnd == std::string::npos ? std::string::npos : nextEnd + 2;
			if (line.empty()) {
				continue;
			}
			const size_t colon = line.find(':');
			if (colon == std::string::npos) {
				continue;
			}
			const std::string name = ToLower(Trim(line.substr(0, colon)));
			const std::string value = Trim(line.substr(colon + 1));
			if (name == "authorization") {
				req.authorization = value;
			} else if (name == "content-length") {
				try {
					const unsigned long parsed = std::stoul(value);
					contentLength = (size_t)parsed;
					hasContentLength = true;
				} catch (...) {
					WriteSimple(sock, 400, "{\"error\":\"bad content-length\"}");
					return;
				}
			}
		}
	}

	// 3) GET -> 405 (no SSE in the MVP). Anything not POST also 405.
	if (req.method != "POST") {
		WriteSimple(sock, 405, "{\"error\":\"method not allowed\"}");
		return;
	}

	// 4) POST body: read exactly Content-Length bytes (or empty if none).
	if (hasContentLength) {
		if (contentLength > kMaxBodyBytes) {
			WriteSimple(sock, 413, "{\"error\":\"payload too large\"}");
			return;
		}
		req.body = std::move(rest);
		while (req.body.size() < contentLength) {
			const int n = recv(sock, temp, (int)sizeof(temp), 0);
			if (n <= 0) {
				WriteSimple(sock, 400, "{\"error\":\"incomplete body\"}");
				return;
			}
			req.body.append(temp, (size_t)n);
			if (req.body.size() > kMaxBodyBytes) {
				WriteSimple(sock, 413, "{\"error\":\"payload too large\"}");
				return;
			}
		}
		if (req.body.size() > contentLength) {
			req.body.resize(contentLength);
		}
	}

	// 5) Dispatch to the handler; a handler exception must not kill the loop.
	HttpResponse resp;
	try {
		resp = handler_(req);
	} catch (const std::exception &e) {
		HostLog(std::string("[mcp] http handler threw: ") + e.what());
		WriteSimple(sock, 500, "{\"error\":\"internal error\"}");
		return;
	} catch (...) {
		HostLog("[mcp] http handler threw (unknown)");
		WriteSimple(sock, 500, "{\"error\":\"internal error\"}");
		return;
	}

	WriteResponse(sock, resp);
}

} // namespace Mcp
