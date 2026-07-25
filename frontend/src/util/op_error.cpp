#include "op_error.hpp"

#include <nlohmann/json.hpp>

namespace Err {

namespace {

using json = nlohmann::json;

// The envelope discriminator. Decode REQUIRES it, so a legitimate plain error
// that merely happens to start with '{' can never be misread as an envelope.
constexpr const char *kTag = "__err";

// dump() must never throw into CEF or a detached worker: diagnostics embed raw
// response bodies that may hold invalid UTF-8, so encode with lossy replacement.
std::string Dump(const json &j)
{
	return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// True (filling d/u) only for a well-formed envelope: an object carrying the
// discriminator and a string "d". Anything else -- plain string, foreign JSON,
// truncated envelope -- is not-an-envelope, so callers treat the whole input as
// the diagnostic. parse() runs with exceptions off; nothing here can throw.
bool Decode(const std::string &err, std::string &d, std::string &u)
{
	if (err.empty() || err.front() != '{') {
		return false;
	}
	const json j = json::parse(err, nullptr, false);
	if (!j.is_object()) {
		return false;
	}
	const auto tag = j.find(kTag);
	if (tag == j.end() || !tag->is_number_integer() || tag->get<int>() != 1) {
		return false;
	}
	const auto dIt = j.find("d");
	if (dIt == j.end() || !dIt->is_string()) {
		return false;
	}
	d = dIt->get<std::string>();
	const auto uIt = j.find("u");
	u = (uIt != j.end() && uIt->is_string()) ? uIt->get<std::string>() : std::string();
	return true;
}

} // namespace

std::string User(const std::string &diagnostic, const std::string &userMessage)
{
	return Dump(json{{kTag, 1}, {"d", diagnostic}, {"u", userMessage}});
}

std::string Wrap(const std::string &prefix, const std::string &err)
{
	std::string d;
	std::string u;
	if (Decode(err, d, u)) {
		return Dump(json{{kTag, 1}, {"d", prefix + d}, {"u", u}});
	}
	return prefix + err;
}

std::string Diagnostic(const std::string &err)
{
	std::string d;
	std::string u;
	return Decode(err, d, u) ? d : err;
}

std::string UserMessage(const std::string &err)
{
	std::string d;
	std::string u;
	return Decode(err, d, u) ? u : std::string();
}

} // namespace Err
