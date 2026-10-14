# Voice control: three concurrency designs

Three pieces of voice control's threading landed without a design review (review
2026-10-09, S-I6). This note gives each one's invariant, the threads involved, the
lock and atomic protocol, the failure it prevents, and how it is tested, so it can
be reviewed against the code. Names are symbols, not line numbers. The tests named
here are the in-app self-test (`VoiceSelfTest.cpp`, run in the Windows smoke) and
the host harnesses (Linux, with fakes for libobs and whisper where noted). Nothing
here has been exercised under load on Windows.

Threads, as the engine names them (`VoiceEngine.hpp`):

| Thread | Runs |
| --- | --- |
| UI (CEF `TID_UI`) | the listener, every effect and event, `Start`/`Stop`, `ApplySettings`, `Persist`; loading the speech engine's DLLs the first time voice is on (`LoadEngine`; `EnsureWhisperLoaded` takes its own mutex, so the recognizer worker's check there only waits for it) |
| libobs hotkey thread | `OnPtt`, `OnCancelKey`; a key-up can also run on the UI thread when a held key is rebound or unregistered there (libobs runs every hotkey callback under its hotkeys mutex) |
| libobs audio thread | `VoiceCapture::OnAudio`, the ring's producer |
| recognizer worker | model load, the detector, whisper; the ring's consumer |
| async workers (`AsyncTask::RunAsync`) | model verification, retiring a runtime, speech synthesis, downloads |

## 1. Retiring a runtime: the generation

`VoiceEngine::Runtime` holds the ring, the capture and the recognizer while voice is
enabled. Turning voice off, changing the model, language, trigger mode or wake phrase,
or a model arriving after a failure, replaces it (`StopRuntime`, then `StartRuntime`).

**Invariant.** Nothing a retired runtime does reaches the listener or the UI, the UI
thread never waits for a recognizer to stop (except at app shutdown), and every
recognizer has stopped before `VoiceEngine::Stop` returns.

**Protocol.**

- `generation_` (atomic) is bumped by `StartRuntime` and by `StopRuntime`, first
  thing, on the UI thread.
- Everything that reaches the UI from another thread carries the generation it was
  created under: `PostEvent(event, generation)` from the hotkey thread, the
  recognizer's callbacks (each captures the generation `LoadModel` ran under), the
  verification worker's `LoadModel` post, the countdown's `Tick`, `SpeakBack`'s
  playback. `HandleOnUi` and `LoadModel` drop anything whose generation is not the
  current one, on the UI thread, where `generation_` is only ever written.
- `runtime_` is a `shared_ptr`, read and swapped under `runtimeMutex_`. The hotkey
  thread copies it (`CurrentRuntime`) and works on its copy, so the runtime cannot be
  freed under a key press; it never waits on the UI.
- `StopRuntime` moves `runtime_` out under the mutex, then on the UI thread: sets the
  runtime's `verifyCancel` (a verification worker stops hashing), stops watching
  channel changes and unbinds the capture (`obs_source_remove_audio_capture_callback`
  takes the mutex libobs holds while dispatching, so no `OnAudio` is in flight
  afterwards: the producer is gone before the consumer stops).
- `Retire` hands the `shared_ptr` to an async worker that calls
  `Recognizer::Stop` (which can wait out one encoder pass) and drops it. `retiring_`
  counts these under `retireMutex_`; `WaitForRetired` (in `Stop` and the destructor)
  waits on `retireCv_` for zero. If no worker can be started the stop runs inline.
- The recognizer's callbacks capture a raw `Runtime *`: they live inside the
  recognizer, which lives inside the runtime, and `Recognizer::Stop` joins the worker
  before the runtime can be destroyed, so no callback outlives it.
- `Stop` (shutdown) stops its own runtime inline (`StopRuntime(true)`) and waits for
  every retired one: whisper's threads are gone before `obs_shutdown`.

**Failures prevented.** A disable freezing the Settings tab for an encoder pass
(seconds on a slow CPU); a transcript or a model-ready from the old model acting on
the listener after voice was turned off, or confirming a command meant for the old
runtime; a key press using a freed ring or recognizer; whisper threads alive at
process exit.

**Residual.** The hotkey thread may hold the last reference briefly, so the runtime
can be destroyed there. That is safe because the UI thread has already unbound and
stopped watching (the capture's destructor then has nothing to do;
`VoiceCapture.hpp`), and the recognizer was stopped by the retire worker before it
dropped its reference.

**Tests.** Engine harness (real `VoiceEngine`, `VoiceListener`, `VoiceCapture`,
`MicMuteGuard`; a fake recognizer whose `Stop` can be made slow): disabling returns
without waiting for the encoder pass; the mic is unbound at once on the UI thread;
re-enabling mid-stop builds a new runtime beside the old one; the retired
recognizer's late reports are dropped; shutdown waits for every recognizer still
stopping; both retired runtimes are destroyed; a slow verification superseded by
another change is cancelled and its outcome dropped. Run under ASan+UBSan and
ThreadSanitizer. In-app: `TestVoiceEngine` (enable, disable, model failure).

## 2. Persisting a mic held by push-to-talk: the epoch

While the push-to-talk key is held, `MicMuteGuard` mutes the mic so viewers do not
hear the command. `GlobalAudioChannels::Persist` saves each global channel's full
source blob, `muted` included, to `audio_devices.json`, at any time on the UI thread.

**Invariant.** The `muted` that `Persist` stores is never the guard's own mute: it is
the state the user wants (their mute or unmute made during the hold included).

**Threads.** UI (`Persist`; the bridge seam `Bridge::SetSourceMuted` behind the mixer,
`audio.setMuted`, MCP and voice's "mute mic"), the hotkey thread (`Engage`,
`Release`), and any thread that mutes the mic (the guard's `"mute"` signal handler
runs synchronously on it).

**Protocol** (`GlobalAudioChannels.cpp`, after group A's N-I1 changes):

- One override, guarded by `g_muteOverrideMutex`: the source (a weak reference,
  resolved and compared, so a new source at a dead one's address does not inherit it)
  and its value, the state the hold returns to.
- `g_muteOverrideEpoch` (atomic) is bumped by every register, clear and change of the
  user's wish.
- The guard registers the override (`SetPersistedMuteOverride`) before it mutes, and
  `EndTemporaryMute` unmutes and then clears it, both under the lock. So a read of the
  mic's state with no epoch change around it is either covered by the override or not
  the guard's mute.
- `Persist` reads the epoch (`MuteOverrideEpoch`) before `obs_save_source` and passes
  the snapshot's `muted` to `PersistedMuteForSnapshot`, which stores the override's
  value when one is active and the epoch did not move, the snapshot when none is, and
  otherwise reads the live state again and rechecks until no transition overlaps the
  read.
- The user's wish enters in two ways. Through the bridge seam, `SetUserMuted` notes it
  and mutes under the lock, so a hold beginning or ending is wholly before or after it.
  By any other path (libobs's per-source Mute hotkey, a script), the guard's `"mute"`
  handler records it lock-free (`NoteHeldUserMute`, plus the guard's own `userWish_`),
  telling its own mute apart by a thread-local flag; `Release` records the wish into
  the override before `EndTemporaryMute` decides.
- Lock order is one way: the override lock may be held while libobs emits `"mute"`,
  and the handler takes no lock. Taking it there ordered the two both ways
  (ThreadSanitizer found it).

**Failure prevented.** A save landing during a hold (a settings change, a mixer move)
persisting the mic as muted, so the next launch starts with the mic muted and nobody
knows why; a save straddling the release storing the guard's mute; a user's mute made
during a hold undone on release, or not saved.

**Residual** (documented in group A's commit aa37493): a mute from a thread other than
the hotkey thread, in the microseconds between the guard disconnecting its handler
and ending the hold, is not seen. libobs's own mute hotkey runs on the hotkey thread,
as push-to-talk does, so it cannot land there.

**Tests.** Mic harness (real `GlobalAudioChannels` and `MicMuteGuard`, fake sources
that emit `"mute"`): Persist during the hold writes unmuted for the mic and leaves
another channel alone; address reuse gets no override; the plan's single check after
release would store the guard's mute, while the epoch resolves a snapshot taken
during the hold to unmuted; the mute-intent cases (through the seam, by the signal
alone, last choice wins both ways, a hold left alone gives the mic back); a user mute
racing back-to-back holds on another thread; hotkey-thread holds against UI-thread
persists. 39 checks, run under ThreadSanitizer (0 warnings). In-app:
`TestMicMuteGuard` (including "a save straddling the release stores unmuted" and the
mute-intent cases).

## 3. Who owns the audio when a key and a wake collide

In always-listen the recognizer worker runs the ring past the voice activity
detector, checks each utterance's opening for the wake phrase, and transcribes a woken
utterance. The push-to-talk key can open a segment at any moment from the hotkey
thread.

**Invariant.** At any time the audio belongs to exactly one of: the idle drain (the
pre-roll), an always-listen utterance, or the key's segment. A key segment takes over
an utterance that has not been claimed by the wake phrase; once one has been claimed,
the key is refused until its result is out. Every wake callback is followed by exactly
one result, unless `Stop` comes first. The ring has one producer (the audio thread)
and one consumer (the worker), always.

**Protocol** (`Recognizer.cpp`):

- `SpscRing`: the audio thread writes, the worker reads, nobody else touches the
  indices. The hotkey thread never reads audio; it only changes `segment_`.
- `segment_` (`Idle`, `Open`, `Closing`, `Busy`) and `wakeBusy_` are under `mutex_`.
  `BeginSegment` opens only from `Idle` with `wakeBusy_` false; `EndSegment` moves
  `Open` to `Closing`.
- `ContinuousStep` checks `segment_` under the lock before each block it reads and
  returns when a key opened one, so the detector never consumes the key's audio.
- The wake check (`WakeCheck`) runs without the lock. Its verdict takes effect only
  through `ClaimWake`, which sets `wakeBusy_` under the lock and only while `segment_`
  is `Idle`: a key pressed during the wake check wins, and the utterance is dismissed
  without a wake callback.
- On the first worker pass that sees a key's segment, `AbandonUtterance` drops what
  always-listen was collecting and resets the detector (its stream now has a gap),
  before `StartCollecting` takes the pre-roll and `audio_` over.
- `FinishWoken` clears `wakeBusy_` under the lock only after the result is built, and
  delivers it unless `stopping_` is set. It first waits, at most
  `kWakePromptWaitMs`, for the prompt refresh its wake asked for (`promptSeq_`, under
  `mutex_`; `Stop` ends the wait through `quit_`).
- The muted-mic flag belongs to the segment or utterance it was seen in:
  `BeginUtterance` resets it under the lock only while no segment is open; once a key
  opened one, the flag is that segment's.

**Failure prevented.** Audio read by two consumers (a corrupted ring); a command spoken
with the key down being half-consumed by the detector, or transcribed twice; a wake
indicator with no result, leaving the listener in Listening; a key segment opened
while a woken utterance is being transcribed, mixing the two; a stale mute flag hiding
or inventing a muted-mic refusal.

**Tests.** Continuous harness (real `Recognizer`, `VadEndpointer`, `WakeGate` over a
scripted whisper, exact audio and timing): a key pressed while idle opens a segment
with the pre-roll; a key pressed mid-utterance drops it for the key's segment and the
detector starts over; a key pressed while a woken utterance is transcribed is refused
and accepted again after its result; a key pressed during the wake check wins with no
wake; Stop during a woken transcription or a wake check returns promptly and delivers
nothing; the mute flag cases; a fast producer and racing key presses (every wake has
its result); the wake-prompt wait. Run under ASan+UBSan and ThreadSanitizer
(0 warnings). The real-whisper continuous harness runs the repo's
`TestContinuousRecognizer` verbatim with real model loading and the real detector.
In-app: `TestContinuousRecognizer` (needs `BRAIDCAST_SELFTEST_VOICE_MODELS` and the
fixture WAVs, so it skips until both exist).
