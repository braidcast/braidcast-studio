#pragma once

#include <util/windows/ComPtr.hpp>
#include <Mmdeviceapi.h>

#include <unordered_map>
#include <functional>
#include <mutex>

typedef std::function<void(EDataFlow, ERole, LPCWSTR)> WASAPINotifyDefaultDeviceChangedCallback;

class NotificationClient;

class WASAPINotify {
public:
	WASAPINotify();
	~WASAPINotify();

	void AddDefaultDeviceChangedCallback(void *handle, WASAPINotifyDefaultDeviceChangedCallback cb);
	void RemoveDefaultDeviceChangedCallback(void *handle);
	/* Hands one default-device change to the callback registered under `handle` alone, in
	 * turn with the real notifications. */
	void DeliverDefaultDeviceChanged(void *handle, EDataFlow flow, ERole role, LPCWSTR id);

private:
	void OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id);

	std::mutex mutex;

	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<NotificationClient> notificationClient;

	std::unordered_map<void *, WASAPINotifyDefaultDeviceChangedCallback> defaultDeviceChangedCallbacks;
};
