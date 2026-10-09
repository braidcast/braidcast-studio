#include "voice/VoiceModels.hpp"

#include "bridge.hpp"
#include "event_names.hpp"
#include "log.hpp"
#include "multistream/StorePaths.hpp"
#include "util/async_task.hpp"
#include "util/file_util.hpp"
#include "util/http_client.hpp"
#include "util/sha256.hpp"
#include "util/time_util.hpp"
#include "voice/VoiceEngine.hpp"
#include "voice/VoiceP0Defaults.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace Voice {

namespace fs = std::filesystem;

namespace {

#define BRAIDCAST_HF_WHISPER "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"

constexpr ModelInfo kModels[] = {
	{"tiny.en-q5_1", "ggml-tiny.en-q5_1.bin", BRAIDCAST_HF_WHISPER "ggml-tiny.en-q5_1.bin", 32166155ull,
	 "c77c5766f1cef09b6b7d47f21b546cbddd4157886b3b5d6d4f709e91e66c7c2b", "Tiny (English, fastest)",
	 ModelKind::Speech, false},
	{"base.en-q5_1", "ggml-base.en-q5_1.bin", BRAIDCAST_HF_WHISPER "ggml-base.en-q5_1.bin", 59721011ull,
	 "4baf70dd0d7c4247ba2b81fafd9c01005ac77c2f9ef064e00dcf195d0e2fdd2f", "Base (English)", ModelKind::Speech,
	 false},
	{"small.en-q5_1", "ggml-small.en-q5_1.bin", BRAIDCAST_HF_WHISPER "ggml-small.en-q5_1.bin", 190098681ull,
	 "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30", "Small (English, higher accuracy)",
	 ModelKind::Speech, false},
	{"base-q5_1", "ggml-base-q5_1.bin", BRAIDCAST_HF_WHISPER "ggml-base-q5_1.bin", 59707625ull,
	 "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898", "Base (multilingual)", ModelKind::Speech,
	 true},
	{"silero-v5.1.2", "ggml-silero-v5.1.2.bin",
	 "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin", 885098ull,
	 "29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf", "Voice activity detector", ModelKind::Vad,
	 false},
};

#undef BRAIDCAST_HF_WHISPER

const ModelInfo *const kCatalog[] = {&kModels[0], &kModels[1], &kModels[2], &kModels[3], &kModels[4]};
static_assert(std::size(kCatalog) == std::size(kModels), "every model must be listed in kCatalog");

constexpr const char *kVerifiedSuffix = ".verified";
constexpr const char *kPartSuffix = ".part";
constexpr int kConnectTimeoutSec = 30;
constexpr int64_t kEmitIntervalMs = 250;

std::string MarkerPath(const std::string &finalPath)
{
	return finalPath + kVerifiedSuffix;
}

} // namespace

const ModelInfo *const *ModelCatalog(size_t &count)
{
	count = std::size(kCatalog);
	return kCatalog;
}

const ModelInfo *FindModel(const std::string &id)
{
	for (const ModelInfo *m : kCatalog) {
		if (id == m->id) {
			return m;
		}
	}
	return nullptr;
}

bool IsSelectableModel(const std::string &id)
{
	const ModelInfo *m = FindModel(id);
	if (!m || m->kind != ModelKind::Speech || m->multilingual) {
		return false;
	}
	return id != "small.en-q5_1" || P0::kOfferSmallModel;
}

std::string ModelsDir()
{
	const std::string dir = BraidcastConfigPath("voice/models");
	std::error_code ec;
	fs::create_directories(fs::u8path(dir), ec);
	return dir;
}

std::string ModelPath(const ModelInfo &m)
{
	return ModelsDir() + "/" + m.file;
}

bool ModelFilePresent(const ModelInfo &m)
{
	std::error_code ec;
	const auto size = fs::file_size(fs::u8path(ModelPath(m)), ec);
	return !ec && size == m.bytes;
}

bool VerifyFile(const std::string &utf8Path, uint64_t bytes, const char *sha256Hex, std::string &error,
		const std::atomic<bool> *cancel)
{
	std::error_code ec;
	const auto size = fs::file_size(fs::u8path(utf8Path), ec);
	if (ec || size != bytes) {
		error = "size mismatch (expected " + std::to_string(bytes) + " bytes)";
		return false;
	}
	std::string hex;
	if (!Sha256::FileHex(utf8Path, hex, cancel)) {
		error = "could not read the file to verify it";
		return false;
	}
	if (hex != sha256Hex) {
		error = "hash mismatch (the file is not the pinned model)";
		return false;
	}
	return true;
}

bool CommitDownload(const std::string &partPath, const std::string &finalPath, const std::string &sha256Hex,
		    std::string &error)
{
	std::error_code ec;
	fs::rename(fs::u8path(partPath), fs::u8path(finalPath), ec);
	if (ec) {
		error = "could not move the download into place: " + ec.message();
		return false;
	}
	std::ofstream marker(fs::u8path(MarkerPath(finalPath)), std::ios::binary | std::ios::trunc);
	marker << sha256Hex;
	return true;
}

bool EnsureModelVerified(const ModelInfo &m, std::string &error, const std::atomic<bool> *cancel)
{
	const std::string path = ModelPath(m);
	if (!ModelFilePresent(m)) {
		error = std::string(m.label) + " model is not downloaded";
		return false;
	}
	std::string marker;
	if (FileUtil::ReadUtf8File(MarkerPath(path), marker) && marker == m.sha256) {
		return true;
	}
	if (!VerifyFile(path, m.bytes, m.sha256, error, cancel)) {
		error = std::string(m.label) + " model: " + error;
		return false;
	}
	std::ofstream out(fs::u8path(MarkerPath(path)), std::ios::binary | std::ios::trunc);
	out << m.sha256;
	return true;
}

bool ModelDownloader::Start(const std::string &id, std::string &error)
{
	const ModelInfo *m = FindModel(id);
	if (!m) {
		error = "unknown model '" + id + "'";
		return false;
	}
	std::vector<const ModelInfo *> queue;
	const ModelInfo *vad = FindModel(kVadModelId);
	if (m->kind == ModelKind::Speech && vad && !ModelFilePresent(*vad)) {
		queue.push_back(vad);
	}
	if (!ModelFilePresent(*m)) {
		queue.push_back(m);
	}
	if (queue.empty()) {
		error = std::string(m->label) + " is already downloaded";
		return false;
	}
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const ModelInfo *q : queue) {
			Progress &p = progress_[q->id];
			if (p.active) {
				error = std::string(q->label) + " is already downloading";
				return false;
			}
		}
		for (const ModelInfo *q : queue) {
			Progress &p = progress_[q->id];
			p = Progress{};
			p.cancel = cancel;
			p.active = true;
		}
	}
	for (const ModelInfo *q : queue) {
		Emit(*q);
	}
	AsyncTask::RunAsync([this, queue, cancel] { Run(queue, cancel); });
	return true;
}

void ModelDownloader::Run(std::vector<const ModelInfo *> queue, std::shared_ptr<std::atomic<bool>> cancel)
{
	for (const ModelInfo *m : queue) {
		if (!DownloadOne(*m, cancel)) {
			// A failed or cancelled dependency ends the queue: the speech model is useless
			// without the VAD, and the user sees the dependency's reason.
			std::lock_guard<std::mutex> lock(mutex_);
			for (const ModelInfo *rest : queue) {
				progress_[rest->id].active = false;
			}
			break;
		}
	}
	for (const ModelInfo *m : queue) {
		Emit(*m);
	}
	// A model that just arrived may be the one an enabled engine is waiting for. Posted:
	// the engine lives on the UI thread, and this is a download worker.
	AsyncTask::PostToUi([] { Engine().NoteModelsChanged(); });
}

bool ModelDownloader::DownloadOne(const ModelInfo &m, const std::shared_ptr<std::atomic<bool>> &cancel)
{
	const std::string finalPath = ModelPath(m);
	const std::string partPath = finalPath + kPartSuffix;
	HostLog(std::string("[voice] model download start: ") + m.id);
	std::ofstream out(fs::u8path(partPath), std::ios::binary | std::ios::trunc);
	Sha256::Hasher hasher;
	uint64_t received = 0;
	int64_t lastEmit = 0;
	bool writeFailed = !out;

	Http::HttpReq req;
	req.method = "GET";
	req.url = m.url;
	req.timeoutSec = kConnectTimeoutSec;
	req.followRedirects = true;
	req.cancel = cancel.get();
	std::string errorBody, error;
	const long status = Http::HttpRequestStreaming(
		req,
		[&](std::string_view chunk) {
			if (cancel->load(std::memory_order_acquire) || writeFailed) {
				return false;
			}
			out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
			if (!out) {
				writeFailed = true;
				return false;
			}
			hasher.Update(chunk.data(), chunk.size());
			received += chunk.size();
			const int64_t now = TimeUtil::NowMs();
			if (now - lastEmit >= kEmitIntervalMs) {
				lastEmit = now;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					progress_[m.id].received = received;
				}
				Emit(m);
			}
			return true;
		},
		errorBody, error);
	out.close();

	std::string failure;
	unsigned char digest[32];
	if (cancel->load(std::memory_order_acquire)) {
		failure = "cancelled";
	} else if (writeFailed) {
		failure = "could not write to " + ModelsDir();
	} else if (!error.empty()) {
		failure = "network error: " + error;
	} else if (status != 200) {
		failure = "server answered HTTP " + std::to_string(status);
	} else if (received != m.bytes) {
		failure =
			"size mismatch (got " + std::to_string(received) + " of " + std::to_string(m.bytes) + " bytes)";
	} else if (!hasher.Final(digest) || Sha256::ToHex(digest) != m.sha256) {
		failure = "hash mismatch (the download is not the pinned model)";
	} else if (!CommitDownload(partPath, finalPath, m.sha256, failure)) {
		// failure filled by CommitDownload
	}

	std::error_code ec;
	if (!failure.empty()) {
		fs::remove(fs::u8path(partPath), ec);
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		Progress &p = progress_[m.id];
		p.active = false;
		p.received = failure.empty() ? m.bytes : 0;
		p.error = failure == "cancelled" ? std::string() : failure;
	}
	HostLog(std::string("[voice] model download ") + m.id + ": " + (failure.empty() ? "done" : failure));
	return failure.empty();
}

bool ModelDownloader::Cancel(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = progress_.find(id);
	if (it == progress_.end() || !it->second.active || !it->second.cancel) {
		return false;
	}
	it->second.cancel->store(true, std::memory_order_release);
	return true;
}

void ModelDownloader::CancelAll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &kv : progress_) {
		if (kv.second.active && kv.second.cancel) {
			kv.second.cancel->store(true, std::memory_order_release);
		}
	}
}

nlohmann::json ModelDownloader::StatusFor(const ModelInfo &m) const
{
	Progress p;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = progress_.find(m.id);
		if (it != progress_.end()) {
			p = it->second;
		}
	}
	std::string state = "absent";
	if (p.active) {
		state = "downloading";
	} else if (ModelFilePresent(m)) {
		state = "ready";
	} else if (!p.error.empty()) {
		state = "failed";
	}
	nlohmann::json j = {
		{"id", m.id},
		{"label", m.label},
		{"kind", m.kind == ModelKind::Vad ? "vad" : "speech"},
		{"selectable", IsSelectableModel(m.id)},
		{"multilingual", m.multilingual},
		{"bytes", m.bytes},
		{"received", p.active ? p.received : (state == "ready" ? m.bytes : 0)},
		{"state", state},
	};
	if (state == "failed") {
		j["error"] = p.error;
	}
	return j;
}

nlohmann::json ModelDownloader::StatusJson() const
{
	nlohmann::json models = nlohmann::json::array();
	for (const ModelInfo *m : kCatalog) {
		models.push_back(StatusFor(*m));
	}
	return nlohmann::json{{"models", models}};
}

void ModelDownloader::Emit(const ModelInfo &m) const
{
	Bridge::EmitEvent(EventNames::kVoiceModelStatus, StatusFor(m));
}

ModelDownloader &Downloads()
{
	static ModelDownloader downloader;
	return downloader;
}

} // namespace Voice
