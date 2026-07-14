#include "kick_pusher.hpp"

#include <nlohmann/json.hpp>

#include "util/http_client.hpp"

namespace Chat {

using json = nlohmann::json;

namespace {

// Bounded so a connect-time id fetch cannot stall teardown for long (the fetch is a
// blocking sync HTTP call that can't observe a cancel flag). Kept short so a
// detached worker lingers only briefly past Stop()/Shutdown.
constexpr int kChannelLookupTimeoutSec = 5;

// Extract an id field that Kick may serialize as a JSON integer or a string into a
// decimal string; returns "" when absent/unusable.
std::string IdToString(const json &j, const char *key)
{
	auto it = j.find(key);
	if (it == j.end()) {
		return std::string();
	}
	if (it->is_number_integer()) {
		return std::to_string(it->get<long long>());
	}
	if (it->is_string()) {
		return it->get<std::string>();
	}
	return std::string();
}

} // namespace

std::string KickPusherUrl()
{
	return std::string("wss://") + kKickPusherHost + "/app/" + kKickPusherAppKey +
	       "?protocol=7&client=js&version=" + kKickPusherClientVersion + "&flash=false";
}

bool ResolveKickChannelIds(const std::string &slug, std::string &chatroomIdOut, std::string &channelIdOut,
			   std::string &err)
{
	chatroomIdOut.clear();
	channelIdOut.clear();

	if (slug.empty()) {
		err = "Kick channel lookup: empty channel slug";
		return false;
	}

	Http::HttpReq req;
	req.method = "GET";
	req.url = "https://kick.com/api/v2/channels/" + Http::UrlEncode(slug);
	req.timeoutSec = kChannelLookupTimeoutSec;
	req.headers.push_back("Accept: application/json");
	req.headers.push_back(std::string("User-Agent: ") + kKickBrowserUserAgent);

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		err = "Kick channel lookup failed: " + resp.error;
		return false;
	}
	if (resp.status < 200 || resp.status >= 300) {
		err = "Kick channel lookup HTTP " + std::to_string(resp.status) +
		      " (unofficial /api/v2 endpoint may be gated)";
		return false;
	}

	const json j = json::parse(resp.body, nullptr, false);
	if (!j.is_object()) {
		err = "Kick channel lookup: malformed response";
		return false;
	}

	// chatroom.id -- required (chat + sub/gift/host events key off it).
	auto chat = j.find("chatroom");
	if (chat == j.end() || !chat->is_object()) {
		err = "Kick channel lookup: no chatroom in response";
		return false;
	}
	chatroomIdOut = IdToString(*chat, "id");
	if (chatroomIdOut.empty() || chatroomIdOut == "0") {
		err = "Kick channel lookup: invalid chatroom id";
		return false;
	}

	// Top-level id -- the numeric channel id (follower events). Best-effort: absence
	// is non-fatal (a caller wanting only the chatroom id still succeeds).
	channelIdOut = IdToString(j, "id");
	if (channelIdOut == "0") {
		channelIdOut.clear();
	}
	return true;
}

} // namespace Chat
