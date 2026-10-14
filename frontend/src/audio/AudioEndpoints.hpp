#ifndef OBS_MULTISTREAM_FRONTEND_AUDIO_ENDPOINTS_HPP_
#define OBS_MULTISTREAM_FRONTEND_AUDIO_ENDPOINTS_HPP_

#include <functional>
#include <string>
#include <vector>

// Windows audio endpoints, in one place: the ducking control Advanced settings owns,
// and the device comparison voice control needs. Both open an IMMDeviceEnumerator, and
// one module opening it is better than two. UI thread (COM is initialized there).
namespace AudioEndpoints {

// Opt this process's default-render audio session out of (or back into) Windows'
// automatic ducking. On Windows this maps to IAudioSessionControl2::SetDuckingPreference;
// on other platforms it is a no-op.
void DisableAudioDucking(bool disable);

// The id of the current default render (playback) endpoint, eRender/eConsole as libobs
// picks it, or empty if there is none.
std::string DefaultRenderDeviceId();

// OBS stores "default" rather than an id in both the monitoring device setting and a
// Desktop Audio source's device_id, so a string comparison alone would miss the case
// where both mean the same endpoint. This resolves "default" (and the empty string) to
// the real id and passes anything else through, unknown ids included.
std::string ResolveRenderDeviceId(const std::string &id);

// True when the audio monitoring device is an endpoint some output-capture source in
// the collection captures: the case where a monitor-only sound still reaches the stream.
// `captureName` receives that source's name ("" when false). Reads the sources with
// OutputCaptures and resolves the device ids after, so no COM call runs under libobs's
// sources mutex: endpoint enumeration can block on the audio service, and every thread
// that creates, destroys or enumerates a source waits on that mutex meanwhile.
bool MonitorSharesCapturedDevice(std::string &captureName);

// An output-capture source as stored: its name and its device_id ("default" kept as is).
struct OutputCapture {
	std::string name;
	std::string deviceId;
};
// Every output capture in the collection, whichever scene it sits in or none (the
// Desktop Audio sources on the global channels count too). Only reads them, under the
// sources mutex obs_enum_sources holds; no device is resolved here.
std::vector<OutputCapture> OutputCaptures();
// The name of the first capture whose device, through `resolve`, is `monitor` (already
// resolved), or "". A capture with no name reads "Desktop Audio". Pure.
std::string CaptureOfMonitor(const std::string &monitor, const std::vector<OutputCapture> &captures,
			     const std::function<std::string(const std::string &)> &resolve);

} // namespace AudioEndpoints

#endif // OBS_MULTISTREAM_FRONTEND_AUDIO_ENDPOINTS_HPP_
