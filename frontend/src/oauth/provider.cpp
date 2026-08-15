#include "provider.hpp"

#include <algorithm>
#include <array>

#include "../chat/chat_transport.hpp"
#include "../events/event_transport.hpp"
#include "util/http_client.hpp"
#include "util/json_util.hpp"
#include "util/op_error.hpp"
#include "util/string_util.hpp"
#include "../log.hpp"

// The base StreamProvider's transport factories default to "no transport". They are
// defined here rather than inline in the header because the return type is a
// std::unique_ptr<> of a type the header only forward-declares (to break the
// chat_transport.hpp <-> provider.hpp include cycle); unique_ptr's default_delete
// needs the complete type, which this translation unit sees via the includes above.
namespace OAuth {

namespace {

using JsonUtil::Obj;
using JsonUtil::Str;

// How one field's two values are reduced to something comparable. Text and Flag compare what
// they render; CategoryId compares the platform id behind a name the user reads; StringSet
// compares membership, case-folded and order-free, because a platform is free to re-case and
// re-order a tag list without having changed it.
enum class Compare { Text, Flag, CategoryId, StringSet };

// EVERY metadata field the go-live read-back compares, and what a mismatch costs. A key absent
// from this table is never compared, so ADDING A FIELD IS ONE ROW HERE -- and setting the third
// column true is the whole of what makes a mismatch refuse the go-live instead of reporting it.
//
// Safety means "who can see this broadcast": a divergence there would put the stream in front of
// an audience nobody confirmed, which is the one outcome worth losing an airing over. Everything
// else is cosmetic, because a stale title is worth far less than a broadcast that never happens.
struct FieldRule {
	const char *key;
	const char *label;
	bool safety;
	Compare compare;
};

const std::array<FieldRule, 9> kFieldRules = {{
	{"privacy", "Privacy", true, Compare::Text},
	{"madeForKids", "Made for kids", true, Compare::Flag},
	{"title", "Title", false, Compare::Text},
	{"description", "Description", false, Compare::Text},
	{"category", "Category", false, Compare::CategoryId},
	{"tags", "Tags", false, Compare::StringSet},
	{"language", "Language", false, Compare::Text},
	{"contentLabels", "Content classification", false, Compare::StringSet},
	{"brandedContent", "Branded content", false, Compare::Flag},
}};

std::string JoinValues(const std::vector<std::string> &values, const char *separator)
{
	std::string out;
	for (const std::string &value : values) {
		if (!out.empty()) {
			out += separator;
		}
		out += value;
	}
	return out;
}

// Render one field out of a metadata bag: `cmp` is what two bags are compared by, `text` is what
// the streamer reads. Returns false when the bag makes no assertion about this field -- an
// untouched field cannot disagree with anything.
bool ReadField(const json &bag, const FieldRule &rule, std::string &cmp, std::string &text)
{
	switch (rule.compare) {
	case Compare::Text:
		text = Str(bag, rule.key);
		cmp = text;
		return !text.empty();
	case Compare::Flag: {
		const json &value = Obj(bag, rule.key);
		if (!value.is_boolean()) {
			return false;
		}
		cmp = value.get<bool>() ? "yes" : "no";
		text = cmp;
		return true;
	}
	case Compare::CategoryId: {
		const json &category = Obj(bag, rule.key);
		cmp = Str(category, "id");
		const std::string name = Str(category, "name");
		text = name.empty() ? cmp : name;
		return !cmp.empty();
	}
	case Compare::StringSet: {
		const json &list = Obj(bag, rule.key);
		if (!list.is_array()) {
			return false;
		}
		std::vector<std::string> values;
		for (const json &entry : list) {
			if (entry.is_string() && !entry.get<std::string>().empty()) {
				values.push_back(entry.get<std::string>());
			}
		}
		text = JoinValues(values, ", ");
		std::vector<std::string> folded;
		folded.reserve(values.size());
		for (const std::string &value : values) {
			folded.push_back(StringUtil::ToLower(value));
		}
		std::sort(folded.begin(), folded.end());
		cmp = JoinValues(folded, "\n");
		// An EMPTY list is a real assertion ("no tags"), unlike an absent key -- which is
		// why every provider must omit the key rather than report an empty array for a
		// field it could not read.
		return true;
	}
	}
	return false;
}

// The PLATFORM's side of the comparison, where PRESENCE decides rather than emptiness. A provider
// omits any field it could not read, so a key that is there carries a stated value -- including an
// empty one, which is a real answer ("the channel holds no title") and not the absence of one.
// Reading it through the requested side's emptiness rule instead would make a platform reporting
// an empty visibility produce neither a divergence nor an unconfirmed note: a clean result
// carrying no signal at all, which is the one outcome this whole path exists to prevent.
bool ReadActual(const json &bag, const FieldRule &rule, std::string &cmp, std::string &text)
{
	if (!bag.is_object() || !bag.contains(rule.key)) {
		return false;
	}
	ReadField(bag, rule, cmp, text);
	return true;
}

// The descriptor's declared default for `key`. Read off capabilityJson() rather than kept in a
// second table here: the descriptor is already the one statement of what a field defaults to, and
// it is what the dialog renders for a field the remembered bag never carried -- so it, not "", is
// what an untouched field asserts to the platform.
json DescriptorDefault(const json &capability, const char *key)
{
	const json &fields = Obj(capability, "fields");
	if (!fields.is_array()) {
		return json(nullptr);
	}
	for (const json &field : fields) {
		if (Str(field, "key") == key) {
			return Obj(field, "default");
		}
	}
	return json(nullptr);
}

// What this destination effectively asked for: the bag's own value, else the descriptor default.
// The fallback runs through ReadField on a one-key bag so a default is parsed by exactly the
// same code as a submitted value.
bool ReadRequested(const json &capability, const json &fields, const FieldRule &rule, std::string &cmp,
		   std::string &text)
{
	if (ReadField(fields, rule, cmp, text)) {
		return true;
	}
	const json fallback = DescriptorDefault(capability, rule.key);
	if (fallback.is_null()) {
		return false;
	}
	return ReadField(json{{rule.key, fallback}}, rule, cmp, text);
}

// The whole comparison, in one place, over the table above: every field the platform stated and
// this destination asserted, where the two disagree.
std::vector<MetadataDivergence> DiffAppliedMetadata(const json &capability, const json &requested, const json &actual)
{
	std::vector<MetadataDivergence> out;
	for (const FieldRule &rule : kFieldRules) {
		std::string actualCmp;
		std::string actualText;
		// Compared only where the PLATFORM stated a value: a field a provider cannot read
		// back has nothing to disagree with, and inventing one manufactures divergences.
		if (!ReadActual(actual, rule, actualCmp, actualText)) {
			continue;
		}
		std::string wantCmp;
		std::string wantText;
		if (!ReadRequested(capability, requested, rule, wantCmp, wantText)) {
			continue;
		}
		if (wantCmp == actualCmp) {
			continue;
		}
		out.push_back(MetadataDivergence{rule.key, rule.label, wantText, actualText, rule.safety});
	}
	return out;
}

} // namespace

std::vector<MetadataDivergence> SafetyDivergences(const std::vector<MetadataDivergence> &divergences)
{
	std::vector<MetadataDivergence> out;
	for (const MetadataDivergence &entry : divergences) {
		if (entry.safety) {
			out.push_back(entry);
		}
	}
	return out;
}

json DivergencesJson(const std::vector<MetadataDivergence> &divergences)
{
	json out = json::array();
	for (const MetadataDivergence &entry : divergences) {
		out.push_back(json{{"field", entry.field},
				   {"label", entry.label},
				   {"requested", entry.requested},
				   {"actual", entry.actual},
				   {"safety", entry.safety},
				   {"remedy", entry.remedy}});
	}
	return out;
}

std::string DivergenceSummary(const std::vector<MetadataDivergence> &divergences)
{
	std::vector<std::string> parts;
	parts.reserve(divergences.size());
	for (const MetadataDivergence &entry : divergences) {
		std::string part = entry.label + ": asked for \"" + entry.requested +
				   "\", the platform last reported \"" + entry.actual + "\"";
		if (!entry.remedy.empty()) {
			part += " -- " + entry.remedy;
		}
		parts.push_back(std::move(part));
	}
	return JoinValues(parts, "; ");
}

std::unique_ptr<Chat::ChatTransport> StreamProvider::makeChat(const OAuthAccount &acct)
{
	(void)acct;
	return nullptr;
}

std::unique_ptr<Events::EventTransport> StreamProvider::makeEvents(const OAuthAccount &acct)
{
	(void)acct;
	return nullptr;
}

bool StreamProvider::confirmDestination(OAuthAccount &acct, const std::string &profileUuid, const json &requested,
					AppliedBy by, DestinationReadback &readback)
{
	AppliedState actual;
	std::string readErr;
	if (!readAppliedMetadata(acct, profileUuid, by, actual, readErr)) {
		readback.unconfirmed = readErr.empty() ? displayName() + " did not report what it applied" : readErr;
		return false;
	}
	std::vector<MetadataDivergence> fresh = DiffAppliedMetadata(capabilityJson(), requested, actual.fields);
	for (MetadataDivergence &entry : fresh) {
		entry.remedy = divergenceRemedy(entry.field);
	}
	// A field this read did not STATE has not been answered, only passed over: the diff compares
	// what the platform stated and is silent about the rest, so an empty result means "nothing to
	// compare" just as readily as "we agree". Anything an earlier rung caught on such a field is
	// carried through that silence rather than dropped -- without this a read that simply omits
	// the visibility reads as agreement, and a destination the ladder had already watched the
	// platform get wrong would go live on it. Nothing is duplicated: a field the read did state
	// is decided by `fresh` alone.
	for (const MetadataDivergence &prior : readback.divergences) {
		if (!actual.fields.is_object() || !actual.fields.contains(prior.field)) {
			fresh.push_back(prior);
		}
	}
	readback.unconfirmed = actual.unconfirmed;
	readback.divergences = std::move(fresh);
	return true;
}

// The go-live precondition, implemented once for every platform: apply, then ask the platform
// what it ended up with, and where the two disagree, make the platform match. Every per-platform
// part is a hook -- whether a broadcast exists, how one is made, how the current state is read,
// what a corrective push looks like -- so adding a platform means answering those rather than
// touching the go-live path, and no platform can be the one that forgets to confirm.
bool StreamProvider::prepareDestination(OAuthAccount &acct, const std::string &profileUuid, const json &fields,
					DestinationReadback &readback, std::string &err)
{
	readback = DestinationReadback{};
	const json requested = fields.is_object() ? fields : json::object();

	// A destination that is ALREADY broadcasting is edited in place, never re-created:
	// goingLive=false is that path, and it is what carries the dialog's values onto a broadcast
	// this session did not make. Skipping the apply instead -- which is what this used to do --
	// is how a relaunch mid-stream ran the encoders under whatever title and visibility that
	// broadcast happened to carry. A persistent-channel provider answers no here and ignores
	// the flag, so it keeps pushing exactly as before.
	const bool creating = !hasActiveBroadcast(acct, profileUuid);
	if (!applyMetadata(acct, profileUuid, requested, creating, err)) {
		return false;
	}
	// How a read may be INTERPRETED, which is a different question from who wrote the value. The
	// two reads that observe the apply above are told the broadcast's origin, because a provider
	// is free to withhold a value it cannot yet vouch for on a broadcast this call has only just
	// made, and calling those reads an edit would compare a value the create path deliberately
	// holds back. The read after the corrective push is told Edit unconditionally: by then this
	// code has written that value at a broadcast that already existed, so the answer is about an
	// edit whatever made the broadcast -- and leaving it at Create would make the push
	// structurally incapable of being observed to have worked, since the same withholding that
	// hid the value before would hide the corrected one too.
	const AppliedBy by = creating ? AppliedBy::Create : AppliedBy::Edit;

	if (!confirmDestination(acct, profileUuid, requested, by, readback) || readback.divergences.empty()) {
		return true;
	}

	// The platform is not holding what it was asked for. Below is the whole of what this does
	// about that: ONE re-read, then ONE corrective push confirmed by ONE more read, written out
	// as three statements. The bound is the shape of the code rather than a limit checked inside
	// a loop -- there is no counter to raise and no condition to widen, so "try harder" is not a
	// one-line change here, which for calls billed per attempt against a shared daily quota is
	// the point.
	//
	// Cheapest rung first, and for every diverged field whatever it costs to lose: a read taken
	// immediately after a write can land before the platform has settled it, and on YouTube that
	// read costs 1 unit where the write costs 50. Only an answered read that now agrees ends this
	// here -- a read that failed has disproved nothing, and the divergence the first read did see
	// still stands. Hence the && here against the || above: the first read failing means there is
	// nothing yet to carry, while this one failing leaves a divergence that must still be answered.
	if (confirmDestination(acct, profileUuid, requested, by, readback) && readback.divergences.empty()) {
		return true;
	}

	// The corrective push is spent on SAFETY divergences alone. A cosmetic value the platform
	// normalized rather than refused -- YouTube strips angle brackets out of a title -- comes back
	// divergent on every single go-live, so re-sending it buys an unchanged notice at 50 units a
	// time out of a daily pool every install shares. That write is worth its price only where the
	// alternative is refusing to stream at all, which is exactly what a safety field is.
	if (SafetyDivergences(readback.divergences).empty()) {
		return true;
	}

	// Push the values at it again. This is the rung that makes the platform match the dialog,
	// which is what editing stream info is for. It overwrites a value changed outside the app
	// since -- in the platform's own studio, say -- and that is intended at go-live: the user has
	// just read these values and pressed the button, so they are the stated intent for this
	// broadcast.
	//
	// The push's own result is advisory. What the platform ends up holding is settled by the read
	// after it, not by whether the call returned true, so a failed push still gets confirmed
	// rather than assumed.
	std::string reapplyErr;
	const bool pushed = reapplyMetadata(acct, profileUuid, requested, reapplyErr);
	// Logged either way. A provider can report success having sent nothing -- an edit path whose
	// broadcast lookup misses returns true without a request -- so a refusal that follows would
	// otherwise be indistinguishable from one where the platform rejected the value outright.
	HostLog("[oauth] " + displayName() + " corrective stream-info push for destination " + profileUuid +
		(pushed ? std::string(" returned success") : ": " + Err::Diagnostic(reapplyErr)));
	confirmDestination(acct, profileUuid, requested, AppliedBy::Edit, readback);
	return true;
}

bool StreamProvider::ensureIdentity(OAuthAccount &acct, std::string &err)
{
	if (!acct.userId.empty()) {
		return true;
	}
	return fetchIdentity(acct, err);
}

void StreamProvider::stampAuth(Http::HttpReq &r, const OAuthAccount &acct) const
{
	r.headers.push_back("Authorization: Bearer " + acct.access);
}

bool StreamProvider::SendAuthed(OAuthAccount &acct, Http::HttpReq req, Http::HttpResponse &resp, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort: if it fails the token
	// may still be valid, so we let the request proceed and rely on the 401 path).
	std::string freshErr;
	auth()->ensureFresh(acct, freshErr);

	Http::HttpReq attempt = req;
	stampAuth(attempt, acct);
	resp = Http::HttpRequest(attempt);
	if (resp.status == 0) {
		err = displayName() + " request failed: " + resp.error;
		return false;
	}
	if (resp.status != 401) {
		return true;
	}

	// Reactive 401: force one refresh + retry with the new bearer. Route through
	// ensureFresh(force) -- NOT a bare refresh() -- so a rotated refresh token is
	// re-read + written back under the same single-flight lock the proactive path uses.
	// Kick rotates its refresh token on every refresh, so a bare refresh() would rotate
	// it in memory and drop the new token, bricking the account on the next refresh;
	// ensureFresh(force) keeps every provider on one store-coherent path (benign for
	// Twitch, whose refresh tokens do not rotate).
	std::string refreshErr;
	if (!auth()->ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return false;
	}
	Http::HttpReq retry = req;
	stampAuth(retry, acct);
	resp = Http::HttpRequest(retry);
	if (resp.status == 0) {
		err = displayName() + " request failed: " + resp.error;
		return false;
	}
	if (resp.status == 401) {
		err = "re-authentication required";
		return false;
	}
	return true;
}

long StreamProvider::SendAuthedStreaming(OAuthAccount &acct, Http::HttpReq req,
					 const std::function<bool(std::string_view chunk)> &onChunk,
					 std::string &errorBody, std::string &err)
{
	// Proactive refresh inside the skew window (best-effort, mirroring SendAuthed): if it
	// fails the token may still be valid, so proceed and rely on the 401 path below.
	std::string freshErr;
	auth()->ensureFresh(acct, freshErr);

	Http::HttpReq attempt = req;
	stampAuth(attempt, acct);
	errorBody.clear();
	long status = Http::HttpRequestStreaming(attempt, onChunk, errorBody, err);
	if (status == 0) {
		err = displayName() + " request failed: " + err;
		return 0;
	}
	if (status != 401) {
		return status;
	}

	// Reactive 401: force one refresh + retry with the new bearer. A non-2xx body is not
	// streamed to onChunk (HttpRequestStreaming captured it into errorBody instead), so the
	// caller has emitted nothing yet and the retry starts clean. Route through
	// ensureFresh(force) for the same store-coherent single-flight path SendAuthed uses.
	std::string refreshErr;
	if (!auth()->ensureFresh(acct, refreshErr, /*force=*/true)) {
		err = "re-authentication required";
		return 401;
	}
	Http::HttpReq retry = req;
	stampAuth(retry, acct);
	errorBody.clear();
	status = Http::HttpRequestStreaming(retry, onChunk, errorBody, err);
	if (status == 0) {
		err = displayName() + " request failed: " + err;
		return 0;
	}
	if (status == 401) {
		err = "re-authentication required";
	}
	return status;
}

} // namespace OAuth
