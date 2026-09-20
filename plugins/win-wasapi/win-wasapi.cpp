#include "wasapi-notify.hpp"
#include "enum-wasapi.hpp"

#include <obs-module.h>
#include <obs.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/windows/HRError.hpp>
#include <util/windows/ComPtr.hpp>
#include <util/windows/WinHandle.hpp>
#include <util/windows/CoTaskMemPtr.hpp>
#include <util/windows/win-version.h>
#include <util/windows/window-helpers.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <atomic>
#include <cinttypes>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <RTWorkQ.h>
#include <wrl/implements.h>

using namespace std;

#define OPT_DEVICE_ID "device_id"
#define OPT_USE_DEVICE_TIMING "use_device_timing"
#define OPT_WINDOW "window"
#define OPT_PRIORITY "priority"

WASAPINotify *GetNotify();
static void GetWASAPIDefaults(obs_data_t *settings);

#define OBS_KSAUDIO_SPEAKER_4POINT1 (KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)

typedef HRESULT(STDAPICALLTYPE *PFN_ActivateAudioInterfaceAsync)(LPCWSTR, REFIID, PROPVARIANT *,
								 IActivateAudioInterfaceCompletionHandler *,
								 IActivateAudioInterfaceAsyncOperation **);

typedef HRESULT(STDAPICALLTYPE *PFN_RtwqUnlockWorkQueue)(DWORD);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqLockSharedWorkQueue)(PCWSTR usageClass, LONG basePriority, DWORD *taskId,
							     DWORD *id);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqCreateAsyncResult)(IUnknown *, IRtwqAsyncCallback *, IUnknown *,
							   IRtwqAsyncResult **);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqPutWorkItem)(DWORD, LONG, IRtwqAsyncResult *);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqPutWaitingWorkItem)(HANDLE, LONG, IRtwqAsyncResult *, RTWQWORKITEM_KEY *);

class WASAPIActivateAudioInterfaceCompletionHandler
	: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
					      Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
	IUnknown *unknown;
	HRESULT activationResult;
	WinHandle activationSignal;

public:
	WASAPIActivateAudioInterfaceCompletionHandler();
	HRESULT GetActivateResult(IAudioClient **client);

private:
	virtual HRESULT STDMETHODCALLTYPE
	ActivateCompleted(IActivateAudioInterfaceAsyncOperation *activateOperation) override final;
};

WASAPIActivateAudioInterfaceCompletionHandler::WASAPIActivateAudioInterfaceCompletionHandler()
{
	activationSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!activationSignal.Valid()) {
		throw "Could not create receive signal";
	}
}

HRESULT
WASAPIActivateAudioInterfaceCompletionHandler::GetActivateResult(IAudioClient **client)
{
	WaitForSingleObject(activationSignal, INFINITE);
	*client = static_cast<IAudioClient *>(unknown);
	return activationResult;
}

HRESULT
WASAPIActivateAudioInterfaceCompletionHandler::ActivateCompleted(
	IActivateAudioInterfaceAsyncOperation *activateOperation)
{
	HRESULT hr, hr_activate;
	hr = activateOperation->GetActivateResult(&hr_activate, &unknown);
	hr = SUCCEEDED(hr) ? hr_activate : hr;
	activationResult = hr;

	SetEvent(activationSignal);
	return hr;
}

enum class SourceType {
	Input,
	DeviceOutput,
	ProcessOutput,
};

class RtwqSourceLink;

/* Both duplicates this file takes stay in this process: the copy is what outlives the settings
 * value or the member handle it came from. */
static bool DuplicateInProcess(HANDLE source, HANDLE *dup)
{
	*dup = NULL;
	if (!source) {
		SetLastError(ERROR_INVALID_HANDLE);
		return false;
	}
	return DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), dup, 0, FALSE,
			       DUPLICATE_SAME_ACCESS) != FALSE;
}

/* Self-test seam for the create-then-release race in OnStartCapture. Source settings can
 * come from saved or imported data, so they alone never arm it: the settings must also carry
 * a nonce equal to SELFTEST_NONCE_ENV in the process environment, which the self-test mints
 * at run time and clears once the source is created, and FE_SMOKE_QUIT_SECONDS must be set
 * there too. Both are read from the Win32 environment block, never from a file. The names are
 * mirrored by the kStartRace* constants in frontend/src/obs_bootstrap.cpp, which the start-race
 * and restart self-tests share through CreateProbedCapture. */
#define SELFTEST_SMOKE_ENV "FE_SMOKE_QUIT_SECONDS"
#define SELFTEST_NONCE_ENV "BRAIDCAST_SELFTEST_WASAPI_NONCE"
#define OPT_SELFTEST_NONCE "selftest_start_race_nonce"
#define OPT_SELFTEST_IN_WINDOW "selftest_start_race_in_window"
#define OPT_SELFTEST_IDLE_LATE "selftest_start_race_idle_late"
#define OPT_SELFTEST_HELD "selftest_start_race_held"
#define OPT_SELFTEST_INIT_DONE "selftest_start_race_init_done"
#define OPT_SELFTEST_WAIT_MS "selftest_start_race_wait_ms"
#define OPT_SELFTEST_STATUS "selftest_start_race_status"

class StartRaceProbe {
	static constexpr long long kMinWaitMs = 100;
	static constexpr long long kMaxWaitMs = 60000;
	static constexpr DWORD kStartBoundMs = 60000;

	WinHandle inWindow;
	WinHandle idleLate;
	WinHandle held;
	WinHandle initDone;
	WinHandle stopWoke;
	WinHandle startEnded;
	DWORD waitMs = 0;

	static bool ProcessEnv(const char *name, string &value)
	{
		char buf[128];
		SetLastError(ERROR_SUCCESS);
		const DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
		if (len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
			return false;
		}
		if (len >= sizeof(buf)) {
			return false;
		}
		value.assign(buf, len);
		return true;
	}

	static bool InvokedBySelfTest(obs_data_t *settings)
	{
		string smoke;
		string nonce;
		if (!ProcessEnv(SELFTEST_SMOKE_ENV, smoke) || !ProcessEnv(SELFTEST_NONCE_ENV, nonce) || nonce.empty()) {
			return false;
		}
		return nonce == obs_data_get_string(settings, OPT_SELFTEST_NONCE);
	}

	static HANDLE Duplicate(obs_data_t *settings, const char *key)
	{
		HANDLE dup = NULL;
		DuplicateInProcess((HANDLE)(intptr_t)obs_data_get_int(settings, key), &dup);
		return dup;
	}

	static std::unique_ptr<StartRaceProbe> Refuse(obs_data_t *settings, const char *status)
	{
		obs_data_set_string(settings, OPT_SELFTEST_STATUS, status);
		blog(LOG_WARNING, "WASAPI: start-race probe not armed: %s", status);
		return nullptr;
	}

public:
	/* Stands in for the audio engine's event, so no capture event can ever arrive:
	 * the behavior of a loopback endpoint that renders nothing. */
	WinHandle silentEndpoint;

	/* Reports its outcome in OPT_SELFTEST_STATUS once the invocation is proven, so the
	 * self-test can tell a refusal from a capture that never started. */
	static std::unique_ptr<StartRaceProbe> FromSettings(obs_data_t *settings, bool rtwqSupported)
	{
		if (!obs_data_has_user_value(settings, OPT_SELFTEST_NONCE)) {
			return nullptr;
		}
		if (!InvokedBySelfTest(settings)) {
			blog(LOG_WARNING, "WASAPI: start-race probe settings ignored: not this self-test's invocation");
			return nullptr;
		}

		/* Only the RTWorkQ path can lose the wake-up; on the capture-thread path the case
		 * could never fail, so a pass there would prove nothing. */
		if (!rtwqSupported) {
			return Refuse(settings, "no-rtwq");
		}

		const long long waitMs = obs_data_get_int(settings, OPT_SELFTEST_WAIT_MS);
		if (waitMs < kMinWaitMs || waitMs > kMaxWaitMs) {
			return Refuse(settings, "invalid-wait-ms");
		}

		auto probe = std::make_unique<StartRaceProbe>();
		probe->inWindow = Duplicate(settings, OPT_SELFTEST_IN_WINDOW);
		probe->idleLate = Duplicate(settings, OPT_SELFTEST_IDLE_LATE);
		probe->held = Duplicate(settings, OPT_SELFTEST_HELD);
		probe->initDone = Duplicate(settings, OPT_SELFTEST_INIT_DONE);
		probe->stopWoke = CreateEvent(nullptr, true, false, nullptr);
		probe->startEnded = CreateEvent(nullptr, true, false, nullptr);
		probe->silentEndpoint = CreateEvent(nullptr, false, false, nullptr);
		probe->waitMs = (DWORD)waitMs;
		if (!probe->inWindow.Valid() || !probe->idleLate.Valid() || !probe->held.Valid() ||
		    !probe->initDone.Valid() || !probe->stopWoke.Valid() || !probe->startEnded.Valid() ||
		    !probe->silentEndpoint.Valid()) {
			return Refuse(settings, "setup-failed");
		}

		obs_data_set_string(settings, OPT_SELFTEST_STATUS, "armed");
		return probe;
	}

	/* Called from Initialize just before it resets receiveSignal: parks there until
	 * Stop() has sent its wake-up, so the reset always lands after it. `held` is set only
	 * when that ordering was achieved, and before the reset, so any idleSignal that
	 * follows implies it is already visible. A hold that times out leaves it unset and
	 * the case is not exercised. */
	bool HoldUntilStopWakes()
	{
		SetEvent(inWindow);
		if (WaitForSingleObject(stopWoke, waitMs) != WAIT_OBJECT_0) {
			return false;
		}
		SetEvent(held);
		return true;
	}

	/* Called at the end of a successful Initialize that held, once the sample-ready work
	 * item is armed: an Initialize that throws after the hold never exercises the lost
	 * wake-up, and its reconnect path must not read as a pass. */
	void MarkInitDone() { SetEvent(initDone); }

	/* Set as the last act of OnStartCapture, on every path. */
	HANDLE StartEndedEvent() const { return startEnded; }

	/* Called from Stop() after its wake-up. Waits for OnStartCapture to have returned,
	 * so nothing is judged, or woken, while it can still touch the source. Only a held
	 * Initialize that completed is judged: if idleSignal does not follow it within the
	 * bound, reports that and delivers the wake a late capture event would, which the
	 * sample handler answers by tearing the client down and setting idleSignal. Every
	 * other path resolves Stop() on its own. A start that never returns is left alone:
	 * nothing can safely wake it, and the self-test reports the stalled destroy. */
	void AwaitIdle(HANDLE idleSignal, HANDLE receiveSignal)
	{
		SetEvent(stopWoke);
		if (WaitForSingleObject(startEnded, kStartBoundMs) != WAIT_OBJECT_0) {
			return;
		}
		if (WaitForSingleObject(initDone, 0) != WAIT_OBJECT_0) {
			return;
		}
		if (WaitForSingleObject(idleSignal, waitMs) == WAIT_TIMEOUT) {
			SetEvent(idleLate);
			SetEvent(receiveSignal);
		}
	}
};

/* Keeps one started render stream of silence on a loopback capture's endpoint.
 *
 * Measured, on a render endpoint with no other render session open: the loopback capture
 * receives no packets at all, not silent ones. libobs keeps its own timeline, so its expected
 * timestamp falls behind wall clock for the whole idle stretch, then it buffers to its cap and
 * restarts the source's audio. Holding one started render session on the endpoint for as long
 * as the capture is open is what keeps packets arriving; without it the same endpoint delivers
 * zero packets (see the loopback-silence self-test, which fails in exactly that shape).
 *
 * Self-contained by design: its thread creates, refills and releases every COM object it
 * touches, so nothing crosses an apartment, and it reads no WASAPISource state. Every failure
 * is one log line and a capture that behaves exactly as it did before this existed. */
class SilentRenderKeepalive {
	/* Refill cadence. The client buffer is BUFFER_TIME_100NS long and each pass tops up to
	 * full against GetCurrentPadding rather than adding a fixed amount, so a late pass costs
	 * nothing and only starvation longer than the whole buffer can underrun it. Waking on the
	 * render client's own event would refill an order of magnitude more often to the same end,
	 * and would add a third handle to a teardown path this file has already been bitten by. */
	static constexpr DWORD kRefillMs = 500;
	/* Retry cadence after a failed open, so an endpoint another app holds in exclusive mode is
	 * retried without spinning. */
	static constexpr DWORD kRetryMs = 5000;
	/* Consecutive refill passes finding the buffer still full -- the engine having consumed
	 * nothing -- before the client is stopped and reopened. Every documented way an engine stops
	 * consuming invalidates the stream, which the failure path already catches; a driver that
	 * simply stops servicing the client does not, and Refill succeeds against a frozen buffer
	 * either way. So this is the only thing that would ever notice the keepalive reporting
	 * healthy while the endpoint idles -- the defect it exists to prevent, with a clean log.
	 * Spans the whole client buffer in kRefillMs passes, so no healthy cadence can reach it
	 * (asserted against BUFFER_TIME_100NS in Run). */
	static constexpr int kStalledPassLimit = 10;

	std::mutex lock;
	wstring wanted;

	WinHandle retargetSignal;
	WinHandle exitSignal;
	WinHandle thread;
	bool reportedRefusal = false;

	static HRESULT OpenClient(IMMDeviceEnumerator *enumerator, const wstring &deviceId,
				  ComPtr<IAudioClient> &client, ComPtr<IAudioRenderClient> &render,
				  UINT32 &bufferFrames, UINT32 &blockAlign);
	/* `wroteFrames` reports whether the buffer had room for any, i.e. whether the engine
	 * consumed anything since the last pass. See kStalledPassLimit. */
	static HRESULT Refill(IAudioClient *client, IAudioRenderClient *render, UINT32 bufferFrames, UINT32 blockAlign,
			      bool &wroteFrames);

	static DWORD WINAPI ThreadProc(LPVOID param);
	void Run();

public:
	SilentRenderKeepalive();
	~SilentRenderKeepalive() { Stop(); }

	/* Points the keepalive at `deviceId`, starting its thread on the first call. Safe to call
	 * on every capture start: a restart retargets the one thread instead of adding a client. */
	void Start(const wstring &deviceId);

	/* Stops the render stream and joins the thread. Idempotent. */
	void Stop();
};

class WASAPISource {
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;

	obs_source_t *source;
	obs_weak_source_t *reroute_target = nullptr;
	wstring default_id;
	string device_id;
	string device_name;
	WinModule mmdevapi_module;
	PFN_ActivateAudioInterfaceAsync activate_audio_interface_async = NULL;
	PFN_RtwqUnlockWorkQueue rtwq_unlock_work_queue = NULL;
	PFN_RtwqLockSharedWorkQueue rtwq_lock_shared_work_queue = NULL;
	PFN_RtwqCreateAsyncResult rtwq_create_async_result = NULL;
	PFN_RtwqPutWorkItem rtwq_put_work_item = NULL;
	PFN_RtwqPutWaitingWorkItem rtwq_put_waiting_work_item = NULL;
	bool rtwq_supported = false;
	window_priority priority;
	string window_class;
	string title;
	string executable;
	HWND hwnd = NULL;
	DWORD process_id = 0;
	const SourceType sourceType;
	std::atomic<bool> useDeviceTiming = false;
	std::atomic<bool> isDefaultDevice = false;
	std::atomic<bool> sawBadTimestamp = false;
	bool hooked = false;

	bool previouslyFailed = false;
	WinHandle reconnectThread = NULL;

	DWORD rtwqQueueId = 0;
	std::shared_ptr<RtwqSourceLink> rtwqLink;
	ComPtr<IRtwqAsyncResult> startCaptureAsyncResult;
	ComPtr<IRtwqAsyncResult> sampleReadyAsyncResult;

	WinHandle captureThread;
	WinHandle idleSignal;
	WinHandle stopSignal;
	WinHandle receiveSignal;
	WinHandle restartSignal;
	WinHandle reconnectExitSignal;
	WinHandle exitSignal;
	WinHandle initSignal;
	DWORD reconnectDuration = 0;
	WinHandle reconnectSignal;

	std::unique_ptr<StartRaceProbe> startRaceProbe;

	/* Non-null for SourceType::DeviceOutput only, which is the only type that captures a
	 * render endpoint whose engine can go idle. */
	std::unique_ptr<SilentRenderKeepalive> keepalive;

	speaker_layout speakers;
	audio_format format;
	uint32_t sampleRate;

	vector<BYTE> silence;

	static DWORD WINAPI ReconnectThread(LPVOID param);
	static DWORD WINAPI CaptureThread(LPVOID param);

	bool ProcessCaptureData();

	void Start();
	void Stop();

	static ComPtr<IMMDevice> InitDevice(IMMDeviceEnumerator *enumerator, bool isDefaultDevice, SourceType type,
					    const string device_id);
	static ComPtr<IAudioClient> InitClient(IMMDevice *device, SourceType type, DWORD process_id,
					       PFN_ActivateAudioInterfaceAsync activate_audio_interface_async,
					       speaker_layout &speakers, audio_format &format, uint32_t &sampleRate);
	static void InitFormat(const WAVEFORMATEX *wfex, enum speaker_layout &speakers, enum audio_format &format,
			       uint32_t &sampleRate);
	static ComPtr<IAudioCaptureClient> InitCapture(IAudioClient *client, HANDLE receiveSignal);
	void Initialize();

	ComPtr<IRtwqAsyncResult> CreateRtwqResult(void (WASAPISource::*handler)(), std::atomic<bool> *armed = nullptr);
	HRESULT ArmSampleReady();
	void RequestRestart();

	bool TryInitialize();

	struct UpdateParams {
		string device_id;
		bool useDeviceTiming;
		bool isDefaultDevice;
		window_priority priority;
		string window_class;
		string title;
		string executable;
	};

	UpdateParams BuildUpdateParams(obs_data_t *settings);
	void UpdateSettings(UpdateParams &&params);
	void LogSettings();

public:
	WASAPISource(obs_data_t *settings, obs_source_t *source_, SourceType type);
	~WASAPISource();

	void Update(obs_data_t *settings);
	void OnWindowChanged(obs_data_t *settings);

	void Activate();
	void Deactivate();

	void SetDefaultDevice(EDataFlow flow, ERole role, LPCWSTR id);

	void OnStartCapture();
	void OnSampleReady();

	bool GetHooked();
	HWND GetHwnd();

	void SetRerouteTarget(obs_source_t *target)
	{
		obs_weak_source_release(reroute_target);
		reroute_target = obs_source_get_weak_source(target);
	}
};

/* RTWorkQ keeps its own references to a source's callbacks and releases them after Invoke
 * returns, which can be after Stop() has returned and the source is freed. So the callbacks,
 * this link, and the event handle the sample-ready wait is registered on are refcounted apart
 * from the source, and reach it only while it is attached. */
class RtwqSourceLink {
	std::shared_mutex lock;
	WASAPISource *source;

public:
	const WinHandle sampleWait;

	/* Set while a sample-ready work item is armed. Its callback clears it on invoke, before the
	 * source is checked, so an item consumed after Detach() does not read as still armed. */
	std::atomic<bool> sampleReadyArmed = false;

	RtwqSourceLink(WASAPISource *source, HANDLE sampleWait) : source(source), sampleWait(sampleWait) {}

	void Run(void (WASAPISource::*handler)())
	{
		std::shared_lock<std::shared_mutex> guard(lock);
		if (source) {
			(source->*handler)();
		}
	}

	/* Returns once no handler is running, and none runs afterwards. */
	void Detach()
	{
		std::unique_lock<std::shared_mutex> guard(lock);
		source = nullptr;
	}
};

class RtwqSourceCallback
	: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
					      IRtwqAsyncCallback> {
	const std::shared_ptr<RtwqSourceLink> link;
	void (WASAPISource::*const handler)();
	std::atomic<bool> *const armed;
	const DWORD queueId;

public:
	RtwqSourceCallback(std::shared_ptr<RtwqSourceLink> link, void (WASAPISource::*handler)(),
			   std::atomic<bool> *armed, DWORD queueId)
		: link(std::move(link)),
		  handler(handler),
		  armed(armed),
		  queueId(queueId)
	{
	}

	STDMETHODIMP GetParameters(DWORD *flags, DWORD *queue) override
	{
		*flags = 0;
		*queue = queueId;
		return S_OK;
	}

	STDMETHODIMP Invoke(IRtwqAsyncResult *) override
	{
		if (armed) {
			*armed = false;
		}
		link->Run(handler);
		return S_OK;
	}
};

ComPtr<IRtwqAsyncResult> WASAPISource::CreateRtwqResult(void (WASAPISource::*handler)(), std::atomic<bool> *armed)
{
	const Microsoft::WRL::ComPtr<RtwqSourceCallback> callback =
		Microsoft::WRL::Make<RtwqSourceCallback>(rtwqLink, handler, armed, rtwqQueueId);
	if (!callback) {
		throw HRError("Could not create RTWQ callback", E_OUTOFMEMORY);
	}

	ComPtr<IRtwqAsyncResult> result;
	const HRESULT hr = rtwq_create_async_result(nullptr, callback.Get(), nullptr, result.Assign());
	if (FAILED(hr)) {
		throw HRError("Could not create RTWQ async result", hr);
	}
	return result;
}

WASAPISource::WASAPISource(obs_data_t *settings, obs_source_t *source_, SourceType type)
	: source(source_),
	  sourceType(type)
{
	mmdevapi_module = LoadLibrary(L"Mmdevapi");
	if (mmdevapi_module) {
		activate_audio_interface_async =
			(PFN_ActivateAudioInterfaceAsync)GetProcAddress(mmdevapi_module, "ActivateAudioInterfaceAsync");
	}

	UpdateSettings(BuildUpdateParams(settings));
	LogSettings();

	idleSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!idleSignal.Valid()) {
		throw "Could not create idle signal";
	}

	stopSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!stopSignal.Valid()) {
		throw "Could not create stop signal";
	}

	receiveSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!receiveSignal.Valid()) {
		throw "Could not create receive signal";
	}

	restartSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!restartSignal.Valid()) {
		throw "Could not create restart signal";
	}

	reconnectExitSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!reconnectExitSignal.Valid()) {
		throw "Could not create reconnect exit signal";
	}

	exitSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!exitSignal.Valid()) {
		throw "Could not create exit signal";
	}

	initSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!initSignal.Valid()) {
		throw "Could not create init signal";
	}

	reconnectSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!reconnectSignal.Valid()) {
		throw "Could not create reconnect signal";
	}

	HRESULT hr =
		CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.Assign()));
	if (FAILED(hr)) {
		throw HRError("Failed to create enumerator", hr);
	}

	/* OBS will already load DLL on startup if it exists */
	const HMODULE rtwq_module = GetModuleHandle(L"RTWorkQ.dll");

	// while RTWQ was introduced in Win 8.1, it silently fails
	// to capture Desktop Audio for some reason. Disable for now.
	struct win_version_info win1703 = {};
	win1703.major = 10;
	win1703.minor = 0;
	win1703.build = 15063;
	win1703.revis = 0;
	struct win_version_info ver;
	get_win_ver(&ver);
	if (win_version_compare(&ver, &win1703) >= 0) {
		rtwq_supported = rtwq_module != NULL;
	}

	if (rtwq_supported) {
		rtwq_unlock_work_queue = (PFN_RtwqUnlockWorkQueue)GetProcAddress(rtwq_module, "RtwqUnlockWorkQueue");
		rtwq_lock_shared_work_queue =
			(PFN_RtwqLockSharedWorkQueue)GetProcAddress(rtwq_module, "RtwqLockSharedWorkQueue");
		rtwq_create_async_result =
			(PFN_RtwqCreateAsyncResult)GetProcAddress(rtwq_module, "RtwqCreateAsyncResult");
		rtwq_put_work_item = (PFN_RtwqPutWorkItem)GetProcAddress(rtwq_module, "RtwqPutWorkItem");
		rtwq_put_waiting_work_item =
			(PFN_RtwqPutWaitingWorkItem)GetProcAddress(rtwq_module, "RtwqPutWaitingWorkItem");

		bool queueLocked = false;
		try {
			DWORD taskId = 0;
			hr = rtwq_lock_shared_work_queue(L"Capture", 0, &taskId, &rtwqQueueId);
			if (FAILED(hr)) {
				throw HRError("RtwqLockSharedWorkQueue failed", hr);
			}
			queueLocked = true;

			HANDLE sampleWait = NULL;
			if (!DuplicateInProcess(receiveSignal, &sampleWait)) {
				throw HRError("Could not duplicate receive signal", HRESULT_FROM_WIN32(GetLastError()));
			}
			rtwqLink = std::make_shared<RtwqSourceLink>(this, sampleWait);

			startCaptureAsyncResult = CreateRtwqResult(&WASAPISource::OnStartCapture);
			sampleReadyAsyncResult =
				CreateRtwqResult(&WASAPISource::OnSampleReady, &rtwqLink->sampleReadyArmed);
		} catch (HRError &err) {
			blog(LOG_ERROR, "RTWQ setup failed: %s (0x%08X)", err.str, err.hr);
			if (queueLocked) {
				rtwq_unlock_work_queue(rtwqQueueId);
			}
			rtwq_supported = false;
		}
	}

	startRaceProbe = StartRaceProbe::FromSettings(settings, rtwq_supported);

	if (sourceType == SourceType::DeviceOutput) {
		keepalive = std::make_unique<SilentRenderKeepalive>();
	}

	if (!rtwq_supported) {
		captureThread = CreateThread(nullptr, 0, WASAPISource::CaptureThread, this, 0, nullptr);
		if (!captureThread.Valid()) {
			throw "Failed to create capture thread";
		}
	}

	auto notify = GetNotify();
	if (notify) {
		notify->AddDefaultDeviceChangedCallback(this, std::bind(&WASAPISource::SetDefaultDevice, this,
									std::placeholders::_1, std::placeholders::_2,
									std::placeholders::_3));
	}

	Start();
}

void WASAPISource::Start()
{
	if (rtwq_supported) {
		rtwq_put_work_item(rtwqQueueId, 0, startCaptureAsyncResult);
	} else {
		SetEvent(initSignal);
	}
}

void WASAPISource::Stop()
{
	SetEvent(stopSignal);

	blog(LOG_INFO, "WASAPI: Device '%s' Terminated", device_name.c_str());

	if (rtwq_supported) {
		SetEvent(receiveSignal);
	}

	if (startRaceProbe) {
		startRaceProbe->AwaitIdle(idleSignal, receiveSignal);
	}

	if (reconnectThread.Valid()) {
		WaitForSingleObject(idleSignal, INFINITE);
	} else {
		const HANDLE sigs[] = {reconnectSignal, idleSignal};
		WaitForMultipleObjects(_countof(sigs), sigs, false, INFINITE);
	}

	SetEvent(exitSignal);

	if (reconnectThread.Valid()) {
		SetEvent(reconnectExitSignal);
		WaitForSingleObject(reconnectThread, INFINITE);
	}

	if (rtwq_supported) {
		/* idleSignal can be set while a start or sample handler is still running, and a start
		 * queued by the reconnect thread can still be pending. */
		rtwqLink->Detach();
		if (rtwqLink->sampleReadyArmed) {
			blog(LOG_ERROR, "WASAPI: Device '%s' stopped with its sample-ready work item still armed",
			     device_name.c_str());
		}
		rtwq_unlock_work_queue(rtwqQueueId);
	} else {
		WaitForSingleObject(captureThread, INFINITE);
	}

	/* Joined last, after the capture is quiesced: its thread touches none of the state above,
	 * and stopping it earlier would let the endpoint idle while a teardown still reads it. */
	if (keepalive) {
		keepalive->Stop();
	}

	obs_weak_source_release(reroute_target);
}

WASAPISource::~WASAPISource()
{
	auto notify = GetNotify();
	if (notify) {
		notify->RemoveDefaultDeviceChangedCallback(this);
	}
	// If the device is also used for monitoring, a cleanup is needed.
	if (sourceType == SourceType::DeviceOutput) {
		obs_source_audio_output_capture_device_changed(source, NULL);
	}

	Stop();
}

WASAPISource::UpdateParams WASAPISource::BuildUpdateParams(obs_data_t *settings)
{
	WASAPISource::UpdateParams params;
	params.device_id = obs_data_get_string(settings, OPT_DEVICE_ID);
	params.useDeviceTiming = obs_data_get_bool(settings, OPT_USE_DEVICE_TIMING);
	params.isDefaultDevice = _strcmpi(params.device_id.c_str(), "default") == 0;
	params.priority = (window_priority)obs_data_get_int(settings, "priority");
	params.window_class.clear();
	params.title.clear();
	params.executable.clear();
	if (sourceType != SourceType::Input) {
		const char *const window = obs_data_get_string(settings, OPT_WINDOW);
		char *window_class = nullptr;
		char *title = nullptr;
		char *executable = nullptr;
		ms_build_window_strings(window, &window_class, &title, &executable);
		if (window_class) {
			params.window_class = window_class;
			bfree(window_class);
		}
		if (title) {
			params.title = title;
			bfree(title);
		}
		if (executable) {
			params.executable = executable;
			bfree(executable);
		}
	}

	return params;
}

void WASAPISource::UpdateSettings(UpdateParams &&params)
{
	// Signal to deduplication logic in case the device is also used for monitoring.
	if (device_id != params.device_id && sourceType == SourceType::DeviceOutput) {
		obs_source_audio_output_capture_device_changed(source, params.device_id.c_str());
	}

	device_id = std::move(params.device_id);
	useDeviceTiming = params.useDeviceTiming;
	isDefaultDevice = params.isDefaultDevice;
	priority = params.priority;
	window_class = std::move(params.window_class);
	title = std::move(params.title);
	executable = std::move(params.executable);
}

void WASAPISource::LogSettings()
{
	if (sourceType == SourceType::ProcessOutput) {
		blog(LOG_INFO,
		     "[win-wasapi: '%s'] update settings:\n"
		     "\texecutable: %s\n"
		     "\ttitle: %s\n"
		     "\tclass: %s\n"
		     "\tpriority: %d",
		     obs_source_get_name(source), executable.c_str(), title.c_str(), window_class.c_str(),
		     (int)priority);
	} else {
		blog(LOG_INFO,
		     "[win-wasapi: '%s'] update settings:\n"
		     "\tdevice id: %s\n"
		     "\tuse device timing: %d",
		     obs_source_get_name(source), device_id.c_str(), (int)useDeviceTiming);
	}
}

void WASAPISource::Update(obs_data_t *settings)
{
	UpdateParams params = BuildUpdateParams(settings);

	const bool restart = (sourceType == SourceType::ProcessOutput)
				     ? ((priority != params.priority) || (window_class != params.window_class) ||
					(title != params.title) || (executable != params.executable))
				     : (device_id.compare(params.device_id) != 0);

	UpdateSettings(std::move(params));
	LogSettings();

	if (restart) {
		RequestRestart();
	}
}

void WASAPISource::OnWindowChanged(obs_data_t *settings)
{
	UpdateParams params = BuildUpdateParams(settings);

	const bool restart = (sourceType == SourceType::ProcessOutput)
				     ? ((priority != params.priority) || (window_class != params.window_class) ||
					(title != params.title) || (executable != params.executable))
				     : (device_id.compare(params.device_id) != 0);

	UpdateSettings(std::move(params));

	if (restart) {
		RequestRestart();
	}
}

void WASAPISource::RequestRestart()
{
	SetEvent(restartSignal);

	/* The sample handler owns the restart, and on a silent endpoint nothing else wakes it. */
	if (rtwq_supported) {
		SetEvent(receiveSignal);
	}
}

void WASAPISource::Activate()
{
	if (!reconnectThread.Valid()) {
		ResetEvent(reconnectExitSignal);
		reconnectThread = CreateThread(nullptr, 0, WASAPISource::ReconnectThread, this, 0, nullptr);
	}
}

void WASAPISource::Deactivate()
{
	if (reconnectThread.Valid()) {
		SetEvent(reconnectExitSignal);
		WaitForSingleObject(reconnectThread, INFINITE);
		reconnectThread = NULL;
	}
}

ComPtr<IMMDevice> WASAPISource::InitDevice(IMMDeviceEnumerator *enumerator, bool isDefaultDevice, SourceType type,
					   const string device_id)
{
	ComPtr<IMMDevice> device;

	if (isDefaultDevice) {
		const bool input = type == SourceType::Input;
		HRESULT res = enumerator->GetDefaultAudioEndpoint(input ? eCapture : eRender,
								  input ? eCommunications : eConsole, device.Assign());
		if (FAILED(res)) {
			throw HRError("Failed GetDefaultAudioEndpoint", res);
		}
	} else {
		wchar_t *w_id;
		os_utf8_to_wcs_ptr(device_id.c_str(), device_id.size(), &w_id);
		if (!w_id) {
			throw "Failed to widen device id string";
		}

		const HRESULT res = enumerator->GetDevice(w_id, device.Assign());

		bfree(w_id);

		if (FAILED(res)) {
			throw HRError("Failed to enumerate device", res);
		}
	}

	return device;
}

#define BUFFER_TIME_100NS (5 * 10000000)

static DWORD GetSpeakerChannelMask(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_STEREO:
		return KSAUDIO_SPEAKER_STEREO;
	case SPEAKERS_2POINT1:
		return KSAUDIO_SPEAKER_2POINT1;
	case SPEAKERS_4POINT0:
		return KSAUDIO_SPEAKER_SURROUND;
	case SPEAKERS_4POINT1:
		return OBS_KSAUDIO_SPEAKER_4POINT1;
	case SPEAKERS_5POINT1:
		return KSAUDIO_SPEAKER_5POINT1_SURROUND;
	case SPEAKERS_7POINT1:
		return KSAUDIO_SPEAKER_7POINT1_SURROUND;
	}

	return (DWORD)layout;
}

ComPtr<IAudioClient> WASAPISource::InitClient(IMMDevice *device, SourceType type, DWORD process_id,
					      PFN_ActivateAudioInterfaceAsync activate_audio_interface_async,
					      speaker_layout &speakers, audio_format &format, uint32_t &samples_per_sec)
{
	WAVEFORMATEXTENSIBLE wfextensible;
	CoTaskMemPtr<WAVEFORMATEX> wfex;
	const WAVEFORMATEX *pFormat;
	HRESULT res;
	ComPtr<IAudioClient> client;

	if (type == SourceType::ProcessOutput) {
		if (activate_audio_interface_async == NULL) {
			throw "ActivateAudioInterfaceAsync is not available";
		}

		struct obs_audio_info oai;
		obs_get_audio_info(&oai);

		const WORD nChannels = (WORD)get_audio_channels(oai.speakers);
		const DWORD nSamplesPerSec = oai.samples_per_sec;
		constexpr WORD wBitsPerSample = 32;
		const WORD nBlockAlign = nChannels * wBitsPerSample / 8;

		WAVEFORMATEX &wf = wfextensible.Format;
		wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wf.nChannels = nChannels;
		wf.nSamplesPerSec = nSamplesPerSec;
		wf.nAvgBytesPerSec = nSamplesPerSec * nBlockAlign;
		wf.nBlockAlign = nBlockAlign;
		wf.wBitsPerSample = wBitsPerSample;
		wf.cbSize = sizeof(wfextensible) - sizeof(wf);
		wfextensible.Samples.wValidBitsPerSample = wBitsPerSample;
		wfextensible.dwChannelMask = GetSpeakerChannelMask(oai.speakers);
		wfextensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

		AUDIOCLIENT_ACTIVATION_PARAMS audioclientActivationParams;
		audioclientActivationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		audioclientActivationParams.ProcessLoopbackParams.TargetProcessId = process_id;
		audioclientActivationParams.ProcessLoopbackParams.ProcessLoopbackMode =
			PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
		PROPVARIANT activateParams{};
		activateParams.vt = VT_BLOB;
		activateParams.blob.cbSize = sizeof(audioclientActivationParams);
		activateParams.blob.pBlobData = reinterpret_cast<BYTE *>(&audioclientActivationParams);

		{
			Microsoft::WRL::ComPtr<WASAPIActivateAudioInterfaceCompletionHandler> handler =
				Microsoft::WRL::Make<WASAPIActivateAudioInterfaceCompletionHandler>();
			ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
			res = activate_audio_interface_async(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
							     __uuidof(IAudioClient), &activateParams, handler.Get(),
							     &asyncOp);
			if (FAILED(res)) {
				throw HRError("Failed to get activate audio client", res);
			}

			res = handler->GetActivateResult(client.Assign());
			if (FAILED(res)) {
				throw HRError("Async activation failed", res);
			}
		}

		pFormat = &wf;
	} else {
		res = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.Assign());
		if (FAILED(res)) {
			throw HRError("Failed to activate client context", res);
		}

		res = client->GetMixFormat(&wfex);
		if (FAILED(res)) {
			throw HRError("Failed to get mix format", res);
		}

		pFormat = wfex.Get();
	}

	InitFormat(pFormat, speakers, format, samples_per_sec);

	DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	if (type != SourceType::Input) {
		flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
	}
	res = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_TIME_100NS, 0, pFormat, nullptr);
	if (FAILED(res)) {
		throw HRError("Failed to initialize audio client", res);
	}

	return client;
}

/* Its own audio session, because AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE is ignored for a stream that
 * joins a session the process already opened, and the app renders audio monitoring on the
 * process default one. Shared across sources: two captures of the same endpoint then share one
 * session rather than showing two. */
static const GUID kSilentRenderSession = {0x6b2f0f2a, 0x9c41, 0x4b7e, {0x8d, 0x2c, 0x5a, 0x17, 0xe3, 0x94, 0x6f, 0xd1}};

static wstring GetDeviceId(IMMDevice *device)
{
	CoTaskMemPtr<wchar_t> id;
	if (!device || FAILED(device->GetId(&id)) || !id.Get()) {
		return wstring();
	}
	return wstring(id.Get());
}

SilentRenderKeepalive::SilentRenderKeepalive()
{
	/* Failing to create either is not fatal: Start() then declines and says so once. */
	retargetSignal = CreateEvent(nullptr, false, false, nullptr);
	exitSignal = CreateEvent(nullptr, true, false, nullptr);
}

HRESULT SilentRenderKeepalive::OpenClient(IMMDeviceEnumerator *enumerator, const wstring &deviceId,
					  ComPtr<IAudioClient> &client, ComPtr<IAudioRenderClient> &render,
					  UINT32 &bufferFrames, UINT32 &blockAlign)
{
	ComPtr<IMMDevice> device;
	HRESULT res = enumerator->GetDevice(deviceId.c_str(), device.Assign());
	if (FAILED(res)) {
		return res;
	}

	res = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.Assign());
	if (FAILED(res)) {
		return res;
	}

	/* The engine mixes at the endpoint's own format, which the capture source's negotiated
	 * format need not match. */
	CoTaskMemPtr<WAVEFORMATEX> wfex;
	res = client->GetMixFormat(&wfex);
	if (FAILED(res)) {
		return res;
	}

	res = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE, BUFFER_TIME_100NS, 0,
				 wfex, &kSilentRenderSession);
	if (FAILED(res)) {
		return res;
	}

	res = client->GetBufferSize(&bufferFrames);
	if (FAILED(res)) {
		return res;
	}

	res = client->GetService(IID_PPV_ARGS(render.Assign()));
	if (FAILED(res)) {
		return res;
	}

	/* Names the mixer entry on any build that shows it in spite of the hide flag. */
	ComPtr<IAudioSessionControl> session;
	if (SUCCEEDED(client->GetService(IID_PPV_ARGS(session.Assign())))) {
		session->SetDisplayName(L"Desktop audio capture keepalive", nullptr);
	}

	blockAlign = wfex->nBlockAlign;

	/* Filled before it is started, so the first engine period after Start has data instead of
	 * an underrun glitch. What holds the endpoint's engine up is the started session rather
	 * than the data in it, so this is about this stream's own first period, nothing more. */
	bool wroteFrames = false;
	res = Refill(client, render, bufferFrames, blockAlign, wroteFrames);
	if (FAILED(res)) {
		return res;
	}

	return client->Start();
}

HRESULT SilentRenderKeepalive::Refill(IAudioClient *client, IAudioRenderClient *render, UINT32 bufferFrames,
				      UINT32 blockAlign, bool &wroteFrames)
{
	wroteFrames = false;

	UINT32 padding = 0;
	HRESULT res = client->GetCurrentPadding(&padding);
	if (FAILED(res)) {
		return res;
	}
	if (padding >= bufferFrames) {
		return S_OK;
	}
	wroteFrames = true;

	const UINT32 frames = bufferFrames - padding;
	BYTE *buffer = nullptr;
	res = render->GetBuffer(frames, &buffer);
	if (FAILED(res)) {
		return res;
	}

	/* Zeros rather than AUDCLNT_BUFFERFLAGS_SILENT, which is documented only as a shortcut for
	 * filling a buffer with silence: this leaves the stream indistinguishable from audio. */
	memset(buffer, 0, (size_t)frames * (size_t)blockAlign);
	return render->ReleaseBuffer(frames, 0);
}

DWORD WINAPI SilentRenderKeepalive::ThreadProc(LPVOID param)
{
	os_set_thread_name("win-wasapi: silent render keepalive");
	static_cast<SilentRenderKeepalive *>(param)->Run();
	return 0;
}

void SilentRenderKeepalive::Run()
{
	const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hrCom)) {
		blog(LOG_WARNING,
		     "WASAPI: silent render keepalive CoInitializeEx failed: 0x%08X;"
		     " the endpoint can stop delivering loopback while it is silent",
		     hrCom);
		return;
	}

	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioRenderClient> render;
	wstring open;
	UINT32 bufferFrames = 0;
	UINT32 blockAlign = 0;
	bool reportedFailure = false;
	int stalledPasses = 0;

	static_assert(
		kStalledPassLimit * kRefillMs == BUFFER_TIME_100NS / 10000,
		"the stalled-pass limit must span the whole client buffer, so no healthy refill cadence reaches it");

	HRESULT res =
		CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.Assign()));

	const HANDLE sigs[] = {exitSignal, retargetSignal};
	bool exit = false;
	while (!exit && enumerator) {
		wstring target;
		{
			std::lock_guard<std::mutex> guard(lock);
			target = wanted;
		}

		if (client && target != open) {
			client->Stop();
			render.Clear();
			client.Clear();
		}

		bool degraded = false;
		if (!client) {
			res = OpenClient(enumerator, target, client, render, bufferFrames, blockAlign);
			if (SUCCEEDED(res)) {
				open = target;
				/* The only trace this subsystem leaves when it works: it plays
				 * silence and asks not to be shown in the volume mixer. */
				blog(LOG_DEBUG, "WASAPI: silent render keepalive started (%u frame buffer)",
				     bufferFrames);
			} else {
				render.Clear();
				client.Clear();
				degraded = true;
			}
		}

		if (client) {
			bool wroteFrames = false;
			res = Refill(client, render, bufferFrames, blockAlign, wroteFrames);
			if (FAILED(res)) {
				client->Stop();
				render.Clear();
				client.Clear();
				degraded = true;
				stalledPasses = 0;
			} else if (wroteFrames) {
				stalledPasses = 0;
			} else if (++stalledPasses >= kStalledPassLimit) {
				/* Not degraded: there is no failing HRESULT to report, and a reopen is
				 * the whole remedy. Reachable at most once per buffer duration, so it
				 * cannot spin even if every reopen stalls again. */
				blog(LOG_WARNING,
				     "WASAPI: silent render keepalive consumed nothing for %ums; reopening the client",
				     kStalledPassLimit * kRefillMs);
				client->Stop();
				render.Clear();
				client.Clear();
				stalledPasses = 0;
			}
		}

		if (degraded != reportedFailure) {
			reportedFailure = degraded;
			if (degraded) {
				blog(LOG_WARNING,
				     "WASAPI: silent render keepalive is not running: 0x%08X;"
				     " the endpoint can stop delivering loopback while it is silent",
				     res);
			} else {
				blog(LOG_INFO, "WASAPI: silent render keepalive recovered");
			}
		}

		/* Only a retarget or the refill timeout continues; anything else, the stop signal
		 * included, ends the loop rather than risking a spin on a wait that cannot wait. */
		const DWORD ret = WaitForMultipleObjects(_countof(sigs), sigs, false, degraded ? kRetryMs : kRefillMs);
		exit = ret != (WAIT_OBJECT_0 + 1) && ret != WAIT_TIMEOUT;
	}

	if (!enumerator) {
		blog(LOG_WARNING,
		     "WASAPI: silent render keepalive could not create an enumerator: 0x%08X;"
		     " the endpoint can stop delivering loopback while it is silent",
		     res);
	}

	if (client) {
		client->Stop();
	}
	render.Clear();
	client.Clear();
	enumerator.Clear();

	CoUninitialize();
}

void SilentRenderKeepalive::Start(const wstring &deviceId)
{
	const char *refusal = nullptr;
	if (deviceId.empty()) {
		refusal = "the endpoint reported no id";
	} else if (!retargetSignal.Valid() || !exitSignal.Valid()) {
		refusal = "its signals could not be created";
	} else {
		{
			std::lock_guard<std::mutex> guard(lock);
			wanted = deviceId;
		}

		if (thread.Valid()) {
			SetEvent(retargetSignal);
			return;
		}

		/* Stop() leaves this manual-reset signal set, so a thread started after one would
		 * exit on its first wait and quietly leave the endpoint to idle. */
		ResetEvent(exitSignal);
		thread = CreateThread(nullptr, 0, SilentRenderKeepalive::ThreadProc, this, 0, nullptr);
		if (thread.Valid()) {
			return;
		}
		refusal = "its thread could not be created";
	}

	/* Reported once: a device that keeps failing reconnects every few seconds. */
	if (!reportedRefusal) {
		reportedRefusal = true;
		blog(LOG_WARNING,
		     "WASAPI: no silent render keepalive (%s);"
		     " the endpoint can stop delivering loopback while it is silent",
		     refusal);
	}
}

void SilentRenderKeepalive::Stop()
{
	if (!thread.Valid()) {
		return;
	}

	SetEvent(exitSignal);
	WaitForSingleObject(thread, INFINITE);
	thread = NULL;
}

static speaker_layout ConvertSpeakerLayout(DWORD layout, WORD channels)
{
	switch (layout) {
	case KSAUDIO_SPEAKER_2POINT1:
		return SPEAKERS_2POINT1;
	case KSAUDIO_SPEAKER_SURROUND:
		return SPEAKERS_4POINT0;
	case OBS_KSAUDIO_SPEAKER_4POINT1:
		return SPEAKERS_4POINT1;
	case KSAUDIO_SPEAKER_5POINT1_SURROUND:
		return SPEAKERS_5POINT1;
	case KSAUDIO_SPEAKER_7POINT1_SURROUND:
		return SPEAKERS_7POINT1;
	}

	return (speaker_layout)channels;
}

void WASAPISource::InitFormat(const WAVEFORMATEX *wfex, enum speaker_layout &speakers, enum audio_format &format,
			      uint32_t &sampleRate)
{
	DWORD layout = 0;

	if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfex;
		layout = ext->dwChannelMask;
	}

	/* WASAPI is always float */
	speakers = ConvertSpeakerLayout(layout, wfex->nChannels);
	format = AUDIO_FORMAT_FLOAT;
	sampleRate = wfex->nSamplesPerSec;
}

ComPtr<IAudioCaptureClient> WASAPISource::InitCapture(IAudioClient *client, HANDLE receiveSignal)
{
	ComPtr<IAudioCaptureClient> capture;
	HRESULT res = client->GetService(IID_PPV_ARGS(capture.Assign()));
	if (FAILED(res)) {
		throw HRError("Failed to create capture context", res);
	}

	res = client->SetEventHandle(receiveSignal);
	if (FAILED(res)) {
		throw HRError("Failed to set event handle", res);
	}

	res = client->Start();
	if (FAILED(res)) {
		throw HRError("Failed to start capture client", res);
	}

	return capture;
}

void WASAPISource::Initialize()
{
	ComPtr<IMMDevice> device;
	if (sourceType == SourceType::ProcessOutput) {
		device_name = "[VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK]";

		hwnd = ms_find_window(INCLUDE_MINIMIZED, priority, window_class.c_str(), title.c_str(),
				      executable.c_str());
		if (!hwnd) {
			throw "Failed to find window";
		}

		DWORD dwProcessId = 0;
		if (!GetWindowThreadProcessId(hwnd, &dwProcessId)) {
			hwnd = NULL;
			throw "Failed to get process id of window";
		}

		process_id = dwProcessId;
	} else {
		device = InitDevice(enumerator, isDefaultDevice, sourceType, device_id);

		device_name = GetDeviceName(device);
	}

	const bool heldForStop = startRaceProbe && startRaceProbe->HoldUntilStopWakes();

	ResetEvent(receiveSignal);

	ComPtr<IAudioClient> temp_client = InitClient(device, sourceType, process_id, activate_audio_interface_async,
						      speakers, format, sampleRate);
	if (keepalive) {
		keepalive->Start(GetDeviceId(device));
	}
	const HANDLE engineSignal = startRaceProbe ? HANDLE(startRaceProbe->silentEndpoint) : HANDLE(receiveSignal);
	ComPtr<IAudioCaptureClient> temp_capture = InitCapture(temp_client, engineSignal);

	client = std::move(temp_client);
	capture = std::move(temp_capture);

	/* The last step that can fail, so a throw never leaves a work item armed. */
	if (rtwq_supported) {
		const HRESULT hr = ArmSampleReady();
		if (FAILED(hr)) {
			capture.Clear();
			client.Clear();
			throw HRError("RtwqPutWaitingWorkItem failed", hr);
		}
	}

	if (heldForStop) {
		startRaceProbe->MarkInitDone();
	}

	/* Parsed by frontend/src/loopback_silence_selftest.cpp: its kInitializedMarker matches
	 * "] initialized (source:" and it reads the endpoint rate out of the bracket before that.
	 * Rewording this line makes that regression gate report SKIP on the very defect it exists to
	 * catch, so reword the producer and the consumer together. */
	blog(LOG_INFO, "WASAPI: Device '%s' [%" PRIu32 " Hz] initialized (source: %s)", device_name.c_str(), sampleRate,
	     obs_source_get_name(source));

	if (sourceType == SourceType::ProcessOutput && !hooked) {
		hooked = true;

		signal_handler_t *sh = obs_source_get_signal_handler(source);
		calldata_t data = {0};
		struct dstr title = {0};
		struct dstr window_class = {0};
		struct dstr executable = {0};

		ms_get_window_title(&title, hwnd);
		ms_get_window_class(&window_class, hwnd);
		ms_get_window_exe(&executable, hwnd);

		calldata_set_ptr(&data, "source", source);
		calldata_set_string(&data, "title", title.array);
		calldata_set_string(&data, "class", window_class.array);
		calldata_set_string(&data, "executable", executable.array);
		signal_handler_signal(sh, "hooked", &data);

		dstr_free(&title);
		dstr_free(&window_class);
		dstr_free(&executable);
		calldata_free(&data);
	}
}

bool WASAPISource::TryInitialize()
{
	bool success = false;
	try {
		Initialize();
		success = true;

	} catch (HRError &error) {
		if (!previouslyFailed) {
			blog(LOG_WARNING, "[WASAPISource::TryInitialize]:[%s] %s: %lX",
			     device_name.empty() ? device_id.c_str() : device_name.c_str(), error.str, error.hr);
		}
	} catch (const char *error) {
		if (!previouslyFailed) {
			blog(LOG_WARNING, "[WASAPISource::TryInitialize]:[%s] %s",
			     device_name.empty() ? device_id.c_str() : device_name.c_str(), error);
		}
	}

	previouslyFailed = !success;
	return success;
}

DWORD WINAPI WASAPISource::ReconnectThread(LPVOID param)
{
	os_set_thread_name("win-wasapi: reconnect thread");

	WASAPISource *source = (WASAPISource *)param;

	const HANDLE sigs[] = {
		source->reconnectExitSignal,
		source->reconnectSignal,
	};

	const HANDLE reconnect_sigs[] = {
		source->reconnectExitSignal,
		source->stopSignal,
	};

	bool exit = false;
	while (!exit) {
		const DWORD ret = WaitForMultipleObjects(_countof(sigs), sigs, false, INFINITE);
		switch (ret) {
		case WAIT_OBJECT_0:
			exit = true;
			break;
		default:
			assert(ret == (WAIT_OBJECT_0 + 1));
			if (source->reconnectDuration > 0) {
				WaitForMultipleObjects(_countof(reconnect_sigs), reconnect_sigs, false,
						       source->reconnectDuration);
			}
			source->Start();
		}
	}

	return 0;
}

bool WASAPISource::ProcessCaptureData()
{
	HRESULT res;
	LPBYTE buffer;
	UINT32 frames;
	DWORD flags;
	UINT64 pos, ts;
	UINT captureSize = 0;

	while (true) {
		if ((sourceType == SourceType::ProcessOutput) && !IsWindow(hwnd)) {
			blog(LOG_WARNING, "[WASAPISource::ProcessCaptureData] window disappeared");
			return false;
		}

		res = capture->GetNextPacketSize(&captureSize);
		if (FAILED(res)) {
			if (res != AUDCLNT_E_DEVICE_INVALIDATED) {
				blog(LOG_WARNING,
				     "[WASAPISource::ProcessCaptureData]"
				     " capture->GetNextPacketSize"
				     " failed: %lX",
				     res);
			}
			return false;
		}

		if (!captureSize) {
			break;
		}

		res = capture->GetBuffer(&buffer, &frames, &flags, &pos, &ts);
		if (FAILED(res)) {
			if (res != AUDCLNT_E_DEVICE_INVALIDATED) {
				blog(LOG_WARNING,
				     "[WASAPISource::ProcessCaptureData]"
				     " capture->GetBuffer"
				     " failed: %lX",
				     res);
			}
			return false;
		}

		if (!sawBadTimestamp && flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
			blog(LOG_WARNING, "[WASAPISource::ProcessCaptureData]"
					  " Timestamp error!");
			sawBadTimestamp = true;
		}

		if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
			/* buffer size = frame size * number of frames
			 * frame size = channels * sample size
			 * sample size = 4 bytes (always float per InitFormat) */
			uint32_t requiredBufSize = get_audio_channels(speakers) * frames * 4;
			if (silence.size() < requiredBufSize) {
				silence.resize(requiredBufSize);
			}

			buffer = silence.data();
		}

		obs_source_audio data = {};
		data.data[0] = buffer;
		data.frames = frames;
		data.speakers = speakers;
		data.samples_per_sec = sampleRate;
		data.format = format;
		if (sourceType == SourceType::ProcessOutput) {
			data.timestamp = ts * 100;
		} else {
			data.timestamp = useDeviceTiming ? ts * 100 : os_gettime_ns();

			if (!useDeviceTiming) {
				data.timestamp -= util_mul_div64(frames, UINT64_C(1000000000), sampleRate);
			}
		}

		if (reroute_target) {
			obs_source_t *target = obs_weak_source_get_source(reroute_target);

			if (target) {
				obs_source_output_audio(target, &data);
				obs_source_release(target);
			}
		} else {
			obs_source_output_audio(source, &data);
		}

		capture->ReleaseBuffer(frames);
	}

	return true;
}

#define RECONNECT_INTERVAL 3000

DWORD WINAPI WASAPISource::CaptureThread(LPVOID param)
{
	os_set_thread_name("win-wasapi: capture thread");

	const HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	const bool com_initialized = SUCCEEDED(hr);
	if (!com_initialized) {
		blog(LOG_ERROR,
		     "[WASAPISource::CaptureThread]"
		     " CoInitializeEx failed: 0x%08X",
		     hr);
	}

	DWORD unused = 0;
	const HANDLE handle = AvSetMmThreadCharacteristics(L"Audio", &unused);

	WASAPISource *source = (WASAPISource *)param;

	const HANDLE inactive_sigs[] = {
		source->exitSignal,
		source->stopSignal,
		source->initSignal,
	};

	const HANDLE active_sigs[] = {
		source->exitSignal,
		source->stopSignal,
		source->receiveSignal,
		source->restartSignal,
	};

	DWORD sig_count = _countof(inactive_sigs);
	const HANDLE *sigs = inactive_sigs;

	bool exit = false;
	while (!exit) {
		bool idle = false;
		bool stop = false;
		bool reconnect = false;
		do {
			/* Windows 7 does not seem to wake up for LOOPBACK */
			const DWORD dwMilliseconds =
				((sigs == active_sigs) && (source->sourceType != SourceType::Input)) ? 10 : INFINITE;

			const DWORD ret = WaitForMultipleObjects(sig_count, sigs, false, dwMilliseconds);
			switch (ret) {
			case WAIT_OBJECT_0: {
				exit = true;
				stop = true;
				idle = true;
				break;
			}

			case WAIT_OBJECT_0 + 1:
				stop = true;
				idle = true;
				break;

			case WAIT_OBJECT_0 + 2:
			case WAIT_TIMEOUT:
				if (sigs == inactive_sigs) {
					assert(ret != WAIT_TIMEOUT);

					if (source->TryInitialize()) {
						sig_count = _countof(active_sigs);
						sigs = active_sigs;
					} else {
						if (source->reconnectDuration == 0) {
							blog(LOG_INFO,
							     "WASAPI: Device '%s' failed to start (source: %s)",
							     source->device_id.c_str(),
							     obs_source_get_name(source->source));
						}
						stop = true;
						reconnect = true;
						source->reconnectDuration = RECONNECT_INTERVAL;
					}
				} else {
					stop = !source->ProcessCaptureData();
					if (stop) {
						blog(LOG_INFO, "Device '%s' invalidated.  Retrying (source: %s)",
						     source->device_name.c_str(), obs_source_get_name(source->source));
						if (source->sourceType == SourceType::ProcessOutput && source->hooked) {
							source->hooked = false;
							signal_handler_t *sh =
								obs_source_get_signal_handler(source->source);
							calldata_t data = {0};
							calldata_set_ptr(&data, "source", source->source);
							signal_handler_signal(sh, "unhooked", &data);
							calldata_free(&data);
						}
						stop = true;
						reconnect = true;
						source->reconnectDuration = RECONNECT_INTERVAL;
					}
				}
				break;

			default:
				assert(sigs == active_sigs);
				assert(ret == WAIT_OBJECT_0 + 3);
				stop = true;
				reconnect = true;
				source->reconnectDuration = 0;
				ResetEvent(source->restartSignal);
			}
		} while (!stop);

		sig_count = _countof(inactive_sigs);
		sigs = inactive_sigs;

		if (source->client) {
			source->client->Stop();

			source->capture.Clear();
			source->client.Clear();
		}

		if (idle) {
			SetEvent(source->idleSignal);
		} else if (reconnect) {
			blog(LOG_INFO, "Device '%s' invalidated.  Retrying (source: %s)", source->device_name.c_str(),
			     obs_source_get_name(source->source));
			if (source->sourceType == SourceType::ProcessOutput && source->hooked) {
				source->hooked = false;
				signal_handler_t *sh = obs_source_get_signal_handler(source->source);
				calldata_t data = {0};
				calldata_set_ptr(&data, "source", source->source);
				signal_handler_signal(sh, "unhooked", &data);
				calldata_free(&data);
			}
			SetEvent(source->reconnectSignal);
		}
	}

	if (handle) {
		AvRevertMmThreadCharacteristics(handle);
	}

	if (com_initialized) {
		CoUninitialize();
	}

	return 0;
}

void WASAPISource::SetDefaultDevice(EDataFlow flow, ERole role, LPCWSTR id)
{
	if (!isDefaultDevice) {
		return;
	}

	const bool input = sourceType == SourceType::Input;
	const EDataFlow expectedFlow = input ? eCapture : eRender;
	const ERole expectedRole = input ? eCommunications : eConsole;
	if (flow != expectedFlow || role != expectedRole) {
		return;
	}

	if (id) {
		if (default_id.compare(id) == 0) {
			return;
		}
		default_id = id;
	} else {
		if (default_id.empty()) {
			return;
		}
		default_id.clear();
	}

	blog(LOG_INFO, "WASAPI: Default %s device changed", input ? "input" : "output");

	RequestRestart();
}

void WASAPISource::OnStartCapture()
{
	struct StartEnded {
		HANDLE event;
		~StartEnded()
		{
			if (event) {
				SetEvent(event);
			}
		}
	} startEnded{startRaceProbe ? startRaceProbe->StartEndedEvent() : NULL};

	const DWORD ret = WaitForSingleObject(stopSignal, 0);
	switch (ret) {
	case WAIT_OBJECT_0:
		SetEvent(idleSignal);
		break;

	default:
		assert(ret == WAIT_TIMEOUT);

		if (!TryInitialize()) {
			if (reconnectDuration == 0) {
				blog(LOG_INFO, "WASAPI: Device '%s' failed to start (source: %s)", device_id.c_str(),
				     obs_source_get_name(source));
			}
			reconnectDuration = RECONNECT_INTERVAL;
			SetEvent(reconnectSignal);
		} else if (WaitForSingleObject(stopSignal, 0) == WAIT_OBJECT_0 ||
			   WaitForSingleObject(restartSignal, 0) == WAIT_OBJECT_0) {
			/* Stop() or RequestRestart() may have woken the sample handler before Initialize
			 * reset receiveSignal. Wake it again: it owns both, and on a stop it sets idleSignal
			 * once the client is stopped and the work item armed by Initialize is consumed. */
			SetEvent(receiveSignal);
		}
	}
}

HRESULT WASAPISource::ArmSampleReady()
{
	/* Set before the put: the item can fire, and clear it, before the put returns. */
	rtwqLink->sampleReadyArmed = true;
	const HRESULT hr = rtwq_put_waiting_work_item(rtwqLink->sampleWait, 0, sampleReadyAsyncResult, nullptr);
	if (FAILED(hr)) {
		rtwqLink->sampleReadyArmed = false;
	}
	return hr;
}

void WASAPISource::OnSampleReady()
{
	bool stop = false;
	bool reconnect = false;

	if (!ProcessCaptureData()) {
		stop = true;
		reconnect = true;
		reconnectDuration = RECONNECT_INTERVAL;
	}

	if (WaitForSingleObject(restartSignal, 0) == WAIT_OBJECT_0) {
		stop = true;
		reconnect = true;
		reconnectDuration = 0;

		ResetEvent(restartSignal);
	}

	if (WaitForSingleObject(stopSignal, 0) == WAIT_OBJECT_0) {
		stop = true;
		reconnect = false;
	}

	if (!stop) {
		if (FAILED(ArmSampleReady())) {
			blog(LOG_ERROR, "Could not requeue sample receive work");
			stop = true;
			reconnect = true;
			reconnectDuration = RECONNECT_INTERVAL;
		}
	}

	if (stop) {
		client->Stop();

		capture.Clear();
		client.Clear();

		if (reconnect) {
			blog(LOG_INFO, "Device '%s' invalidated.  Retrying (source: %s)", device_name.c_str(),
			     obs_source_get_name(source));
			SetEvent(reconnectSignal);
			if (sourceType == SourceType::ProcessOutput && hooked) {
				hooked = false;
				signal_handler_t *sh = obs_source_get_signal_handler(source);
				calldata_t data = {0};
				calldata_set_ptr(&data, "source", source);
				signal_handler_signal(sh, "unhooked", &data);
				calldata_free(&data);
			}
		} else {
			SetEvent(idleSignal);
		}
	}
}

bool WASAPISource::GetHooked()
{
	return hooked;
}

HWND WASAPISource::GetHwnd()
{
	return hwnd;
}

/* ------------------------------------------------------------------------- */

static const char *GetWASAPIInputName(void *)
{
	return obs_module_text("AudioInput");
}

static const char *GetWASAPIDeviceOutputName(void *)
{
	return obs_module_text("AudioOutput");
}

static const char *GetWASAPIProcessOutputName(void *)
{
	return obs_module_text("ApplicationAudioCapture");
}

static void GetWASAPIDefaultsInput(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_DEVICE_ID, "default");
	obs_data_set_default_bool(settings, OPT_USE_DEVICE_TIMING, false);
}

static void GetWASAPIDefaultsDeviceOutput(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_DEVICE_ID, "default");
	obs_data_set_default_bool(settings, OPT_USE_DEVICE_TIMING, true);
}

static void GetWASAPIDefaultsProcessOutput(obs_data_t *) {}

static void wasapi_get_hooked(void *data, calldata_t *cd)
{
	WASAPISource *wasapi_source = reinterpret_cast<WASAPISource *>(data);

	if (!wasapi_source) {
		return;
	}

	bool hooked = wasapi_source->GetHooked();
	HWND hwnd = wasapi_source->GetHwnd();

	if (hooked && hwnd) {
		calldata_set_bool(cd, "hooked", true);
		struct dstr title = {0};
		struct dstr window_class = {0};
		struct dstr executable = {0};
		ms_get_window_title(&title, hwnd);
		ms_get_window_class(&window_class, hwnd);
		ms_get_window_exe(&executable, hwnd);
		calldata_set_string(cd, "title", title.array);
		calldata_set_string(cd, "class", window_class.array);
		calldata_set_string(cd, "executable", executable.array);
		dstr_free(&title);
		dstr_free(&window_class);
		dstr_free(&executable);
	} else {
		calldata_set_bool(cd, "hooked", false);
		calldata_set_string(cd, "title", "");
		calldata_set_string(cd, "class", "");
		calldata_set_string(cd, "executable", "");
	}
}

static void wasapi_reroute_audio(void *data, calldata_t *cd)
{
	auto wasapi_source = static_cast<WASAPISource *>(data);
	if (!wasapi_source) {
		return;
	}

	obs_source_t *target = nullptr;
	calldata_get_ptr(cd, "target", &target);
	wasapi_source->SetRerouteTarget(target);
}

static void *CreateWASAPISource(obs_data_t *settings, obs_source_t *source, SourceType type)
{
	try {
		if (type != SourceType::ProcessOutput) {
			return new WASAPISource(settings, source, type);
		} else {
			WASAPISource *wasapi_source = new WASAPISource(settings, source, type);

			if (wasapi_source) {
				signal_handler_t *sh = obs_source_get_signal_handler(source);
				signal_handler_add(sh, "void unhooked(ptr source)");
				signal_handler_add(
					sh, "void hooked(ptr source, string title, string class, string executable)");

				proc_handler_t *ph = obs_source_get_proc_handler(source);
				proc_handler_add(
					ph,
					"void get_hooked(out bool hooked, out string title, out string class, out string executable)",
					wasapi_get_hooked, wasapi_source);
				proc_handler_add(ph, "void reroute_audio(in ptr target)", wasapi_reroute_audio,
						 wasapi_source);
			}
			return wasapi_source;
		}
	} catch (const char *error) {
		blog(LOG_ERROR, "[CreateWASAPISource] %s", error);
	}

	return nullptr;
}

static void *CreateWASAPIInput(obs_data_t *settings, obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::Input);
}

static void *CreateWASAPIDeviceOutput(obs_data_t *settings, obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::DeviceOutput);
}

static void *CreateWASAPIProcessOutput(obs_data_t *settings, obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::ProcessOutput);
}

static void DestroyWASAPISource(void *obj)
{
	delete static_cast<WASAPISource *>(obj);
}

static void UpdateWASAPISource(void *obj, obs_data_t *settings)
{
	static_cast<WASAPISource *>(obj)->Update(settings);
}

static void ActivateWASAPISource(void *obj)
{
	static_cast<WASAPISource *>(obj)->Activate();
}

static void DeactivateWASAPISource(void *obj)
{
	static_cast<WASAPISource *>(obj)->Deactivate();
}

static bool UpdateWASAPIMethod(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	WASAPISource *source = (WASAPISource *)obs_properties_get_param(props);
	if (!source) {
		return false;
	}

	source->Update(settings);

	return true;
}

static obs_properties_t *GetWASAPIPropertiesInput(void *)
{
	obs_properties_t *props = obs_properties_create();
	vector<AudioDeviceInfo> devices;

	obs_property_t *device_prop = obs_properties_add_list(props, OPT_DEVICE_ID, obs_module_text("Device"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	GetWASAPIAudioDevices(devices, true);

	if (devices.size()) {
		obs_property_list_add_string(device_prop, obs_module_text("Default"), "default");
	}

	for (size_t i = 0; i < devices.size(); i++) {
		AudioDeviceInfo &device = devices[i];
		obs_property_list_add_string(device_prop, device.name.c_str(), device.id.c_str());
	}

	obs_properties_add_bool(props, OPT_USE_DEVICE_TIMING, obs_module_text("UseDeviceTiming"));

	return props;
}

static obs_properties_t *GetWASAPIPropertiesDeviceOutput(void *)
{
	obs_properties_t *props = obs_properties_create();
	vector<AudioDeviceInfo> devices;

	obs_property_t *device_prop = obs_properties_add_list(props, OPT_DEVICE_ID, obs_module_text("Device"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	GetWASAPIAudioDevices(devices, false);

	if (devices.size()) {
		obs_property_list_add_string(device_prop, obs_module_text("Default"), "default");
	}

	for (size_t i = 0; i < devices.size(); i++) {
		AudioDeviceInfo &device = devices[i];
		obs_property_list_add_string(device_prop, device.name.c_str(), device.id.c_str());
	}

	obs_properties_add_bool(props, OPT_USE_DEVICE_TIMING, obs_module_text("UseDeviceTiming"));

	return props;
}

static bool wasapi_window_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	WASAPISource *source = (WASAPISource *)obs_properties_get_param(props);
	if (!source) {
		return false;
	}

	source->OnWindowChanged(settings);

	ms_check_window_property_setting(props, p, settings, "window", 0);
	return true;
}

static obs_properties_t *GetWASAPIPropertiesProcessOutput(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, data, NULL);

	obs_property_t *const window_prop = obs_properties_add_list(props, OPT_WINDOW, obs_module_text("Window"),
								    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	ms_fill_window_list(window_prop, INCLUDE_MINIMIZED, nullptr);
	obs_property_set_modified_callback(window_prop, wasapi_window_changed);

	obs_property_t *const priority_prop = obs_properties_add_list(props, OPT_PRIORITY, obs_module_text("Priority"),
								      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(priority_prop, obs_module_text("Priority.Title"), WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(priority_prop, obs_module_text("Priority.Class"), WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(priority_prop, obs_module_text("Priority.Exe"), WINDOW_PRIORITY_EXE);

	return props;
}

void RegisterWASAPIInput()
{
	obs_source_info info = {};
	info.id = "wasapi_input_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = GetWASAPIInputName;
	info.create = CreateWASAPIInput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.activate = ActivateWASAPISource;
	info.deactivate = DeactivateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsInput;
	info.get_properties = GetWASAPIPropertiesInput;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	obs_register_source(&info);
}

void RegisterWASAPIDeviceOutput()
{
	obs_source_info info = {};
	info.id = "wasapi_output_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = GetWASAPIDeviceOutputName;
	info.create = CreateWASAPIDeviceOutput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.activate = ActivateWASAPISource;
	info.deactivate = DeactivateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsDeviceOutput;
	info.get_properties = GetWASAPIPropertiesDeviceOutput;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	obs_register_source(&info);
}

void RegisterWASAPIProcessOutput()
{
	obs_source_info info = {};
	info.id = "wasapi_process_output_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = GetWASAPIProcessOutputName;
	info.create = CreateWASAPIProcessOutput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.activate = ActivateWASAPISource;
	info.deactivate = DeactivateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsProcessOutput;
	info.get_properties = GetWASAPIPropertiesProcessOutput;
	info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;
	obs_register_source(&info);
}
