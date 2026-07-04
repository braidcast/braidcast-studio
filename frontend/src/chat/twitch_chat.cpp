#include "twitch_chat.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../http_client.hpp"
#include "../log.hpp"
#include "../oauth/provider.hpp"
#include "../provider_creds.hpp"
#include "third_party_emotes.hpp"

namespace OAuth {

using json = nlohmann::json;

namespace {

const char *kTwitchIrcUrl = "wss://irc-ws.chat.twitch.tv:443";
// Twitch IRC emote image (v2 CDN). The IRC `emotes` tag gives id + positions but
// no name, so the URL is built straight from the id; default/dark/1.0 is the chat
// default rendition.
const char *kEmoteUrlPrefix = "https://static-cdn.jtvnw.net/emoticons/v2/";
const char *kEmoteUrlSuffix = "/default/dark/1.0";

std::string ToLower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// Strip CR/LF so a sent message cannot inject extra IRC commands.
std::string SanitizeOutbound(const std::string &text)
{
	std::string out = text;
	for (char &c : out) {
		if (c == '\r' || c == '\n') {
			c = ' ';
		}
	}
	return out;
}

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

bool ContainsCI(const std::string &haystack, const std::string &needleLower)
{
	std::string h = ToLower(haystack);
	return h.find(needleLower) != std::string::npos;
}

// Split a frame (which may carry several IRC lines) into individual lines,
// dropping the trailing CR and any empties.
std::vector<std::string> SplitLines(const std::string &frame)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= frame.size()) {
		size_t nl = frame.find('\n', start);
		std::string line = (nl == std::string::npos) ? frame.substr(start) : frame.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!line.empty()) {
			lines.push_back(std::move(line));
		}
		if (nl == std::string::npos) {
			break;
		}
		start = nl + 1;
	}
	return lines;
}

// Decode `s` into a vector of UTF-8 code-point substrings. Twitch emote tag
// positions index by code point (matching tmi.js's Array.from), so fragment
// splitting must operate on code points, not bytes, to stay correct for emoji.
std::vector<std::string> CodePoints(const std::string &s)
{
	std::vector<std::string> cps;
	size_t i = 0;
	const size_t n = s.size();
	while (i < n) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		if (c < 0x80) {
			len = 1;
		} else if ((c >> 5) == 0x6) {
			len = 2;
		} else if ((c >> 4) == 0xE) {
			len = 3;
		} else if ((c >> 3) == 0x1E) {
			len = 4;
		}
		if (i + len > n) {
			len = n - i;
		}
		cps.push_back(s.substr(i, len));
		i += len;
	}
	return cps;
}

// One parsed IRCv3 line: tags + the nick from the prefix + command + the first
// middle param + the trailing param.
struct IrcLine {
	std::map<std::string, std::string> tags;
	std::string nick;
	std::string command;
	std::string param0;
	std::string trailing;
};

IrcLine ParseIrc(const std::string &raw)
{
	IrcLine out;
	std::string line = raw;

	// @tags prefix.
	if (!line.empty() && line[0] == '@') {
		const size_t sp = line.find(' ');
		const std::string tagStr = line.substr(1, (sp == std::string::npos ? line.size() : sp) - 1);
		size_t p = 0;
		while (p < tagStr.size()) {
			const size_t semi = tagStr.find(';', p);
			const std::string kv = tagStr.substr(p, (semi == std::string::npos ? tagStr.size() : semi) - p);
			const size_t eq = kv.find('=');
			if (eq == std::string::npos) {
				out.tags[kv] = "";
			} else {
				out.tags[kv.substr(0, eq)] = kv.substr(eq + 1);
			}
			if (semi == std::string::npos) {
				break;
			}
			p = semi + 1;
		}
		line = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
	}

	// :prefix
	if (!line.empty() && line[0] == ':') {
		const size_t sp = line.find(' ');
		const std::string prefix = line.substr(1, (sp == std::string::npos ? line.size() : sp) - 1);
		out.nick = prefix.substr(0, prefix.find_first_of("!@"));
		line = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
	}

	// trailing param (first " :" delimits it).
	std::string middle = line;
	const size_t tp = line.find(" :");
	if (tp != std::string::npos) {
		middle = line.substr(0, tp);
		out.trailing = line.substr(tp + 2);
	} else if (!line.empty() && line[0] == ':') {
		out.trailing = line.substr(1);
		middle.clear();
	}

	// command + first middle param.
	size_t q = 0;
	const size_t firstSp = middle.find(' ');
	out.command = middle.substr(0, firstSp == std::string::npos ? middle.size() : firstSp);
	if (firstSp != std::string::npos) {
		q = firstSp + 1;
		const size_t secondSp = middle.find(' ', q);
		out.param0 = middle.substr(q, secondSp == std::string::npos ? std::string::npos : secondSp - q);
	}
	return out;
}

struct EmoteSpan {
	int start = 0;
	int end = 0; // inclusive
	std::string id;
};

// Parse the IRC `emotes` tag "25:0-4,12-16/1902:6-10" into spans.
std::vector<EmoteSpan> ParseEmotes(const std::string &tag)
{
	std::vector<EmoteSpan> spans;
	if (tag.empty()) {
		return spans;
	}
	size_t g = 0;
	while (g < tag.size()) {
		const size_t slash = tag.find('/', g);
		const std::string group = tag.substr(g, (slash == std::string::npos ? tag.size() : slash) - g);
		const size_t colon = group.find(':');
		if (colon != std::string::npos) {
			const std::string id = group.substr(0, colon);
			const std::string ranges = group.substr(colon + 1);
			size_t r = 0;
			while (r < ranges.size()) {
				const size_t comma = ranges.find(',', r);
				const std::string range =
					ranges.substr(r, (comma == std::string::npos ? ranges.size() : comma) - r);
				const size_t dash = range.find('-');
				if (dash != std::string::npos) {
					EmoteSpan s;
					s.id = id;
					s.start = std::atoi(range.substr(0, dash).c_str());
					s.end = std::atoi(range.substr(dash + 1).c_str());
					if (s.end >= s.start) {
						spans.push_back(std::move(s));
					}
				}
				if (comma == std::string::npos) {
					break;
				}
				r = comma + 1;
			}
		}
		if (slash == std::string::npos) {
			break;
		}
		g = slash + 1;
	}
	std::sort(spans.begin(), spans.end(), [](const EmoteSpan &a, const EmoteSpan &b) { return a.start < b.start; });
	return spans;
}

std::string JoinCps(const std::vector<std::string> &cps, int from, int to)
{
	std::string out;
	for (int i = from; i <= to && i < static_cast<int>(cps.size()); ++i) {
		out += cps[i];
	}
	return out;
}

// Split the message text into text/emote fragments using the emote spans.
json BuildFragments(const std::string &message, const std::vector<EmoteSpan> &spans)
{
	json frags = json::array();
	if (spans.empty()) {
		if (!message.empty()) {
			frags.push_back(json{{"type", "text"}, {"text", message}});
		}
		return frags;
	}

	const std::vector<std::string> cps = CodePoints(message);
	const int n = static_cast<int>(cps.size());
	int cursor = 0;
	for (const EmoteSpan &s : spans) {
		if (s.start < 0 || s.start >= n || s.end >= n || s.start < cursor) {
			continue; // malformed/overlapping span -- skip defensively
		}
		if (s.start > cursor) {
			frags.push_back(json{{"type", "text"}, {"text", JoinCps(cps, cursor, s.start - 1)}});
		}
		frags.push_back(json{{"type", "emote"},
				     {"code", JoinCps(cps, s.start, s.end)},
				     {"url", std::string(kEmoteUrlPrefix) + s.id + kEmoteUrlSuffix}});
		cursor = s.end + 1;
	}
	if (cursor < n) {
		frags.push_back(json{{"type", "text"}, {"text", JoinCps(cps, cursor, n - 1)}});
	}
	return frags;
}

// Resolve a Twitch login to its numeric user id via Helix. Best-effort: any failure
// returns "" so the caller still fetches global + FFZ-by-login sets and merely skips
// the id-keyed (7TV/BTTV) channel sets. Blocking; runs on the chat worker thread.
std::string ResolveTwitchUserId(const std::string &login, const std::string &token,
				const std::function<bool()> &canceled)
{
	if (login.empty() || token.empty() || (canceled && canceled())) {
		return std::string();
	}
	Http::HttpReq req;
	req.method = "GET";
	req.url = "https://api.twitch.tv/helix/users?login=" + Http::UrlEncode(login);
	req.timeoutSec = 8;
	req.headers.push_back("Authorization: Bearer " + token);
	req.headers.push_back("Client-Id: " + TwitchClientId());

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status < 200 || resp.status >= 300) {
		HostLog("[chat] twitch: user-id lookup HTTP " + std::to_string(resp.status));
		return std::string();
	}
	const json j = json::parse(resp.body, nullptr, false);
	if (!j.is_object()) {
		return std::string();
	}
	auto data = j.find("data");
	if (data == j.end() || !data->is_array() || data->empty()) {
		return std::string();
	}
	const json &row = data->front();
	if (!row.is_object()) {
		return std::string();
	}
	auto id = row.find("id");
	if (id == row.end() || !id->is_string()) {
		return std::string();
	}
	return id->get<std::string>();
}

json BuildBadges(const std::string &badgeTag)
{
	json arr = json::array();
	if (badgeTag.empty()) {
		return arr;
	}
	size_t p = 0;
	while (p < badgeTag.size()) {
		const size_t comma = badgeTag.find(',', p);
		const std::string entry = badgeTag.substr(p, (comma == std::string::npos ? badgeTag.size() : comma) - p);
		const std::string kind = entry.substr(0, entry.find('/'));
		if (!kind.empty()) {
			// Twitch badge images need the Get Chat Badges API to map set-id ->
			// image UUID; that is not cheap inside the read loop, so emit the kind
			// without a url rather than block. (Spec: don't block on badge URLs.)
			arr.push_back(json{{"kind", kind}});
		}
		if (comma == std::string::npos) {
			break;
		}
		p = comma + 1;
	}
	return arr;
}

} // namespace

bool TwitchChat::sendLine(const std::string &line)
{
	std::lock_guard<std::mutex> lock(wsMutex_);
	if (!ws_.connected()) {
		return false;
	}
	return ws_.sendText(line);
}

bool TwitchChat::connect(const Chat::ChatContext &ctx, OAuthAccount &acct, const std::string &channelRef,
			 std::string &err)
{
	// Serialize against an overlapping re-Start: the hub does not join old workers,
	// so a prior connect() might still be unwinding -- block until it has fully
	// released the socket before this generation touches ws_.
	std::lock_guard<std::mutex> run(runMutex_);

	stopped_.store(false);
	ready_.store(false);

	const std::string channel = ToLower(channelRef);
	const std::string nick = ToLower(acct.login.empty() ? channelRef : acct.login);
	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		channel_ = channel;
	}

	const auto canceled = [&] { return stopped_.load() || (ctx.canceled && ctx.canceled()); };

	// Build the third-party emote map ONCE, before the (re)connect loop starts. This
	// runs on the chat worker thread and the read loop only READS the map on that same
	// thread, so no lock is needed (see the member's comment). Each fetch is
	// best-effort; a failure just yields fewer emotes. The blocking HTTP can't observe
	// the cancel flag mid-transfer, so it holds runMutex_ until the in-flight GET
	// returns -- which would delay the NEXT generation's connect(). To bound that, the
	// fetch polls `canceled` before each GET (and each GET has a short timeout), so a
	// Stop() abandons the rest and returns a partial map promptly.
	thirdPartyEmotes_ = Chat::FetchThirdPartyEmotes(Chat::EmotePlatform::Twitch, channel,
							ResolveTwitchUserId(channel, acct.access, canceled), canceled);

	Chat::Backoff backoff;
	int reauthAttempts = 0;

	while (!canceled()) {
		// (Re)open the socket.
		{
			std::lock_guard<std::mutex> lock(wsMutex_);
			std::string cerr;
			if (!ws_.connect(kTwitchIrcUrl, cerr)) {
				err = cerr;
			}
		}
		if (!ws_.connected()) {
			ctx.emit(json{{"event", "chat.state"},
				      {"platform", "twitch"},
				      {"connected", false},
				      {"error", err}});
			if (Chat::CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		// Handshake. CAP first (so tags/commands apply to the welcome burst), then
		// PASS / NICK to authenticate, then JOIN the channel.
		const bool handshakeOk =
			sendLine("CAP REQ :twitch.tv/membership twitch.tv/tags twitch.tv/commands\r\n") &&
			sendLine("PASS oauth:" + acct.access + "\r\n") && sendLine("NICK " + nick + "\r\n") &&
			sendLine("JOIN #" + channel + "\r\n");
		if (!handshakeOk) {
			ready_.store(false);
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ws_.close();
			}
			if (canceled()) {
				break;
			}
			if (Chat::CancelableSleep(backoff.next(), canceled)) {
				break;
			}
			continue;
		}

		// Read loop. recv() polls with a ~250ms timeout, so canceled() is re-checked
		// frequently and a Stop() returns within that window.
		bool authFailed = false;
		while (!canceled()) {
			std::string frame;
			bool isText = false;
			std::string rerr;
			bool ok;
			{
				std::lock_guard<std::mutex> lock(wsMutex_);
				ok = ws_.recv(frame, isText, rerr);
			}
			if (!ok) {
				err = rerr;
				break; // peer closed / transport error -> reconnect
			}
			if (frame.empty() || !isText) {
				continue; // poll timeout, auto-PONGed ping, or partial chunk
			}

			for (const std::string &line : SplitLines(frame)) {
				const IrcLine m = ParseIrc(line);

				if (m.command == "PING") {
					sendLine("PONG :" + m.trailing + "\r\n");
					continue;
				}
				if (m.command == "001") {
					ready_.store(true);
					backoff.reset();
					// A successful connect proves the (refreshed) token worked, so
					// re-arm the reauth budget: each genuine ~4h token expiry then
					// gets its one force-refresh retry, while a no-connect auth-fail
					// loop (never reaching 001) stays bounded.
					reauthAttempts = 0;
					ctx.emit(json{{"event", "chat.state"},
						      {"platform", "twitch"},
						      {"connected", true}});
					continue;
				}
				if (m.command == "NOTICE" &&
				    (ContainsCI(m.trailing, "authentication failed") ||
				     ContainsCI(m.trailing, "login unsuccessful") ||
				     ContainsCI(m.trailing, "login authentication"))) {
					authFailed = true;
					err = "Twitch chat login failed: " + m.trailing;
					break;
				}
				if (m.command == "PRIVMSG") {
					const auto tag = [&](const char *k) -> std::string {
						auto it = m.tags.find(k);
						return it == m.tags.end() ? std::string() : it->second;
					};
					std::string name = tag("display-name");
					if (name.empty()) {
						name = m.nick;
					}
					int64_t ts = NowMs();
					const std::string tsTag = tag("tmi-sent-ts");
					if (!tsTag.empty()) {
						ts = std::strtoll(tsTag.c_str(), nullptr, 10);
					}
					ctx.emit(json{
						{"event", "chat.message"},
						{"platform", "twitch"},
						{"channelId", channel},
						{"id", tag("id")},
						{"ts", ts},
						{"author", json{{"name", name},
								{"color", tag("color")},
								{"badges", BuildBadges(tag("badges"))}}},
						{"fragments",
						 Chat::ApplyThirdPartyEmotes(
							 BuildFragments(m.trailing, ParseEmotes(tag("emotes"))),
							 thirdPartyEmotes_)},
					});
				}
			}
			if (authFailed) {
				break;
			}
		}

		ready_.store(false);
		{
			std::lock_guard<std::mutex> lock(wsMutex_);
			ws_.close();
		}
		if (canceled()) {
			break;
		}

		if (authFailed) {
			// Reactive token issue. Twitch refresh tokens don't rotate, so one
			// force-refresh + reconnect (re-PASS) suffices; never loop on it.
			if (reauthAttempts >= 1 || !auth_) {
				ctx.emit(json{{"event", "chat.state"},
					      {"platform", "twitch"},
					      {"connected", false},
					      {"error", "re-authentication required"}});
				err = "Twitch chat: re-authentication required";
				break;
			}
			++reauthAttempts;
			std::string refreshErr;
			if (!auth_->ensureFresh(acct, refreshErr, /*force=*/true)) {
				ctx.emit(json{{"event", "chat.state"},
					      {"platform", "twitch"},
					      {"connected", false},
					      {"error", "re-authentication required"}});
				err = "Twitch chat: re-authentication required";
				break;
			}
			continue; // reconnect immediately with the refreshed token
		}

		// Unexpected drop: report disconnected and back off before reconnecting.
		ctx.emit(json{{"event", "chat.state"}, {"platform", "twitch"}, {"connected", false}, {"error", err}});
		if (Chat::CancelableSleep(backoff.next(), canceled)) {
			break;
		}
	}

	ready_.store(false);
	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		ws_.close();
	}
	if (canceled()) {
		err.clear(); // clean cancel: hub suppresses the log
	}
	return false;
}

bool TwitchChat::send(OAuthAccount &acct, const std::string &text, std::string &err)
{
	(void)acct; // IRC sends over the already-authenticated socket; no per-send token
	if (!ready_.load()) {
		err = "Twitch chat not connected";
		return false;
	}
	std::string channel;
	{
		std::lock_guard<std::mutex> lock(wsMutex_);
		channel = channel_;
	}
	const std::string line = "PRIVMSG #" + channel + " :" + SanitizeOutbound(text) + "\r\n";
	if (!sendLine(line)) {
		err = "Twitch chat send failed";
		return false;
	}
	return true;
}

void TwitchChat::disconnect()
{
	// Only flip the flag: the worker that owns ws_ tears the socket down on its own
	// thread (curl handles are not safe to touch from here). recv()'s ~250ms poll
	// makes the loop notice promptly.
	stopped_.store(true);
}

} // namespace OAuth
