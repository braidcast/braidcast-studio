#include "PollTemplateStore.hpp"

#include "MruRows.hpp"
#include "StorePaths.hpp"

#include "log.hpp"
#include "util/json_util.hpp"
#include "util/string_util.hpp"
#include "util/time_util.hpp"

#include <uuid_util.hpp>

#include <util/platform.h>

#include <utility>

using json = nlohmann::json;

namespace {

constexpr int kStoreVersion = 1;

std::string FilePath()
{
	return MultistreamBasicPath("poll_templates.json");
}

// Question then each option, length-prefixed so no text a streamer types can forge a
// boundary between two parts (see StringUtil::AppendLengthPrefixed).
std::string TemplateIdentity(const std::string &question, const std::vector<std::string> &options)
{
	std::string identity;
	StringUtil::AppendLengthPrefixed(identity, question);
	for (const std::string &option : options) {
		StringUtil::AppendLengthPrefixed(identity, option);
	}
	return identity;
}

std::vector<std::string> TrimAll(const std::vector<std::string> &values)
{
	std::vector<std::string> out;
	out.reserve(values.size());
	for (const std::string &value : values) {
		out.push_back(StringUtil::Trim(value));
	}
	return out;
}

} // namespace

void PollTemplateStore::Load()
{
	templates_.clear();

	const std::string path = FilePath();
	const json root = LoadStoreJson(path);
	const json &stored = JsonUtil::Obj(root, "templates");
	if (!stored.is_array()) {
		if (os_file_exists(path.c_str())) {
			HostLog("[storage] poll_templates.json unreadable or malformed; the saved poll templates it "
				"held are gone");
		}
		return;
	}

	const int64_t now = TimeUtil::NowMs();
	for (const json &item : stored) {
		if (!item.is_object()) {
			continue;
		}
		Template row;
		row.id = JsonUtil::Str(item, "id");
		row.question = StringUtil::Trim(JsonUtil::Str(item, "question"));
		if (row.id.empty() || row.question.empty()) {
			continue;
		}
		// Options are stored as {text} objects, not bare strings: SaveStoreJson routes through
		// obs_data, whose arrays hold objects only and silently drop every scalar element.
		const json &options = JsonUtil::Obj(item, "options");
		if (!options.is_array()) {
			continue;
		}
		for (const json &option : options) {
			const std::string text = StringUtil::Trim(JsonUtil::Str(option, "text"));
			if (!text.empty()) {
				row.options.push_back(text);
			}
		}
		if (row.options.empty()) {
			continue;
		}
		row.name = JsonUtil::Str(item, "name");
		row.createdAtMs = MruRows::ReadTimestamp(item, "createdAtMs", now);
		row.lastUsedAtMs = MruRows::ReadTimestamp(item, "lastUsedAtMs", now);
		templates_.push_back(std::move(row));
	}
	MruRows::Normalize(templates_, kMaxTemplates);
}

bool PollTemplateStore::Save() const
{
	json rows = json::array();
	for (const Template &row : templates_) {
		json options = json::array();
		for (const std::string &option : row.options) {
			options.push_back(json{{"text", option}});
		}
		rows.push_back(json{{"id", row.id},
				    {"name", row.name},
				    {"question", row.question},
				    {"options", std::move(options)},
				    {"createdAtMs", row.createdAtMs},
				    {"lastUsedAtMs", row.lastUsedAtMs}});
	}
	return SaveStoreJson(json{{"version", kStoreVersion}, {"templates", std::move(rows)}}, FilePath());
}

json PollTemplateStore::List() const
{
	json out = json::array();
	for (const Template &row : templates_) {
		out.push_back(json{{"id", row.id},
				   {"name", row.name},
				   {"question", row.question},
				   {"options", row.options},
				   {"createdAtMs", row.createdAtMs},
				   {"lastUsedAtMs", row.lastUsedAtMs}});
	}
	return out;
}

std::string PollTemplateStore::Remember(const std::string &question, const std::vector<std::string> &options,
					bool &created)
{
	const std::string trimmedQuestion = StringUtil::Trim(question);
	const std::vector<std::string> trimmedOptions = TrimAll(options);
	const std::string incoming = TemplateIdentity(trimmedQuestion, trimmedOptions);
	const int64_t usedNow = MruRows::UsedNowMs(templates_);

	for (Template &row : templates_) {
		if (TemplateIdentity(row.question, row.options) != incoming) {
			continue;
		}
		row.lastUsedAtMs = usedNow;
		created = false;
		const std::string id = row.id;
		MruRows::Normalize(templates_, kMaxTemplates);
		return id;
	}

	Template fresh;
	fresh.id = UuidUtil::New();
	fresh.question = trimmedQuestion;
	fresh.options = trimmedOptions;
	fresh.createdAtMs = TimeUtil::NowMs();
	fresh.lastUsedAtMs = usedNow;
	const std::string id = fresh.id;
	templates_.push_back(std::move(fresh));
	MruRows::Normalize(templates_, kMaxTemplates);
	created = Find(id) != templates_.end();
	return created ? id : std::string();
}

bool PollTemplateStore::Touch(const std::string &id)
{
	const auto it = Find(id);
	if (it == templates_.end()) {
		return false;
	}
	it->lastUsedAtMs = MruRows::UsedNowMs(templates_);
	MruRows::Normalize(templates_, kMaxTemplates);
	return true;
}

bool PollTemplateStore::Remove(const std::string &id)
{
	const auto it = Find(id);
	if (it == templates_.end()) {
		return false;
	}
	templates_.erase(it);
	return true;
}

bool PollTemplateStore::Rename(const std::string &id, const std::string &name)
{
	const auto it = Find(id);
	if (it == templates_.end()) {
		return false;
	}
	// Naming a template is not using it, so the order stays where it was.
	it->name = name;
	return true;
}

auto PollTemplateStore::Find(const std::string &id) -> std::vector<Template>::iterator
{
	return MruRows::FindById(templates_, id);
}
