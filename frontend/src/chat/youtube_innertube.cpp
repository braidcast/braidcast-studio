#include "youtube_innertube.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>

#include "../log.hpp"
#include "chat_transport.hpp" // BuildChatMessage -- the shared normalized-frame assembler
#include "third_party_emotes.hpp"
#include "util/innertube_client.hpp"
#include "util/json_util.hpp"
#include "ws_client.hpp" // CancelableSleep / Backoff

namespace Chat {

namespace YouTubeInnerTube {

namespace {

using JsonUtil::Bool;
using JsonUtil::NumLoose;
using JsonUtil::Obj;
using JsonUtil::Str;

const char *kNextUrl = "https://www.youtube.com/youtubei/v1/next";
const char *kGetLiveChatUrl = "https://www.youtube.com/youtubei/v1/live_chat/get_live_chat";

// Poll cadence. The response advertises a timeoutMs (measured consistently at 10000), but it
// is an UPPER BOUND to respect while the chat is quiet, not the cadence to read at: obeying
// it costs 0-10s of latency on every message, which is worse than the quota-billed read this
// replaces. So poll near the floor while messages flow and step down only while nothing
// arrives.
//
// The 2s floor is deliberately NOT as fast as the endpoint tolerates. Four destinations at 1s
// is four requests a second sustained from one address, an order of magnitude above what the
// reference web client does when it obeys the advised timeout; 2s halves that signature and
// costs ~0.7s of median latency, which chat does not notice. Durability over the last 0.7s.
constexpr long kPollActiveMs = 2000;
constexpr long kPollIdleMs = 4000;
constexpr long kPollDeepIdleMs = 8000;
constexpr int kIdleAfterEmpty = 5;
constexpr int kDeepIdleAfterEmpty = 15;

// Every wait is jittered by this fraction so concurrent readers desynchronize. Readers
// polling in lockstep on a round number is a machine signature no set of ordinary clients
// produces.
constexpr double kJitterFraction = 0.15;

// Applied AFTER both the advisory cap and the jitter, so no advised value and no arithmetic
// slip can turn this into a hot loop. The official streamList loop needed the same guard for
// the same reason -- its unconditional backoff reset on the 2xx path could otherwise resume
// on a 250ms floor and spend a day's quota in two minutes.
constexpr long kPollFloorMs = 1000;

// Consecutive unusable responses (non-2xx, or 2xx carrying no liveChatContinuation at all)
// before handing chat to the caller's official read. Deliberately NOT a terminal verdict:
// this says "the reverse-engineered surface stopped working here", which the quota-billed
// API can still answer authoritatively. An ENDED chat is a different signal, detected
// separately below, and stops rather than handing over.
constexpr int kDeadStrikes = 3;

// Continuation-resolve attempts before handing over. One blip at go-live must not park a
// 24h broadcast on the quota-billed read for its whole duration.
constexpr int kBootstrapAttempts = 3;

// Bounded id memory: a filter switch or a reload continuation re-delivers recent items, and
// a day-long broadcast must not accumulate every id it ever saw.
constexpr size_t kSeenCap = 512;

// Index 1 of the view selector is "Live chat". Index 0 is "Top chat", which is
// SERVER-FILTERED and silently drops messages -- reading it looks like a working chat that
// merely misses lines, which is the worst available failure.
constexpr size_t kLiveChatFilterIndex = 1;

// Bounded FIFO id memory. Dedupe is by item id so a filter switch or a reload continuation
// cannot double-post, and eviction is oldest-first so the set cannot grow with uptime.
class SeenIds {
public:
	// True when `id` is new (and is now remembered); false when it was already seen.
	bool add(const std::string &id)
	{
		if (id.empty()) {
			return true; // nothing to key on -- pass it through rather than dropping it
		}
		if (!ids_.insert(id).second) {
			return false;
		}
		order_.push_back(id);
		if (order_.size() > kSeenCap) {
			ids_.erase(order_.front());
			order_.pop_front();
		}
		return true;
	}

private:
	std::unordered_set<std::string> ids_;
	std::deque<std::string> order_;
};

// The largest thumbnail URL from a `{ "thumbnails": [ { "url": ... } ] }` node ("" when
// absent). InnerTube orders thumbnails smallest-first. Some renderers hand back
// protocol-relative URLs, which an app:// document cannot resolve, so those are completed.
std::string LargestThumbnail(const json &node)
{
	const json &thumbs = Obj(node, "thumbnails");
	if (!thumbs.is_array()) {
		return std::string();
	}
	for (size_t i = thumbs.size(); i > 0; --i) {
		const std::string url = Str(thumbs[i - 1], "url");
		if (url.empty()) {
			continue;
		}
		return url.rfind("//", 0) == 0 ? "https:" + url : url;
	}
	return std::string();
}

// An InnerTube text node renders either as `simpleText` or as `runs[]`; read both, so a
// server-side switch between the two forms cannot silently blank a field.
std::string PlainText(const json &node)
{
	const std::string simple = Str(node, "simpleText");
	if (!simple.empty()) {
		return simple;
	}
	std::string out;
	const json &runs = Obj(node, "runs");
	if (runs.is_array()) {
		for (const json &run : runs) {
			out += Str(run, "text");
		}
	}
	return out;
}

// The first element of a string array ("" when absent or not an array of strings).
std::string FirstString(const json &array)
{
	if (!array.is_array() || array.empty() || !array[0].is_string()) {
		return std::string();
	}
	return array[0].get<std::string>();
}

// The first purely-numeric run of a runs[] node ("Gifted 5 memberships" -> 5), 0 when there
// is none. Locale-independent by construction: it reads a digit run rather than matching
// any wording around it.
int FirstIntegerInRuns(const json &runs)
{
	if (!runs.is_array()) {
		return 0;
	}
	for (const json &run : runs) {
		const std::string text = Str(run, "text");
		if (text.empty() || text.size() > 9) {
			continue;
		}
		if (std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
			return std::stoi(text);
		}
	}
	return 0;
}

// message.runs[] -> normalized fragments. An emoji run carries image thumbnails the official
// Data API response has no equivalent of (its displayMessage is one flat string), which is
// the main content win of this read path -- so a CUSTOM (channel) emoji becomes an emote
// fragment with its image URL. A standard emoji arrives as literal unicode in `emojiId` and
// stays TEXT, so the pane renders it natively instead of loading an image per smiley.
json FragmentsFromRuns(const json &runs)
{
	json fragments = json::array();
	if (!runs.is_array()) {
		return fragments;
	}
	for (const json &run : runs) {
		const std::string text = Str(run, "text");
		if (!text.empty()) {
			fragments.push_back(json{{"type", "text"}, {"text", text}});
			continue;
		}
		const json &emoji = Obj(run, "emoji");
		if (!emoji.is_object()) {
			continue; // an unknown run kind contributes nothing rather than breaking the line
		}
		const bool custom = Bool(emoji, "isCustomEmoji");
		// A custom emoji's emojiId is an opaque channel-scoped hash, so its shortcut
		// (":_name:") -- what the author actually typed -- is the code that belongs on the
		// wire, with searchTerms as the documented second choice. A standard emoji's emojiId
		// IS the character.
		const std::string shortcut = FirstString(Obj(emoji, "shortcuts"));
		std::string code = custom ? shortcut : Str(emoji, "emojiId");
		if (code.empty()) {
			code = custom ? FirstString(Obj(emoji, "searchTerms")) : shortcut;
		}
		if (code.empty()) {
			code = Str(emoji, "emojiId");
		}
		const std::string url = LargestThumbnail(Obj(emoji, "image"));
		if (custom && !url.empty()) {
			fragments.push_back(json{{"type", "emote"}, {"code", code}, {"url", url}});
		} else if (!code.empty()) {
			fragments.push_back(json{{"type", "text"}, {"text", code}});
		}
	}
	return fragments;
}

// icon.iconType -> the badge `kind` vocabulary the official read already emits. Order is
// irrelevant here (one iconType per badge); the table exists so a new built-in badge is one
// row. VERIFIED and any other iconType is skipped rather than invented into a kind the pane
// has no rendering for.
const std::pair<const char *, const char *> kBadgeIcons[] = {
	{"OWNER", "broadcaster"},
	{"MODERATOR", "moderator"},
};

// authorBadges[] -> the SAME kind vocabulary as the official read (broadcaster/moderator/
// member), plus the badge image URL that read cannot supply at all.
//
// A membership badge is identified by its customThumbnail, NOT by an iconType, and that test
// comes FIRST: the built-in-icon table is only consulted for a badge that has no thumbnail.
// The badge's `tooltip` ("Member (6 months)") is the tenure label, which the normalized wire
// shape has nowhere to carry, so it is read past rather than dropped by accident.
json BadgesFromAuthorBadges(const json &authorBadges)
{
	json badges = json::array();
	if (!authorBadges.is_array()) {
		return badges;
	}
	for (const json &entry : authorBadges) {
		const json &renderer = Obj(entry, "liveChatAuthorBadgeRenderer");
		if (!renderer.is_object()) {
			continue;
		}
		const std::string url = LargestThumbnail(Obj(renderer, "customThumbnail"));
		if (!url.empty()) {
			badges.push_back(json{{"kind", "member"}, {"url", url}});
			continue;
		}
		const std::string iconType = Str(Obj(renderer, "icon"), "iconType");
		for (const auto &icon : kBadgeIcons) {
			if (iconType == icon.first) {
				badges.push_back(json{{"kind", icon.second}});
				break;
			}
		}
	}
	return badges;
}

// purchaseAmountText is a DISPLAY string ("$5.00", "₹450.00", "¥1,000", "PHP 100.00"), but
// the Super Chat dedupe id is derived from amount MICROS (Events::YouTubeMoneyEventId) and
// the official superChatEvents.list surface derives its own from the same quantity. If the
// two disagree, one purchase lands twice. So the display string is parsed back into micros
// of the MAJOR unit -- exactly what the Data API's amountMicros carries for the same
// purchase.
struct ParsedAmount {
	int64_t micros = 0;
	std::string currency;
	bool ok = false;
};

// Symbol -> ISO 4217, LONGEST PREFIX FIRST: "CA$"/"MX$"/"A$" must win over "$" and "CN¥"
// over "¥", because the substring fallback below takes the first hit. The code is
// display-only (the dedupe id keys on micros), so an unmapped symbol degrades to an empty
// currency rather than a wrong event -- deliberately not defaulted to USD, which would
// mislabel every unmapped currency as dollars.
const std::pair<const char *, const char *> kCurrencySymbols[] = {
	{"CA$", "CAD"},  {"MX$", "MXN"},  {"NZ$", "NZD"}, {"HK$", "HKD"}, {"NT$", "TWD"}, {"CN¥", "CNY"},
	{"RMB¥", "CNY"}, {"US$", "USD"},  {"GB£", "GBP"}, {"JP¥", "JPY"}, {"COL$", "COP"}, {"Mex$", "MXN"},
	{"AU$", "AUD"},  {"AR$", "ARS"},  {"CL$", "CLP"}, {"RD$", "DOP"}, {"TT$", "TTD"},  {"A$", "AUD"},
	{"R$", "BRL"},   {"S$", "SGD"},   {"C$", "CAD"},  {"N$", "NAD"},  {"kr.", "DKK"},  {"Kč", "CZK"},
	{"zł", "PLN"},   {"Ft", "HUF"},   {"Rp", "IDR"},  {"RM", "MYR"},  {"Rs", "INR"},   {"Ksh", "KES"},
	{"₹", "INR"},    {"₩", "KRW"},    {"₱", "PHP"},   {"₪", "ILS"},   {"₫", "VND"},    {"₺", "TRY"},
	{"₦", "NGN"},    {"₡", "CRC"},    {"₲", "PYG"},   {"₴", "UAH"},   {"€", "EUR"},    {"£", "GBP"},
	{"¥", "JPY"},    {"kr", "SEK"},   {"$", "USD"},
};

bool IsAsciiDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool IsAsciiUpper(char c)
{
	return c >= 'A' && c <= 'Z';
}

// The one embedded 3-letter ISO code in `text`, "" when there is none or more than a single
// candidate run. Catches every code-prefixed form ("PHP 100.00", "SEK 20.00", "CHF 5.00")
// generically instead of enumerating them in the table above.
std::string EmbeddedIsoCode(const std::string &text)
{
	std::string found;
	size_t run = 0;
	for (size_t i = 0; i <= text.size(); ++i) {
		if (i < text.size() && IsAsciiUpper(text[i])) {
			++run;
			continue;
		}
		if (run == 3) {
			if (!found.empty()) {
				return std::string(); // ambiguous -- refuse rather than guess
			}
			found = text.substr(i - 3, 3);
		}
		run = 0;
	}
	return found;
}

ParsedAmount ParseAmountText(const std::string &text)
{
	ParsedAmount out;
	std::string digits;  // digits and separators, in order
	std::string residue; // the currency symbol or code, whitespace stripped
	// The two non-ASCII spaces YouTube separates a currency token from its amount with:
	// U+00A0 NO-BREAK SPACE and U+202F NARROW NO-BREAK SPACE. They must be stripped
	// EXPLICITLY -- left in `residue` their bytes sit between the symbol's characters and
	// break the "CA$"-before-"$" match, silently relabelling Canadian dollars as US ones.
	static const char *kUnicodeSpaces[] = {"\xC2\xA0", "\xE2\x80\xAF"};
	for (size_t i = 0; i < text.size();) {
		bool skipped = false;
		for (const char *space : kUnicodeSpaces) {
			const size_t len = std::string(space).size();
			if (text.compare(i, len, space) == 0) {
				i += len;
				skipped = true;
				break;
			}
		}
		if (skipped) {
			continue;
		}
		const char c = text[i++];
		if (static_cast<unsigned char>(c) <= 0x20) {
			continue;
		}
		if (IsAsciiDigit(c) || c == '.' || c == ',') {
			digits.push_back(c);
		} else {
			residue.push_back(c);
		}
	}
	if (digits.empty()) {
		return out;
	}

	// Separator disambiguation without locale knowledge: a separator followed by EXACTLY two
	// trailing digits is the decimal point, so "1,000.50" and "1.000,50" both read as
	// 1000.50; anything else is a group separator, so "¥1,600" reads as 1600 and "Rp10.000"
	// as 10000. The one shape this gets wrong is a three-decimal currency (KWD/BHD 1.500),
	// where the fraction reads as a group -- rare enough, and it costs the cross-surface
	// dedupe of that one purchase rather than the event itself.
	size_t decimalPos = std::string::npos;
	const size_t lastSep = digits.find_last_of(".,");
	if (lastSep != std::string::npos && digits.size() - lastSep - 1 == 2) {
		decimalPos = lastSep;
	}
	int64_t whole = 0;
	int64_t frac = 0;
	for (size_t i = 0; i < digits.size(); ++i) {
		if (!IsAsciiDigit(digits[i])) {
			continue;
		}
		int64_t &target = (decimalPos != std::string::npos && i > decimalPos) ? frac : whole;
		if (target > 1000000000000LL) {
			return out; // absurd magnitude: refuse rather than wrap into a wrong dedupe key
		}
		target = target * 10 + (digits[i] - '0');
	}
	// Micros of the MAJOR unit, matching amountMicros for the same purchase. `frac` is
	// exactly two digits whenever decimalPos was found, so it scales by 10^4.
	out.micros = whole * 1000000 + frac * 10000;

	// Currency, most specific first: an exact table hit, then an embedded ISO code
	// ("PHP 100.00"), then the loosest substring match for a compound symbol the table only
	// knows part of.
	for (const auto &symbol : kCurrencySymbols) {
		if (residue == symbol.first) {
			out.currency = symbol.second;
			break;
		}
	}
	if (out.currency.empty()) {
		out.currency = EmbeddedIsoCode(residue);
	}
	if (out.currency.empty()) {
		for (const auto &symbol : kCurrencySymbols) {
			if (residue.find(symbol.first) != std::string::npos) {
				out.currency = symbol.second;
				break;
			}
		}
	}
	out.ok = out.micros > 0;
	return out;
}

// The fields every chat-item renderer carries in the same place.
struct ItemCommon {
	std::string id;
	int64_t tsMs = 0;
	std::string authorName;
	std::string authorChannelId;
	json badges = json::array();
};

ItemCommon ReadCommon(const json &renderer)
{
	ItemCommon common;
	common.id = Str(renderer, "id");
	// timestampUsec is MICROseconds since epoch, serialized as a numeric string.
	common.tsMs = NumLoose(renderer, "timestampUsec") / 1000;
	common.authorName = PlainText(Obj(renderer, "authorName"));
	common.authorChannelId = Str(renderer, "authorExternalChannelId");
	common.badges = BadgesFromAuthorBadges(Obj(renderer, "authorBadges"));
	return common;
}

// One renderer kind decoded into (a) the chat line's fragments and (b) optionally the
// monetization/membership event that item ALSO produces (`hasEvent` stays false for plain
// chat, and an event never suppresses the chat line). `common` is mutable because the gift
// renderers keep the author on a nested header rather than on the item itself. A builder
// never emits: the caller owns the wire shape and the emit order.
using RendererFn = void (*)(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
			    bool &hasEvent);

// The shared tail of both paid renderers: the content-derived dedupe id that collapses a
// purchase seen by BOTH this read and the REST superChatEvents.list poll (the two surfaces
// assign the same purchase different resource ids). Falls back to the item-keyed form when
// the supporter channel is unknown or the display amount did not parse -- that item then
// will not cross-path-dedupe, the same accepted edge the official path takes.
void FillMoneyEvent(const char *type, const json &renderer, const ItemCommon &common, Events::NormalizedEvent &ev)
{
	const ParsedAmount amount = ParseAmountText(PlainText(Obj(renderer, "purchaseAmountText")));
	ev.type = type;
	ev.id = (common.authorChannelId.empty() || !amount.ok)
			? (std::string("youtube:") + type + ":" + common.id)
			: Events::YouTubeMoneyEventId(type, common.authorChannelId, amount.micros,
						      common.tsMs / 1000);
	ev.amount = amount.micros / 10000; // micros -> minor units, as the official read stores
	ev.currency = amount.currency;
}

void BuildTextMessage(const json &renderer, ItemCommon &, json &fragments, Events::NormalizedEvent &, bool &)
{
	fragments = FragmentsFromRuns(Obj(Obj(renderer, "message"), "runs"));
}

void BuildPaidMessage(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
		      bool &hasEvent)
{
	const std::string comment = PlainText(Obj(renderer, "message"));
	fragments = FragmentsFromRuns(Obj(Obj(renderer, "message"), "runs"));
	if (fragments.empty()) {
		// A Super Chat with no comment still belongs in the chat feed: the amount is its
		// whole content, and the official read's line carries the same.
		fragments.push_back(json{{"type", "text"}, {"text", PlainText(Obj(renderer, "purchaseAmountText"))}});
	}
	FillMoneyEvent("superchat", renderer, common, ev);
	ev.message = comment;
	hasEvent = true;
}

void BuildPaidSticker(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
		      bool &hasEvent)
{
	// A sticker carries no message runs: its content is the amount plus the sticker image,
	// and that image is a real emote fragment the official read cannot supply.
	fragments = json::array();
	const std::string amountText = PlainText(Obj(renderer, "purchaseAmountText"));
	if (!amountText.empty()) {
		fragments.push_back(json{{"type", "text"}, {"text", amountText}});
	}
	const std::string url = LargestThumbnail(Obj(renderer, "sticker"));
	if (!url.empty()) {
		fragments.push_back(json{{"type", "emote"}, {"code", "[sticker]"}, {"url", url}});
	}
	FillMoneyEvent("supersticker", renderer, common, ev);
	hasEvent = true;
}

// The membership tier out of headerSubtext. YouTube ships no tier field on this renderer, but
// it splits the welcome line into exactly three runs -- ["Welcome to ", "<tier>", "!"] -- so
// the middle run IS the tier. Read structurally rather than by matching the wording, which
// would only work in English. Any other shape falls back to the whole line, which is at
// worst a verbose tier label rather than a wrong one.
std::string TierFromHeaderSubtext(const json &subtext, const std::string &plain)
{
	const json &runs = Obj(subtext, "runs");
	if (runs.is_array() && runs.size() == 3) {
		const std::string middle = Str(runs[1], "text");
		if (!middle.empty()) {
			return middle;
		}
	}
	return plain;
}

void BuildMembership(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
		     bool &hasEvent)
{
	// A new member's line is the header ("Welcome to <tier>!"); a milestone carries the
	// member's own message runs alongside it.
	fragments = FragmentsFromRuns(Obj(Obj(renderer, "message"), "runs"));
	const json &subtext = Obj(renderer, "headerSubtext");
	const std::string subtextPlain = PlainText(subtext);
	if (fragments.empty()) {
		const std::string header = PlainText(Obj(renderer, "headerPrimaryText")) + subtextPlain;
		if (!header.empty()) {
			fragments.push_back(json{{"type", "text"}, {"text", header}});
		}
	}
	ev.type = "member";
	ev.tier = TierFromHeaderSubtext(subtext, subtextPlain);
	// Membership ids stay keyed on the item id: no other surface delivers the same record,
	// so there is nothing to cross-dedupe against.
	ev.id = "youtube:member:" + common.id;
	ev.message = PlainText(Obj(renderer, "message"));
	hasEvent = true;
}

void BuildGiftPurchase(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
		       bool &hasEvent)
{
	// "<name> gifted N memberships" lives on a NESTED header renderer, and so does the
	// gifter's own name/badges -- the announcement itself carries only the id, timestamp and
	// channel id.
	const json &header = Obj(Obj(renderer, "header"), "liveChatSponsorshipsHeaderRenderer");
	const json &primary = Obj(Obj(header, "primaryText"), "runs");
	fragments = FragmentsFromRuns(primary);
	if (common.authorName.empty()) {
		common.authorName = PlainText(Obj(header, "authorName"));
		common.badges = BadgesFromAuthorBadges(Obj(header, "authorBadges"));
	}
	ev.type = "subgift";
	ev.id = "youtube:subgift:" + common.id;
	ev.count = FirstIntegerInRuns(primary);
	hasEvent = true;
}

void BuildGiftRedemption(const json &renderer, ItemCommon &common, json &fragments, Events::NormalizedEvent &ev,
			 bool &hasEvent)
{
	// "<name> was gifted a membership by <gifter>".
	fragments = FragmentsFromRuns(Obj(Obj(renderer, "message"), "runs"));
	ev.type = "member";
	ev.id = "youtube:member:" + common.id;
	hasEvent = true;
}

// addChatItemAction.item.<renderer> -> builder. A renderer name absent from this table is
// SKIPPED SILENTLY: YouTube adds renderers over time (placeholders, mode-change notices,
// engagement messages, product purchases) and an unknown one must never break the read loop.
// Adding support for one is a row here plus its builder.
const std::pair<const char *, RendererFn> kRenderers[] = {
	{"liveChatTextMessageRenderer", BuildTextMessage},
	{"liveChatPaidMessageRenderer", BuildPaidMessage},
	{"liveChatPaidStickerRenderer", BuildPaidSticker},
	{"liveChatMembershipItemRenderer", BuildMembership},
	{"liveChatSponsorshipsGiftPurchaseAnnouncementRenderer", BuildGiftPurchase},
	{"liveChatSponsorshipsGiftRedemptionAnnouncementRenderer", BuildGiftRedemption},
};

// The per-run loop state the action handlers mutate; lives in Run()'s frame.
struct Loop {
	const Config &cfg;
	const Callbacks &cb;
	const std::unordered_map<std::string, std::string> *emotes;
	SeenIds seen;
	// Suppress the batch that follows a reload continuation. Both the initial continuation
	// and a filter switch answer with the chat's HISTORY, and the contract this read has to
	// match is that only messages arriving AFTER the cold connect are emitted -- otherwise
	// the pane floods on every connect. Suppressed ids are still recorded, so a later
	// re-delivery of the same backlog cannot post them either.
	bool suppress = true;
	// Per-batch counters, read and cleared by the poll loop.
	int items = 0;
	int emitted = 0;
	int dropped = 0;
	int suppressed = 0;
};

void OnAddChatItem(Loop &lp, const char *, const json &action)
{
	const json &item = Obj(action, "item");
	for (const auto &entry : kRenderers) {
		const json &renderer = Obj(item, entry.first);
		if (!renderer.is_object()) {
			continue;
		}
		++lp.items;
		ItemCommon common = ReadCommon(renderer);
		if (!lp.seen.add(common.id)) {
			++lp.dropped;
			return;
		}
		if (lp.suppress) {
			++lp.suppressed;
			return;
		}

		json fragments = json::array();
		Events::NormalizedEvent ev;
		bool hasEvent = false;
		entry.second(renderer, common, fragments, ev, hasEvent);

		if (!fragments.empty()) {
			fragments = ApplyThirdPartyEmotes(fragments, *lp.emotes);
			lp.cb.emitMessage(BuildChatMessage("youtube", lp.cfg.channelId, common.id, common.tsMs,
							   common.authorName, std::string(), common.badges,
							   fragments));
			++lp.emitted;
		}
		// Then, IN ADDITION, forward monetization/membership items into the events feed.
		// YouTube has no real-time event socket, so this sink is their only push source.
		if (hasEvent) {
			ev.platform = "youtube";
			ev.actorName = common.authorName;
			ev.ts = common.tsMs;
			lp.cb.emitEvent(ev);
		}
		return;
	}
}

// Moderation and banner actions. The normalized wire shape carries no deletion or pinned-
// banner frame, so there is nothing to render -- but they are recognized ON PURPOSE rather
// than left to the unknown-action path, so the log distinguishes "handled, nothing to draw"
// from "an action name we have never seen".
void OnNothingToRender(Loop &lp, const char *name, const json &)
{
	DBG(LogCat::Chat, "youtube innertube: dest=%s %s has no wire frame to render, skipped",
	    lp.cfg.destTag.c_str(), name);
}

using ActionFn = void (*)(Loop &, const char *name, const json &action);

// actions[] entry name -> handler. Dispatch is a table lookup, not a chain: an entry whose
// name is absent falls through and is skipped silently.
//
// addLiveChatTickerItemAction is here as a NO-OP ON PURPOSE, and must stay one. YouTube emits
// a Super Chat / new membership TWICE while it is happening -- once as the chat item and once
// as the ticker chip above the chat -- under different item ids, so the id dedupe cannot
// collapse them. Rendering the ticker would double every donation in both the chat pane and
// the events feed.
const std::pair<const char *, ActionFn> kActions[] = {
	{"addChatItemAction", OnAddChatItem},
	{"addLiveChatTickerItemAction", OnNothingToRender},
	{"markChatItemAsDeletedAction", OnNothingToRender},
	{"markChatItemsByAuthorAsDeletedAction", OnNothingToRender},
	{"removeChatItemAction", OnNothingToRender},
	{"removeChatItemByAuthorAction", OnNothingToRender},
	{"addBannerToLiveChatCommand", OnNothingToRender},
	{"removeBannerForLiveChatCommand", OnNothingToRender},
};

void ProcessActions(Loop &lp, const json &actions)
{
	if (!actions.is_array()) {
		return;
	}
	for (const json &action : actions) {
		if (lp.cb.canceled()) {
			return;
		}
		for (const auto &entry : kActions) {
			const json &payload = Obj(action, entry.first);
			if (payload.is_object()) {
				entry.second(lp, entry.first, payload);
				break;
			}
		}
	}
}

// The "Live chat" continuation from a view-selector node, "" when the sub-menu is absent or
// too short. The selector appears on the watch-next liveChatRenderer AND on the live-chat
// continuation's own header, so callers try both.
std::string LiveChatFilterFrom(const json &node)
{
	const json &items = Obj(Obj(Obj(Obj(node, "header"), "liveChatHeaderRenderer"), "viewSelector"),
				"sortFilterSubMenuRenderer");
	const json &subMenu = Obj(items, "subMenuItems");
	if (!subMenu.is_array() || subMenu.size() <= kLiveChatFilterIndex) {
		return std::string();
	}
	return Str(Obj(Obj(subMenu[kLiveChatFilterIndex], "continuation"), "reloadContinuationData"), "continuation");
}

// Resolve the first continuation token for `videoId`, via youtubei/v1/next -- NEVER by
// scraping the watch page: a cookieless watch page no longer carries liveChatRenderer at
// all, and HTML parsing is where every recorded breakage in this ecosystem happened. The
// unfiltered "Live chat" token is preferred when the response already offers it, which saves
// both a round trip and a second backlog batch. "" when the video has no live chat.
//
// `canceled` is an out-param rather than a return value because "" already means "no live
// chat here", and the caller must not spend its bootstrap attempts on a canceled read.
std::string Bootstrap(const Config &cfg, const Callbacks &cb, bool &canceled)
{
	const InnerTube::Result resp = InnerTube::Post(kNextUrl, json{{"videoId", cfg.videoId}}, cb.canceled);
	if (resp.canceled) {
		canceled = true;
		return std::string();
	}
	if (resp.status < 200 || resp.status >= 300) {
		DBG(LogCat::Chat, "youtube innertube: dest=%s bootstrap HTTP %ld (%s)", cfg.destTag.c_str(),
		    resp.status, resp.error.empty() ? "no transport error" : resp.error.c_str());
		return std::string();
	}
	const json &liveChatRenderer = Obj(Obj(Obj(Obj(resp.body, "contents"), "twoColumnWatchNextResults"),
					      "conversationBar"),
					   "liveChatRenderer");
	const std::string live = LiveChatFilterFrom(liveChatRenderer);
	if (!live.empty()) {
		DBG(LogCat::Chat,
		    "youtube innertube: dest=%s bootstrap resolved the \"Live chat\" continuation (%zu bytes)",
		    cfg.destTag.c_str(), live.size());
		return live;
	}
	const json &continuations = Obj(liveChatRenderer, "continuations");
	if (!continuations.is_array() || continuations.empty()) {
		DBG(LogCat::Chat, "youtube innertube: dest=%s bootstrap carried no liveChatRenderer continuation",
		    cfg.destTag.c_str());
		return std::string();
	}
	const std::string token = Str(Obj(continuations[0], "reloadContinuationData"), "continuation");
	DBG(LogCat::Chat, "youtube innertube: dest=%s bootstrap resolved a reload continuation (%zu bytes)",
	    cfg.destTag.c_str(), token.size());
	return token;
}

struct NextContinuation {
	std::string token;
	long timeoutMs = 0;
	bool reload = false;
	bool found = false;
};

// continuations[0] carries exactly one of these shapes. liveChatReplayContinuationData and
// playerSeekContinuationData belong to VOD replay and are absent here -- a live read must
// never follow them, so they are simply not in the table.
const struct ContinuationKind {
	const char *key;
	bool reload;
} kContinuationKinds[] = {
	{"invalidationContinuationData", false},
	{"timedContinuationData", false},
	{"reloadContinuationData", true},
};

NextContinuation ReadNextContinuation(const json &liveChat)
{
	NextContinuation out;
	const json &continuations = Obj(liveChat, "continuations");
	if (!continuations.is_array() || continuations.empty()) {
		return out;
	}
	for (const ContinuationKind &kind : kContinuationKinds) {
		const json &data = Obj(continuations[0], kind.key);
		if (!data.is_object()) {
			continue;
		}
		out.token = Str(data, "continuation");
		out.timeoutMs = static_cast<long>(NumLoose(data, "timeoutMs"));
		out.reload = kind.reload;
		out.found = !out.token.empty();
		return out;
	}
	return out;
}

// The cadence step this reader is on: the active floor while messages flow, stepping down
// over a run of empty responses, capped by whatever the response actually ADVISED (read from
// the payload, never assumed). Kept separate from the jitter below so the log can name the
// step -- a jittered value differs on every poll and would log a change every time.
long PollStepMs(int emptyStreak, long advisedMs)
{
	long step = kPollActiveMs;
	if (emptyStreak >= kDeepIdleAfterEmpty) {
		step = kPollDeepIdleMs;
	} else if (emptyStreak >= kIdleAfterEmpty) {
		step = kPollIdleMs;
	}
	if (advisedMs > 0 && step > advisedMs) {
		step = advisedMs;
	}
	return step;
}

// Desynchronize this reader from every other one, then floor the result LAST so neither the
// advised value nor the jitter can produce a hot loop.
long JitteredWaitMs(long stepMs, std::mt19937 &rng)
{
	std::uniform_real_distribution<double> jitter(1.0 - kJitterFraction, 1.0 + kJitterFraction);
	return std::max(static_cast<long>(static_cast<double>(stepMs) * jitter(rng)), kPollFloorMs);
}

} // namespace

bool Run(const Config &cfg, const Callbacks &cb)
{
	if (cfg.videoId.empty()) {
		DBG(LogCat::Chat, "youtube innertube: dest=%s no video id, handing chat to the official read",
		    cfg.destTag.c_str());
		return true;
	}

	static const std::unordered_map<std::string, std::string> kNoEmotes;
	Loop lp{cfg, cb, cfg.thirdPartyEmotes ? cfg.thirdPartyEmotes : &kNoEmotes};
	Backoff backoff(std::chrono::milliseconds(1000), std::chrono::milliseconds(30000));
	// Seeded once per reader; never reseeded per call and never shared across threads.
	std::mt19937 rng(std::random_device{}());

	std::string token;
	for (int attempt = 1; attempt <= kBootstrapAttempts; ++attempt) {
		bool canceled = false;
		token = Bootstrap(cfg, cb, canceled);
		if (canceled) {
			return false;
		}
		if (!token.empty() || attempt == kBootstrapAttempts) {
			break;
		}
		// A blip at go-live must not park a whole broadcast on the quota-billed read, so
		// retry a few times before handing over.
		if (CancelableSleep(backoff.next(), cb.canceled)) {
			return false;
		}
	}
	if (token.empty()) {
		DBG(LogCat::Chat,
		    "youtube innertube: dest=%s no continuation after %d attempts, handing chat to the official read",
		    cfg.destTag.c_str(), kBootstrapAttempts);
		return true;
	}
	backoff.reset();

	bool filterChecked = false;
	int emptyStreak = 0;
	int deadStreak = 0;
	long cadenceMs = 0; // last cadence logged, so only CHANGES are logged

	while (!cb.canceled()) {
		const InnerTube::Result resp =
			InnerTube::Post(kGetLiveChatUrl, json{{"continuation", token}}, cb.canceled);
		if (resp.canceled || cb.canceled()) {
			break;
		}

		if (resp.status == 0) {
			// A transport failure says nothing about this endpoint -- the official read
			// would fail the same way -- so back off and retry rather than handing over.
			DBG(LogCat::Chat, "youtube innertube: dest=%s transport failure (%s), backing off",
			    cfg.destTag.c_str(), resp.error.c_str());
			cb.state(false, resp.error);
			if (CancelableSleep(backoff.next(), cb.canceled)) {
				break;
			}
			continue;
		}

		// `body` is already null for a non-2xx, so the miss below covers both that and a 2xx
		// whose live-chat object is absent.
		const json &liveChat = Obj(Obj(resp.body, "continuationContents"), "liveChatContinuation");
		if (!liveChat.is_object()) {
			// Either a non-2xx, or a 2xx whose live-chat object is missing entirely. NOT
			// terminal: a 403 on a reverse-engineered surface is as likely a bot wall as
			// an ended chat, and an ended chat still answers WITH the object (minus a
			// continuation). Only the official read can tell those apart, so strike and
			// hand chat over to it.
			if (++deadStreak >= kDeadStrikes) {
				DBG(LogCat::Chat,
				    "youtube innertube: dest=%s unusable response (HTTP %ld) on %d consecutive "
				    "polls, handing chat to the official read",
				    cfg.destTag.c_str(), resp.status, deadStreak);
				return true;
			}
			DBG(LogCat::Chat, "youtube innertube: dest=%s unusable response (HTTP %ld), strike %d/%d",
			    cfg.destTag.c_str(), resp.status, deadStreak, kDeadStrikes);
			if (CancelableSleep(backoff.next(), cb.canceled)) {
				break;
			}
			continue;
		}
		deadStreak = 0;
		backoff.reset();
		cb.announce();

		const NextContinuation next = ReadNextContinuation(liveChat);
		ProcessActions(lp, Obj(liveChat, "actions"));
		const int items = lp.items;
		if (lp.suppressed > 0) {
			DBG(LogCat::Chat, "youtube innertube: dest=%s connect batch items=%d (suppressed as backlog)",
			    cfg.destTag.c_str(), lp.suppressed);
		}
		if (lp.dropped > 0) {
			DBG(LogCat::Chat, "youtube innertube: dest=%s dropped %d already-seen item(s)",
			    cfg.destTag.c_str(), lp.dropped);
		}
		if (lp.emitted > 0) {
			DBG(LogCat::Chat, "youtube innertube: dest=%s batch items=%d -> emitted %d",
			    cfg.destTag.c_str(), items, lp.emitted);
		}
		lp.suppress = false;
		lp.items = lp.emitted = lp.dropped = lp.suppressed = 0;

		// AHEAD OF EVERYTHING ELSE -- ahead of the filter switch, the cadence step and the
		// handover strikes. An ended chat answers 200 with a live-chat object that offers no
		// next continuation; treated as anything else it would either sit in a backoff
		// forever or hand over to a read that bills quota against the same dead chat. The
		// only correct response is to stop. This batch's actions were emitted just above, so
		// a final message is not lost.
		if (!next.found) {
			DBG(LogCat::Chat,
			    "youtube innertube: dest=%s response carried no next continuation -> chat ended, "
			    "stopping reads",
			    cfg.destTag.c_str());
			cb.terminal("YouTube live chat ended");
			return false;
		}

		// Switch to the unfiltered view once, if the bootstrap did not already land on it.
		if (!filterChecked) {
			filterChecked = true;
			const std::string live = LiveChatFilterFrom(liveChat);
			if (!live.empty() && live != token) {
				token = live;
				lp.suppress = true; // the switch replays history on its first response
				DBG(LogCat::Chat,
				    "youtube innertube: dest=%s switched to the unfiltered \"Live chat\" view",
				    cfg.destTag.c_str());
				if (CancelableSleep(std::chrono::milliseconds(kPollFloorMs), cb.canceled)) {
					break;
				}
				continue;
			}
		}

		token = next.token;
		if (next.reload) {
			// A reload continuation restarts the feed from history rather than resuming.
			lp.suppress = true;
			DBG(LogCat::Chat, "youtube innertube: dest=%s took a reload continuation, suppressing its "
					  "replayed batch",
			    cfg.destTag.c_str());
		}

		emptyStreak = items > 0 ? 0 : emptyStreak + 1;
		const long stepMs = PollStepMs(emptyStreak, next.timeoutMs);
		if (stepMs != cadenceMs) {
			cadenceMs = stepMs;
			DBG(LogCat::Chat,
			    "youtube innertube: dest=%s poll cadence -> ~%ldms (idle streak %d, advised %ldms)",
			    cfg.destTag.c_str(), stepMs, emptyStreak, next.timeoutMs);
		}
		if (CancelableSleep(std::chrono::milliseconds(JitteredWaitMs(stepMs, rng)), cb.canceled)) {
			break;
		}
	}

	DBG(LogCat::Chat, "youtube innertube: dest=%s read loop exited (canceled=%d)", cfg.destTag.c_str(),
	    cb.canceled() ? 1 : 0);
	return false;
}

} // namespace YouTubeInnerTube

} // namespace Chat
