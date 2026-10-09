#ifndef OBS_MULTISTREAM_FRONTEND_VOICE_MODELS_HPP_
#define OBS_MULTISTREAM_FRONTEND_VOICE_MODELS_HPP_

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Voice {

enum class ModelKind { Speech, Vad };

// One downloadable model. Hash-pinned: a download (or a hand-copied file) is used
// only if its byte count and SHA-256 both match this entry.
struct ModelInfo {
	const char *id;
	const char *file;
	const char *url;
	uint64_t bytes;
	const char *sha256;
	const char *label;
	ModelKind kind;
	bool multilingual;
};

inline constexpr const char *kVadModelId = "silero-v5.1.2";
inline constexpr const char *kWakeModelId = "tiny.en-q5_1";
inline constexpr const char *kMultilingualModelId = "base-q5_1";

const ModelInfo *const *ModelCatalog(size_t &count);
const ModelInfo *FindModel(const std::string &id);
// The speech models offered for `language` (a VoiceLanguages code). English gets the
// English-only models (small.en only when P0 said it fits), which are faster and better
// at English; every other language needs the multilingual one, which is why it is
// otherwise hidden.
bool IsSelectableModel(const std::string &id, const std::string &language);
// The model to use for `language` when the current one does not fit it: the P0 default
// for English, the multilingual model for anything else.
std::string ModelForLanguage(const std::string &language);

// <config>/voice/models, created on demand.
std::string ModelsDir();
std::string ModelPath(const ModelInfo &m);
// The file exists with the catalog's byte count (cheap; no hashing).
bool ModelFilePresent(const ModelInfo &m);
// Size and SHA-256 check of an arbitrary file. Hashes the whole file.
bool VerifyFile(const std::string &utf8Path, uint64_t bytes, const char *sha256Hex, std::string &error,
		const std::atomic<bool> *cancel);
// Rename a verified `.part` onto its final name and write "<final>.verified" holding
// the hash, so later loads skip re-hashing. Replaces an existing final file.
bool CommitDownload(const std::string &partPath, const std::string &finalPath, const std::string &sha256Hex,
		    std::string &error);
// Worker thread (model load): true when the file is present and verified, hashing it
// once if it has no marker yet (a hand-copied model). A mismatch reports why.
bool EnsureModelVerified(const ModelInfo &m, std::string &error, const std::atomic<bool> *cancel);

// Downloads, one worker per request (AsyncTask::RunAsync). Downloading a speech model
// first fetches the VAD model if it is missing, since every speech model needs it.
// Progress is pushed as voice.model.status events (throttled to 4 Hz).
class ModelDownloader {
public:
	// UI thread. False with `error` if the id is unknown, already downloading, or
	// already present.
	bool Start(const std::string &id, std::string &error);
	// Any thread. True if a download for `id` was running and is now told to stop.
	bool Cancel(const std::string &id);
	// Bridge::Shutdown, before the async drain.
	void CancelAll();
	// {models: [status...]} for voice.model.status. `selectable` is for the language
	// SetLanguage last named.
	nlohmann::json StatusJson() const;
	nlohmann::json StatusFor(const ModelInfo &m) const;
	// UI thread (the engine, as its settings change). Stored under the downloader's own
	// mutex, because download workers build statuses too.
	void SetLanguage(const std::string &language);

private:
	struct Progress {
		std::shared_ptr<std::atomic<bool>> cancel;
		uint64_t received = 0;
		std::string error; // last failure; cleared when a new download starts
		bool active = false;
	};

	void Run(std::vector<const ModelInfo *> queue, std::shared_ptr<std::atomic<bool>> cancel);
	bool DownloadOne(const ModelInfo &m, const std::shared_ptr<std::atomic<bool>> &cancel);
	void Emit(const ModelInfo &m) const;

	mutable std::mutex mutex_;
	std::map<std::string, Progress> progress_;
	std::string language_ = "en";
};

// Process-wide downloader (function-local static, so it outlives detached workers).
ModelDownloader &Downloads();

} // namespace Voice

#endif // OBS_MULTISTREAM_FRONTEND_VOICE_MODELS_HPP_
