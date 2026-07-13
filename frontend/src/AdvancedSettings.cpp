#include "AdvancedSettings.hpp"

#include "log.hpp"
#include "multistream/StorePaths.hpp"

#include <obs.hpp>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <wrl/client.h>
#endif

void AdvancedSettings::Load()
{
	OBSDataAutoRelease root =
		obs_data_create_from_json_file_safe(MultistreamBasicPath("advanced.json").c_str(), "bak");
	if (!root) {
		return; // no file yet: keep struct defaults
	}
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		obs_data_set_default_bool(root, f.file, this->*f.member);
		this->*f.member = obs_data_get_bool(root, f.file);
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		obs_data_set_default_string(root, f.file, (this->*f.member).c_str());
		this->*f.member = obs_data_get_string(root, f.file);
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		obs_data_set_default_int(root, f.file, this->*f.member);
		int64_t v = obs_data_get_int(root, f.file);
		v = v < (int64_t)f.min ? (int64_t)f.min : (v > (int64_t)f.max ? (int64_t)f.max : v);
		this->*f.member = (uint32_t)v;
	}
}

void AdvancedSettings::Save() const
{
	OBSDataAutoRelease root = obs_data_create();
	for (const AdvancedBoolField &f : kAdvancedBoolFields) {
		obs_data_set_bool(root, f.file, this->*f.member);
	}
	for (const AdvancedStringField &f : kAdvancedStringFields) {
		obs_data_set_string(root, f.file, (this->*f.member).c_str());
	}
	for (const AdvancedUIntField &f : kAdvancedUIntFields) {
		obs_data_set_int(root, f.file, this->*f.member);
	}

	SaveJsonAtomic(root, MultistreamBasicPath("advanced.json"));
}

void ApplyProcessPriority(const std::string &token)
{
#ifdef _WIN32
	static const struct {
		const char *token;
		DWORD cls;
	} kClasses[] = {
		{"normal", NORMAL_PRIORITY_CLASS},
		{"aboveNormal", ABOVE_NORMAL_PRIORITY_CLASS},
		{"high", HIGH_PRIORITY_CLASS},
	};
	for (const auto &c : kClasses) {
		if (token == c.token) {
			SetPriorityClass(GetCurrentProcess(), c.cls);
			return;
		}
	}
#else
	(void)token;
#endif
}

void DisableAudioDucking(bool disable)
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
