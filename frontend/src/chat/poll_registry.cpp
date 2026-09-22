#include "poll_registry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "../bridge.hpp"
#include "../event_names.hpp"
#include "util/async_task.hpp"
#include "util/json_util.hpp"
#include "util/time_util.hpp"

namespace Chat {

using json = nlohmann::json;

namespace {

// The option rows a freshly opened poll starts with: the platform's own list when it reported
// one, else what was asked for, with no tallies yet.
json OpeningOptions(const std::vector<std::string> &asked, const json &reported)
{
	const json &rows = JsonUtil::Obj(reported, "options");
	if (rows.is_array() && !rows.empty()) {
		return rows;
	}
	json out = json::array();
	for (const std::string &text : asked) {
		out.push_back(json{{"text", text}, {"tally", nullptr}, {"ratio", nullptr}});
	}
	return out;
}

// Lay the tallies a closing response carries over the rows already held. Matched by option
// text first; by position when the text is missing but the counts agree, which is what a
// response that omits the texts but keeps the order still supports. A row with no match
// keeps its current tally rather than being reset.
void MergeTallies(json &held, const json &reported)
{
	const json &rows = JsonUtil::Obj(reported, "options");
	if (!rows.is_array() || !held.is_array()) {
		return;
	}
	const bool sameCount = rows.size() == held.size();
	for (size_t i = 0; i < held.size(); ++i) {
		const std::string text = JsonUtil::Str(held[i], "text");
		const auto byText = std::find_if(rows.begin(), rows.end(), [&text](const json &row) {
			return JsonUtil::Str(row, "text") == text;
		});
		const json *match = byText != rows.end() ? &*byText : nullptr;
		if (!match && sameCount && JsonUtil::Str(rows[i], "text").empty()) {
			match = &rows[i];
		}
		if (match && match->contains("tally") && !(*match)["tally"].is_null()) {
			held[i]["tally"] = (*match)["tally"];
		}
	}
}

} // namespace

json PollRegistry::ToJson(const Poll &poll)
{
	json out = json{{"id", poll.id},
			{"accountId", poll.dest.accountId},
			{"profileUuid", poll.dest.profileUuid},
			{"question", poll.question},
			{"options", poll.options},
			{"status", poll.status},
			{"startedAtMs", poll.startedAtMs},
			{"endedAtMs", poll.endedAtMs ? json(*poll.endedAtMs) : json(nullptr)},
			{"totalVotes", poll.totalVotes ? json(*poll.totalVotes) : json(nullptr)}};
	if (!poll.error.empty()) {
		out["error"] = poll.error;
	}
	if (poll.finishing) {
		out["finishing"] = true;
	}
	return out;
}

json PollRegistry::ListLocked() const
{
	json rows = json::array();
	for (auto it = polls_.rbegin(); it != polls_.rend(); ++it) {
		rows.push_back(ToJson(*it));
	}
	return json{{"polls", std::move(rows)}};
}

auto PollRegistry::FindLocked(const std::string &id) -> std::vector<Poll>::iterator
{
	return std::find_if(polls_.begin(), polls_.end(), [&id](const Poll &p) { return p.id == id; });
}

auto PollRegistry::FindLocked(const std::string &id) const -> const Poll *
{
	const auto it = std::find_if(polls_.begin(), polls_.end(), [&id](const Poll &p) { return p.id == id; });
	return it == polls_.end() ? nullptr : &*it;
}

void PollRegistry::EmitChanged(const json &list)
{
	// Queued even on the UI thread: EmitEvent would deliver inline there but post from a
	// worker, letting an older worker snapshot land after a newer UI-thread one.
	AsyncTask::QueueOnUi([list] { Bridge::EmitEvent(EventNames::kPollsChanged, list); });
}

json PollRegistry::Open(const OAuth::DestinationId &dest, const std::string &question,
			const std::vector<std::string> &options, const json &reported)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json result;
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		Poll poll;
		poll.id = JsonUtil::Str(reported, "id");
		poll.dest = dest;
		poll.question = JsonUtil::Str(reported, "question");
		if (poll.question.empty()) {
			poll.question = question;
		}
		poll.options = OpeningOptions(options, reported);
		poll.status = JsonUtil::Str(reported, "status") == "closed" ? "closed" : "active";
		poll.startedAtMs = TimeUtil::NowMs();
		if (poll.status == "closed") {
			poll.endedAtMs = poll.startedAtMs;
		}
		// A platform reusing an id is the same poll reported again, not a second one.
		polls_.erase(std::remove_if(polls_.begin(), polls_.end(),
					    [&poll](const Poll &p) { return p.id == poll.id; }),
			     polls_.end());
		result = ToJson(poll);
		polls_.push_back(std::move(poll));
		list = ListLocked();
	}
	EmitChanged(list);
	return result;
}

json PollRegistry::Get(const std::string &id) const
{
	const std::lock_guard<std::mutex> lock(mutex_);
	const Poll *poll = FindLocked(id);
	return poll ? ToJson(*poll) : json(nullptr);
}

json PollRegistry::Close(const std::string &id, const json &reported)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json result;
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto poll = FindLocked(id);
		if (poll == polls_.end()) {
			return json(nullptr);
		}
		MergeTallies(poll->options, reported);
		poll->status = "closed";
		poll->endedAtMs = TimeUtil::NowMs();
		poll->error.clear();
		result = ToJson(*poll);
		list = ListLocked();
	}
	EmitChanged(list);
	return result;
}

json PollRegistry::Fail(const std::string &id, const std::string &error)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json result;
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto poll = FindLocked(id);
		if (poll == polls_.end()) {
			return json(nullptr);
		}
		poll->error = error;
		result = ToJson(*poll);
		list = ListLocked();
	}
	EmitChanged(list);
	return result;
}

PollRegistry::DismissResult PollRegistry::Dismiss(const std::string &id, bool orphaned)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto it = FindLocked(id);
		if (it == polls_.end()) {
			return DismissResult::Unknown;
		}
		if (it->status != "closed" && it->error.empty() && !orphaned) {
			return DismissResult::StillRunning;
		}
		polls_.erase(it);
		list = ListLocked();
	}
	EmitChanged(list);
	return DismissResult::Removed;
}

void PollRegistry::UpdateLive(const OAuth::DestinationId &dest, const json &live)
{
	const json &rows = JsonUtil::Obj(live, "options");
	if (!rows.is_array()) {
		return;
	}
	const json &totalRaw = JsonUtil::Obj(live, "totalVotes");
	const std::optional<int64_t> total =
		totalRaw.is_number_integer() ? std::optional<int64_t>(totalRaw.get<int64_t>()) : std::nullopt;

	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		const auto poll = std::find_if(polls_.rbegin(), polls_.rend(), [&dest](const Poll &p) {
			return p.dest == dest && p.status == "active";
		});
		if (poll == polls_.rend() || !poll->options.is_array() || poll->options.size() != rows.size()) {
			return;
		}
		const json before = poll->options;
		const std::optional<int64_t> totalBefore = poll->totalVotes;
		for (size_t i = 0; i < rows.size(); ++i) {
			const json &ratio = JsonUtil::Obj(rows[i], "ratio");
			json &held = poll->options[i];
			held["ratio"] = ratio.is_number() ? ratio : json(nullptr);
			if (total && ratio.is_number()) {
				held["tally"] = static_cast<int64_t>(std::llround(ratio.get<double>() * *total));
			}
		}
		if (total) {
			poll->totalVotes = total;
		}
		if (poll->options == before && poll->totalVotes == totalBefore) {
			return;
		}
		list = ListLocked();
	}
	EmitChanged(list);
}

json PollRegistry::MarkFinishing(const std::optional<OAuth::DestinationId> &dest)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json marked = json::array();
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		for (Poll &poll : polls_) {
			if (!poll.finishing && (!dest || poll.dest == *dest)) {
				poll.finishing = true;
				marked.push_back(ToJson(poll));
			}
		}
		if (marked.empty()) {
			return marked;
		}
		list = ListLocked();
	}
	EmitChanged(list);
	return marked;
}

json PollRegistry::Take(const std::vector<std::string> &ids)
{
	const std::lock_guard<std::mutex> emitLock(emitMutex_);
	json taken = json::array();
	json list;
	{
		const std::lock_guard<std::mutex> lock(mutex_);
		for (const std::string &id : ids) {
			const auto poll = FindLocked(id);
			if (poll != polls_.end()) {
				poll->finishing = false;
				taken.push_back(ToJson(*poll));
				polls_.erase(poll);
			}
		}
		if (taken.empty()) {
			return taken;
		}
		list = ListLocked();
	}
	EmitChanged(list);
	return taken;
}

json PollRegistry::List() const
{
	const std::lock_guard<std::mutex> lock(mutex_);
	return ListLocked();
}

PollRegistry &Polls()
{
	static PollRegistry registry;
	return registry;
}

} // namespace Chat
