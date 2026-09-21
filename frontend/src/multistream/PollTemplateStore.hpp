#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The polls a streamer has run, kept to run again: a capped most-recently-used history
// persisted to poll_templates.json.
//
// A template's identity is DERIVED from its question and options (trimmed, exact case, in
// order), never stored, so running the same poll twice bumps one row while changing any
// option makes a second. The optional name is the one field identity ignores.
//
// UI-thread-only and unguarded, like StreamInfoPresetStore. Trivial ctor; Load() runs from
// the bootstrap. Mutators do NOT persist -- callers Save() when they want it on disk, which
// is also what lets the self-test drive a private instance without touching the real file.
class PollTemplateStore {
public:
	// The most templates kept; past it the least recently used row is dropped.
	static constexpr size_t kMaxTemplates = 20;

	PollTemplateStore() = default;

	PollTemplateStore(const PollTemplateStore &) = delete;
	PollTemplateStore &operator=(const PollTemplateStore &) = delete;

	// Read poll_templates.json (if present) into memory.
	void Load();

	// Persist every template. Returns false on write failure (already logged).
	bool Save() const;

	// Every template as {id, name, question, options:[string], createdAtMs, lastUsedAtMs},
	// most recently used first.
	nlohmann::json List() const;

	// Upsert the poll `question` + `options` (trimmed here). A match keeps its id, name and
	// creation stamp and is marked used now; anything else becomes a new, unnamed template.
	// `created` reports which happened, read back from the store after the cap applies.
	// Returns the template's id, or "" when no row was kept.
	std::string Remember(const std::string &question, const std::vector<std::string> &options, bool &created);

	// Mark `id` used now. False when unknown.
	bool Touch(const std::string &id);

	bool Remove(const std::string &id);

	// Set `id`'s name; "" returns it to the question as its label. False when unknown.
	bool Rename(const std::string &id, const std::string &name);

private:
	struct Template {
		std::string id;
		std::string name;
		std::string question;
		std::vector<std::string> options;
		int64_t createdAtMs = 0;
		int64_t lastUsedAtMs = 0;
	};

	std::vector<Template>::iterator Find(const std::string &id);

	// Ordered most recently used first (MruRows::Normalize), which is also the order List()
	// and the on-disk file carry.
	std::vector<Template> templates_;
};
