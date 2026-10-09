#include "audio/AudioEndpoints.hpp"

#include "util/text_encoding.hpp"

#include <obs.hpp>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <wrl/client.h>
#endif

#include <cstring>

void AudioEndpoints::DisableAudioDucking(bool disable)
{
#ifdef _WIN32
	using Microsoft::WRL::ComPtr;

	ComPtr<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				      IID_PPV_ARGS(enumerator.GetAddressOf()));
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: CoCreateInstance(MMDeviceEnumerator) failed (hr=0x%08lX)", hr);
		return;
	}

	ComPtr<IMMDevice> device;
	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: GetDefaultAudioEndpoint failed (hr=0x%08lX)", hr);
		return;
	}

	ComPtr<IAudioSessionManager2> sessionManager;
	hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
			      (void **)sessionManager.GetAddressOf());
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: IMMDevice::Activate(IAudioSessionManager2) failed (hr=0x%08lX)",
		     hr);
		return;
	}

	// A null AudioSessionGuid assigns the returned control to this process's default
	// audio session, which is how upstream OBS obtains the current process's session.
	ComPtr<IAudioSessionControl> sessionControl;
	hr = sessionManager->GetAudioSessionControl(nullptr, 0, sessionControl.GetAddressOf());
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: GetAudioSessionControl failed (hr=0x%08lX)", hr);
		return;
	}

	ComPtr<IAudioSessionControl2> sessionControl2;
	hr = sessionControl->QueryInterface(IID_PPV_ARGS(sessionControl2.GetAddressOf()));
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: QueryInterface(IAudioSessionControl2) failed (hr=0x%08lX)", hr);
		return;
	}

	hr = sessionControl2->SetDuckingPreference(disable ? TRUE : FALSE);
	if (FAILED(hr)) {
		blog(LOG_WARNING, "AdvancedSettings: SetDuckingPreference failed (hr=0x%08lX)", hr);
	}
#else
	(void)disable;
#endif
}

std::string AudioEndpoints::DefaultRenderDeviceId()
{
#ifdef _WIN32
	using Microsoft::WRL::ComPtr;

	// The same endpoint libobs means by "default" in both places that matter: the
	// monitoring output (wasapi-output.c) and a Desktop Audio capture
	// (win-wasapi.cpp) each ask for the eRender/eConsole default.
	ComPtr<IMMDeviceEnumerator> enumerator;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				      IID_PPV_ARGS(enumerator.GetAddressOf()));
	if (FAILED(hr)) {
		return std::string();
	}
	ComPtr<IMMDevice> device;
	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
	if (FAILED(hr)) {
		return std::string();
	}
	LPWSTR id = nullptr;
	if (FAILED(device->GetId(&id)) || !id) {
		return std::string();
	}
	const std::string utf8 = Encoding::WideToUtf8(id);
	CoTaskMemFree(id);
	return utf8;
#else
	return std::string();
#endif
}

std::string AudioEndpoints::ResolveRenderDeviceId(const std::string &id)
{
	if (id.empty() || id == "default") {
		const std::string resolved = DefaultRenderDeviceId();
		return resolved.empty() ? id : resolved;
	}
	return id;
}

bool AudioEndpoints::MonitorSharesCapturedDevice(std::string &captureName)
{
	captureName.clear();

	const char *monitorName = nullptr;
	const char *monitorId = nullptr;
	obs_get_audio_monitoring_device(&monitorName, &monitorId);
	if (!monitorId) {
		return false;
	}
	struct Search {
		std::string monitor;
		std::string found;
	} search{ResolveRenderDeviceId(monitorId), {}};
	if (search.monitor.empty()) {
		return false;
	}

	// Every output capture in the collection, whichever scene it sits in or none: the
	// Desktop Audio sources on the global channels count too.
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *s = static_cast<Search *>(param);
			const char *type = obs_source_get_id(source);
			if (!type || std::strcmp(type, "wasapi_output_capture") != 0) {
				return true;
			}
			OBSDataAutoRelease settings = obs_source_get_settings(source);
			const char *device = settings ? obs_data_get_string(settings, "device_id") : nullptr;
			if (device && ResolveRenderDeviceId(device) == s->monitor) {
				const char *name = obs_source_get_name(source);
				s->found = name && *name ? name : "Desktop Audio";
				return false;
			}
			return true;
		},
		&search);

	captureName = search.found;
	return !captureName.empty();
}
