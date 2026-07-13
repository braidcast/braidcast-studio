#include "third_party_emotes.hpp"

#include <initializer_list>

#include <nlohmann/json.hpp>

#include "../http_client.hpp"
#include "../log.hpp"

namespace Chat {

namespace {

using EmoteMap = std::unordered_map<std::string, std::string>;

// Kept short (matching kick_chat.cpp's lookup) so that even a run of non-responsive
// providers only briefly delays the caller: the fetch can't observe the cancel flag
// mid-transfer, so a small per-request timeout bounds the worst-case teardown/next-
// connect latency. The whole build is best-effort, so a timeout just drops that set.
constexpr int kFetchTimeoutSec = 5;

// Defense-in-depth: only accept https emote URLs so a compromised/hostile provider
// body cannot inject an http/tracking origin into the dock's <img src>.
void Insert(EmoteMap &out, const std::string &code, const std::string &url)
{
	if (url.rfind("https://", 0) != 0) {
		return;
	}
	out[code] = url;
}

// GET `url` and parse the body as JSON, tolerating any failure. Returns a null json
// on transport error, non-2xx, or malformed JSON -- every caller treats that as "no
// emotes from this set" and moves on. `what` names the set for the log line.
json GetJson(const std::string &url, const char *what)
{
	Http::HttpReq req;
	req.method = "GET";
	req.url = url;
	req.timeoutSec = kFetchTimeoutSec;
	req.headers.push_back("Accept: application/json");

	const Http::HttpResponse resp = Http::HttpRequest(req);
	if (resp.status == 0) {
		HostLog(std::string("[chat] emotes: ") + what + " fetch failed: " + resp.error);
		return json(nullptr);
	}
	if (resp.status < 200 || resp.status >= 300) {
		HostLog(std::string("[chat] emotes: ") + what + " HTTP " + std::to_string(resp.status));
		return json(nullptr);
	}
	json j = json::parse(resp.body, nullptr, false);
	if (j.is_discarded()) {
		HostLog(std::string("[chat] emotes: ") + what + " malformed JSON");
		return json(nullptr);
	}
	return j;
}

// 7TV: an emote-set object with `.emotes[]`, each { name, data.host.url
// ("//cdn.7tv.app/emote/<id>"), data.host.files[] }. Prefer the 2x.webp rendition;
// fall back to the largest webp, then the first file, so an emote with an unusual
// file list still resolves. host.url is protocol-relative, so prefix https:.
void Parse7tv(const json &emoteSet, EmoteMap &out)
{
	if (!emoteSet.is_object()) {
		return;
	}
	auto emotesIt = emoteSet.find("emotes");
	if (emotesIt == emoteSet.end() || !emotesIt->is_array()) {
		return;
	}
	for (const json &e : *emotesIt) {
		if (!e.is_object()) {
			continue;
		}
		const std::string name = e.value("name", std::string());
		if (name.empty()) {
			continue;
		}
		auto dataIt = e.find("data");
		if (dataIt == e.end() || !dataIt->is_object()) {
			continue;
		}
		auto hostIt = dataIt->find("host");
		if (hostIt == dataIt->end() || !hostIt->is_object()) {
			continue;
		}
		const std::string baseUrl = hostIt->value("url", std::string());
		auto filesIt = hostIt->find("files");
		if (baseUrl.empty() || filesIt == hostIt->end() || !filesIt->is_array() || filesIt->empty()) {
			continue;
		}

		std::string chosen;
		std::string largestWebp; // files run 1x..4x per format, so the last webp seen is the largest
		for (const json &f : *filesIt) {
			if (!f.is_object()) {
				continue;
			}
			const std::string fname = f.value("name", std::string());
			if (fname == "2x.webp") {
				chosen = fname;
				break;
			}
			if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".webp") == 0) {
				largestWebp = fname;
			}
		}
		if (chosen.empty()) {
			chosen = !largestWebp.empty() ? largestWebp : filesIt->front().value("name", std::string());
		}
		if (chosen.empty()) {
			continue;
		}
		Insert(out, name, "https:" + baseUrl + "/" + chosen);
	}
}

// BetterTTV: an array of emotes, each { id, code }. The CDN serves per-size webp at
// https://cdn.betterttv.net/emote/<id>/2x.webp.
void ParseBttvArray(const json &arr, EmoteMap &out)
{
	if (!arr.is_array()) {
		return;
	}
	for (const json &e : arr) {
		if (!e.is_object()) {
			continue;
		}
		const std::string id = e.value("id", std::string());
		const std::string code = e.value("code", std::string());
		if (id.empty() || code.empty()) {
			continue;
		}
		Insert(out, code, "https://cdn.betterttv.net/emote/" + id + "/2x.webp");
	}
}

// FrankerFaceZ: a `.sets` map whose sets each hold `.emoticons[]`, each
// { name, urls{"1","2","4"} }. Prefer urls["2"], then "4", then "1". urls may be
// protocol-relative, so prefix https: when they start with "//".
void ParseFfz(const json &root, EmoteMap &out)
{
	if (!root.is_object()) {
		return;
	}
	auto setsIt = root.find("sets");
	if (setsIt == root.end() || !setsIt->is_object()) {
		return;
	}
	for (const auto &set : setsIt->items()) {
		if (!set.value().is_object()) {
			continue;
		}
		auto emIt = set.value().find("emoticons");
		if (emIt == set.value().end() || !emIt->is_array()) {
			continue;
		}
		for (const json &e : *emIt) {
			if (!e.is_object()) {
				continue;
			}
			const std::string name = e.value("name", std::string());
			if (name.empty()) {
				continue;
			}
			auto urlsIt = e.find("urls");
			if (urlsIt == e.end() || !urlsIt->is_object()) {
				continue;
			}
			std::string url;
			for (const char *size : {"2", "4", "1"}) {
				auto u = urlsIt->find(size);
				if (u != urlsIt->end() && u->is_string() && !u->get<std::string>().empty()) {
					url = u->get<std::string>();
					break;
				}
			}
			if (url.empty()) {
				continue;
			}
			if (url.rfind("//", 0) == 0) {
				url = "https:" + url;
			}
			Insert(out, name, url);
		}
	}
}

// Which provider sets each host offers. FFZ is Twitch-only; 7TV and BTTV key channel
// sets by /<slug>/<userId> where the slug is the host name. A null channel slug (or an
// empty userId at the call site) skips that provider's channel set. The fetch below
// walks these fields in fixed precedence order, so per-platform differences stay data.
struct PlatformEmoteConfig {
	bool ffzGlobal;
	bool bttvGlobal;
	bool sevenTvGlobal;
	bool ffzRoom;            // FFZ room set, keyed by login (Twitch only)
	const char *bttvSlug;    // BTTV channel-set provider slug; null -> skip
	const char *sevenTvSlug; // 7TV channel-set platform slug; null -> skip
};

// Indexed by EmotePlatform; entry order MUST match the enum (Twitch, Kick, YouTube).
const PlatformEmoteConfig &ConfigFor(EmotePlatform platform)
{
	static const PlatformEmoteConfig kConfigs[] = {
		{true, true, true, true, "twitch", "twitch"},     // Twitch
		{false, false, true, false, "kick", "kick"},      // Kick: 7TV global + 7TV/BTTV channel
		{false, true, true, false, "youtube", "youtube"}, // YouTube: 7TV+BTTV global + channel
	};
	return kConfigs[static_cast<size_t>(platform)];
}

} // namespace

std::unordered_map<std::string, std::string> FetchThirdPartyEmotes(EmotePlatform platform, const std::string &login,
								   const std::string &userId,
								   const std::function<bool()> &canceled)
{
	const PlatformEmoteConfig &cfg = ConfigFor(platform);
	EmoteMap out;

	// A GET can't observe the cancel flag mid-transfer, but polling before each one
	// lets a Stop() bail out of the remaining fetches promptly rather than paying the
	// full run of per-request timeouts. Returns whatever partial map is built so far.
	const auto stop = [&] {
		return canceled && canceled();
	};

	// Precedence: channel sets beat global sets, and within either tier 7TV > BTTV >
	// FFZ. A single map with last-write-wins encodes both rules by inserting in
	// ASCENDING priority -- all globals first (FFZ, then BTTV, then 7TV), then the
	// channel sets in the same provider order -- so a later write always belongs to a
	// higher-priority source and overwrites any earlier lower-priority code. Which of
	// those sets actually exist per platform comes from `cfg`.

	// --- Global tier (lowest priority) ---
	if (cfg.ffzGlobal) {
		if (stop()) {
			return out;
		}
		ParseFfz(GetJson("https://api.frankerfacez.com/v1/set/global", "FFZ global"), out);
	}
	if (cfg.bttvGlobal) {
		if (stop()) {
			return out;
		}
		ParseBttvArray(GetJson("https://api.betterttv.net/3/cached/emotes/global", "BTTV global"), out);
	}
	if (cfg.sevenTvGlobal) {
		if (stop()) {
			return out;
		}
		Parse7tv(GetJson("https://7tv.io/v3/emote-sets/global", "7TV global"), out);
	}

	// --- Channel tier (highest priority) ---
	// FFZ keys rooms by login; 7TV and BTTV key channel sets by channel id.
	if (cfg.ffzRoom && !login.empty()) {
		if (stop()) {
			return out;
		}
		ParseFfz(GetJson("https://api.frankerfacez.com/v1/room/" + Http::UrlEncode(login), "FFZ room"), out);
	}
	if (cfg.bttvSlug && !userId.empty()) {
		if (stop()) {
			return out;
		}
		const json bc = GetJson("https://api.betterttv.net/3/cached/users/" + std::string(cfg.bttvSlug) + "/" +
						Http::UrlEncode(userId),
					"BTTV channel");
		if (bc.is_object()) {
			auto ce = bc.find("channelEmotes");
			if (ce != bc.end()) {
				ParseBttvArray(*ce, out);
			}
			auto se = bc.find("sharedEmotes");
			if (se != bc.end()) {
				ParseBttvArray(*se, out);
			}
		}
	}
	if (cfg.sevenTvSlug && !userId.empty()) {
		if (stop()) {
			return out;
		}
		const json uc = GetJson("https://7tv.io/v3/users/" + std::string(cfg.sevenTvSlug) + "/" +
						Http::UrlEncode(userId),
					"7TV channel");
		if (uc.is_object()) {
			auto es = uc.find("emote_set");
			if (es != uc.end() && es->is_object()) {
				Parse7tv(*es, out);
			}
		}
	}

	return out;
}

json ApplyThirdPartyEmotes(const json &fragments, const std::unordered_map<std::string, std::string> &emotes)
{
	if (emotes.empty()) {
		return fragments;
	}
	json out = json::array();
	for (const json &frag : fragments) {
		if (frag.value("type", std::string()) != "text") {
			out.push_back(frag);
			continue;
		}
		const std::string text = frag.value("text", std::string());
		std::string pending; // accumulates non-emote words + all inter-word spacing
		size_t i = 0;
		const size_t n = text.size();
		while (i < n) {
			if (text[i] == ' ') {
				size_t j = i;
				while (j < n && text[j] == ' ') {
					++j;
				}
				pending.append(text, i, j - i);
				i = j;
				continue;
			}
			size_t j = i;
			while (j < n && text[j] != ' ') {
				++j;
			}
			const std::string word = text.substr(i, j - i);
			auto it = emotes.find(word);
			if (it != emotes.end()) {
				if (!pending.empty()) {
					out.push_back(json{{"type", "text"}, {"text", pending}});
					pending.clear();
				}
				out.push_back(json{{"type", "emote"}, {"code", word}, {"url", it->second}});
			} else {
				pending += word;
			}
			i = j;
		}
		if (!pending.empty()) {
			out.push_back(json{{"type", "text"}, {"text", pending}});
		}
	}
	return out;
}

} // namespace Chat
