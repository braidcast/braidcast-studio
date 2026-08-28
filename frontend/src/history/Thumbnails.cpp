#include "Thumbnails.hpp"

#include "SessionRecorder.hpp"

#include <obs.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../bridge.hpp"
#include "../multistream/CanvasRuntime.hpp"
#include "../multistream/MultistreamEngine.hpp"
#include "../multistream/StorePaths.hpp"
#include "../obs_bootstrap.hpp"

namespace History {

namespace {

// The canvas a candidate is grabbed from: the first one currently going out.
// Empty when nothing is live, which is the only case where a grab is skipped.
std::string FirstLiveCanvasUuid()
{
	if (!ObsBootstrap::MultistreamAlive()) {
		return {};
	}
	for (const MultistreamEngine::OutputStatus &st : ObsBootstrap::Multistream().Statuses()) {
		if (MultistreamEngine::IsActiveState(st.state)) {
			return st.canvasUuid;
		}
	}
	return {};
}

// Fit the source into kThumbLongEdge without distorting it. A source smaller
// than the cap is left alone rather than upscaled.
void FitLongEdge(uint32_t srcW, uint32_t srcH, uint32_t &outW, uint32_t &outH)
{
	outW = srcW;
	outH = srcH;
	const uint32_t longEdge = std::max(srcW, srcH);
	if (longEdge <= kThumbLongEdge || longEdge == 0) {
		return;
	}
	const double scale = static_cast<double>(kThumbLongEdge) / longEdge;
	outW = std::max<uint32_t>(1, static_cast<uint32_t>(srcW * scale));
	outH = std::max<uint32_t>(1, static_cast<uint32_t>(srcH * scale));
}

// Grab one frame of the given canvas as PNG bytes. Mirrors the two-branch canvas
// resolution the screenshot method uses: CanvasRuntime holds only the additional
// canvases, so a null Find means the Default, whose mix is the global one.
// Resolved per grab -- a canvas's mix can be dropped and rebuilt while inactive.
bool GrabPng(const std::string &canvasUuid, std::vector<unsigned char> &png, std::vector<uint8_t> &bgra,
	     std::string &err)
{
	uint32_t srcW = 0;
	uint32_t srcH = 0;
	std::function<void()> renderFn;

	obs_canvas_t *cv = ObsBootstrap::CanvasRuntime().Find(canvasUuid);
	if (cv) {
		obs_video_info ovi;
		if (!obs_canvas_get_video_info(cv, &ovi)) {
			err = "canvas has no video";
			return false;
		}
		srcW = ovi.base_width;
		srcH = ovi.base_height;
		renderFn = [cv]() {
			obs_canvas_render(cv);
		};
	} else {
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			err = "no video";
			return false;
		}
		srcW = ovi.base_width;
		srcH = ovi.base_height;
		renderFn = []() {
			obs_render_main_texture();
		};
	}

	uint32_t outW = 0;
	uint32_t outH = 0;
	FitLongEdge(srcW, srcH, outW, outH);
	if (!Bridge::RenderToBgraPixels(srcW, srcH, outW, outH, renderFn, true, bgra, err)) {
		return false;
	}
	return Bridge::EncodePngMemory(bgra.data(), outW, outH, png, err);
}

} // namespace

std::string ThumbnailDir()
{
	const std::string dir = MultistreamBasicPath("thumbnails");
	os_mkdirs(dir.c_str());
	return dir;
}

bool IsBlank(const std::vector<uint8_t> &bgra)
{
	if (bgra.size() < 4) {
		return true;
	}
	double sum = 0.0;
	const size_t pixels = bgra.size() / 4;
	for (size_t i = 0; i < pixels; i++) {
		// B,G,R,A -- the Rec.709 weights are per-channel, so they follow the bytes.
		const uint8_t *p = &bgra[i * 4];
		sum += 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];
	}
	return (sum / static_cast<double>(pixels)) < kBlankLumaThreshold;
}

void ThumbnailSampler::OnTick(int64_t sessionElapsedMs, const std::string &sessionId)
{
	if (sessionId.empty()) {
		return;
	}
	const bool firstDue = !haveFirst_ && sessionElapsedMs >= kFirstGrabMs;
	const bool intervalDue = haveFirst_ && sessionElapsedMs - lastGrabMs_ >= kThumbIntervalMs &&
				 static_cast<int>(candidates_.size()) < kMaxCandidates;
	if (!firstDue && !intervalDue) {
		return;
	}
	const std::string canvasUuid = FirstLiveCanvasUuid();
	if (canvasUuid.empty()) {
		return;
	}

	std::vector<unsigned char> png;
	std::vector<uint8_t> bgra;
	std::string err;
	if (!GrabPng(canvasUuid, png, bgra, err)) {
		// A failed grab costs one candidate, never the broadcast. Do not mark
		// the tick as taken, so the next one retries.
		return;
	}

	const std::string name = sessionId + "-" + std::to_string(candidates_.size()) + ".png";
	const std::string path = ThumbnailDir() + "/" + name;
	std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
	if (!f) {
		return;
	}
	f.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
	if (!f) {
		return;
	}
	f.close();

	candidates_.push_back(Candidate{name, IsBlank(bgra)});
	lastGrabMs_ = sessionElapsedMs;
	haveFirst_ = true;
}

void ThumbnailSampler::Finalize(SessionRecorder &recorder)
{
	if (candidates_.empty()) {
		Reset();
		return;
	}
	const std::string dir = ThumbnailDir();

	// Walk backwards: the newest non-blank frame is the one that looks like what
	// the stream became. An all-blank run falls back to the last candidate -- an
	// image of a dark stream beats no image.
	size_t keep = candidates_.size() - 1;
	for (size_t i = candidates_.size(); i-- > 0;) {
		if (!candidates_[i].blank) {
			keep = i;
			break;
		}
	}

	for (size_t i = 0; i < candidates_.size(); i++) {
		if (i == keep) {
			continue;
		}
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(dir + "/" + candidates_[i].name), ec);
	}
	// Relative to ThumbnailDir(): an absolute path breaks the moment a portable
	// install moves, and portable installs are a supported mode here.
	recorder.SetThumbnail(candidates_[keep].name);
	Reset();
}

void ThumbnailSampler::Reset()
{
	candidates_.clear();
	lastGrabMs_ = 0;
	haveFirst_ = false;
}

} // namespace History
