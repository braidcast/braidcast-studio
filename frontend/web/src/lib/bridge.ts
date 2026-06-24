// Typed JS<->C++ bridge client. Ported from the vanilla obs-bridge.js.
//
// Transport: window.cefQuery (CEF message router) carries a JSON request
// envelope {method, params}; C++ returns the result already JSON-encoded.
// Server pushes arrive through window.__obsEmit(event, payload), which the C++
// Bridge::EmitEvent invokes via ExecuteJavaScript.
//
// Contract:
//   obs.call(method, params) -> Promise<result>
//   obs.on(event, handler)   -> unsubscribe()

// --- ambient CEF / push surface ---------------------------------------------

interface CefQueryRequest {
  request: string;
  onSuccess: (response: string) => void;
  onFailure: (code: number, message: string) => void;
  persistent?: boolean;
}

declare global {
  interface Window {
    cefQuery?: (req: CefQueryRequest) => number;
    __obsEmit?: (event: string, payload: unknown) => void;
    obs?: ObsBridge;
  }
}

// --- typed surface (loose for now; tightened as the API grows) ---------------

/** A scene as reported by scenes.list. */
export interface SceneInfo {
  name: string;
  current: boolean;
}

/** A scene item (source within a scene) as reported by sceneItems.list. */
export interface SceneItem {
  id: number;
  source: string | null;
  visible: boolean;
  locked: boolean;
}

export type ReorderDirection = "up" | "down" | "top" | "bottom";

/** A creatable input source type as reported by sourceTypes.list. */
export interface SourceType {
  id: string;
  name: string;
  caps: { video: boolean; audio: boolean };
}

// --- generic obs_properties descriptors (4.3.2) -----------------------------

/** Editable-object kind a property set belongs to. "filter" addresses a filter
 * by its uuid (the ref); the others address their object by name/id. */
export type PropertyKind = "source" | "encoder" | "service" | "output" | "filter";

/** One item in a list (combo/radio) property. */
export interface PropertyListItem {
  name: string | null;
  value: string | number | boolean;
  disabled: boolean;
}

/** Fields shared by every property descriptor. */
interface PropertyBase {
  name: string;
  label: string | null;
  enabled: boolean;
  visible: boolean;
  long_description?: string;
  /** Live value from the object's settings (type-specific). */
  value?: unknown;
}

export interface BoolProperty extends PropertyBase {
  type: "bool";
  value: boolean;
}
export interface IntProperty extends PropertyBase {
  type: "int";
  min: number;
  max: number;
  step: number;
  int_type: "scroller" | "slider";
  suffix: string | null;
  value: number;
}
export interface FloatProperty extends PropertyBase {
  type: "float";
  min: number;
  max: number;
  step: number;
  float_type: "scroller" | "slider";
  suffix: string | null;
  value: number;
}
export interface TextProperty extends PropertyBase {
  type: "text";
  text_type: "default" | "password" | "multiline" | "info";
  monospace: boolean;
  info_type?: "normal" | "warning" | "error";
  info_word_wrap?: boolean;
  value: string | null;
}
export interface PathProperty extends PropertyBase {
  type: "path";
  path_type: "file" | "file_save" | "directory";
  filter: string | null;
  default_path: string | null;
  value: string | null;
}
export interface ListProperty extends PropertyBase {
  type: "list";
  combo_format: "int" | "float" | "string" | "bool" | "invalid";
  combo_type: "editable" | "list" | "radio";
  items: PropertyListItem[];
  value: string | number | boolean | null;
}
export interface ColorProperty extends PropertyBase {
  type: "color" | "color_alpha";
  value: number;
}
export interface ButtonProperty extends PropertyBase {
  type: "button";
  button_type: "default" | "url";
  url?: string;
}
export interface GroupProperty extends PropertyBase {
  type: "group";
  group_type: "normal" | "checkable";
  props: PropertyDescriptor[];
  value?: boolean;
}
/** A font value. `flags` is a bitmask: BOLD=1, ITALIC=2, UNDERLINE=4, STRIKEOUT=8. */
export interface FontValue {
  face: string;
  style?: string;
  size: number;
  flags: number;
}
export interface FontProperty extends PropertyBase {
  type: "font";
  value: FontValue | null;
}
/** One entry in an editable_list value. `value` is the string the list stores;
 * `uuid`/`selected`/`hidden` are preserved on round-trip when present. */
export interface EditableListItem {
  value: string;
  selected?: boolean;
  hidden?: boolean;
  uuid?: string;
}
export interface EditableListProperty extends PropertyBase {
  type: "editable_list";
  editable_list_type: "strings" | "files" | "files_and_urls";
  filter?: string;
  default_path?: string;
  value: EditableListItem[];
}
/** A frame rate as a rational (numerator/denominator); null = unset. */
export interface FrameRateValue {
  numerator: number;
  denominator: number;
}
export interface FrameRateProperty extends PropertyBase {
  type: "frame_rate";
  fps_options: { name: string; description: string }[];
  fps_ranges: { min: FrameRateValue; max: FrameRateValue }[];
  value: FrameRateValue | null;
}
/** Composite types serialized best-effort; rendered as "unsupported (TODO)". */
export interface UnsupportedProperty extends PropertyBase {
  type: "invalid";
}

export type PropertyDescriptor =
  | BoolProperty
  | IntProperty
  | FloatProperty
  | TextProperty
  | PathProperty
  | ListProperty
  | ColorProperty
  | ButtonProperty
  | GroupProperty
  | FontProperty
  | EditableListProperty
  | FrameRateProperty
  | UnsupportedProperty;

/** Result of properties.get / properties.set / properties.button. */
export interface PropertiesResult {
  props: PropertyDescriptor[];
  values: Record<string, unknown>;
}

// --- settings: video / audio (4.3.5) ----------------------------------------

/** Core video config: base (canvas) + output (scaled) size + frame rate. */
export interface VideoSettings {
  baseWidth: number;
  baseHeight: number;
  outputWidth: number;
  outputHeight: number;
  fpsNum: number;
  fpsDen: number;
}

/** Speaker layouts obs_audio_info accepts. */
export type SpeakerLayout = "mono" | "stereo" | "2.1" | "4.0" | "4.1" | "5.1" | "7.1";

/** Core audio config. */
export interface AudioSettings {
  sampleRate: number;
  speakers: SpeakerLayout;
}

// --- canvases (native multistream encode targets, 4.4.1) --------------------

/** A canvas as reported by canvas.list / returned by canvas.update. */
export interface CanvasInfo {
  uuid: string;
  name: string;
  isDefault: boolean;
  baseWidth: number;
  baseHeight: number;
  outputWidth: number;
  outputHeight: number;
  fpsNum: number;
  fpsDen: number;
  videoEncoder: string;
  audioEncoder: string;
  /** True when >=1 enabled output binds this canvas; the canvas panel is shown
   * only when enabled (Default included -- its panel hides when disabled). */
  enabled: boolean;
}

/** Fields accepted by canvas.create. */
export interface CanvasCreateParams {
  name: string;
  baseWidth: number;
  baseHeight: number;
  outputWidth?: number;
  outputHeight?: number;
  fpsNum: number;
  fpsDen: number;
  videoEncoder?: string;
  audioEncoder?: string;
}

/** Fields accepted by canvas.update (all but uuid optional; name always allowed). */
export interface CanvasUpdateParams {
  uuid: string;
  name?: string;
  baseWidth?: number;
  baseHeight?: number;
  outputWidth?: number;
  outputHeight?: number;
  fpsNum?: number;
  fpsDen?: number;
  videoEncoder?: string;
  audioEncoder?: string;
}

/** An encoder type as reported by encoderTypes.list. */
export interface EncoderType {
  id: string;
  name: string;
}

// --- source filters (chroma key, color correction, noise suppression, ...) ---

/** A creatable filter type as reported by filterTypes.list. `video`/`audio`
 * mark which source streams the filter applies to (an entry may be both). */
export interface FilterType {
  id: string;
  name: string;
  video: boolean;
  audio: boolean;
}

/** One filter in a source's filter chain (chain order) as reported by filters.list. */
export interface FilterInfo {
  name: string;
  id: string;
  uuid: string;
  enabled: boolean;
}

// --- stream profiles (reusable destination credentials, 4.4.2) --------------

/** A stream profile as reported by streamProfile.list / returned by update. */
export interface StreamProfileInfo {
  uuid: string;
  label: string;
  isPrimary: boolean;
  /** Raw service id, e.g. "rtmp_common" | "rtmp_custom" | "whip_custom". */
  service: string;
  /** Display prefix derived from the service (e.g. "YouTube", "Custom", "WHIP"). */
  platform: string;
}

/** Fields accepted by streamProfile.create. */
export interface StreamProfileCreateParams {
  label: string;
  service: string;
  settings?: Record<string, unknown>;
}

/** Fields accepted by streamProfile.update (all but uuid optional). */
export interface StreamProfileUpdateParams {
  uuid: string;
  label?: string;
  service?: string;
  settings?: Record<string, unknown>;
}

/** A registered service type as reported by serviceTypes.list. */
export interface ServiceType {
  id: string;
  name: string;
}

// --- output bindings (profile x canvas routing edges, 4.4.3) -----------------

/**
 * An output binding as reported by outputBinding.list / returned by update.
 * `profileLabel` / `canvasName` are the joined display strings: "(unset)" for an
 * empty reference, "(deleted)" for a uuid whose profile/canvas no longer exists.
 */
export interface OutputBindingInfo {
  uuid: string;
  profileUuid: string;
  profileLabel: string;
  canvasUuid: string;
  canvasName: string;
  enabled: boolean;
}

/** Fields accepted by outputBinding.create (profileUuid optional = unset). */
export interface OutputBindingCreateParams {
  profileUuid?: string;
  canvasUuid: string;
}

/** Fields accepted by outputBinding.update (all but uuid optional). */
export interface OutputBindingUpdateParams {
  uuid: string;
  profileUuid?: string;
  canvasUuid?: string;
}

// --- multistream live status (fan-out engine, 4.4.4) -------------------------

/** Live state of one enabled output binding. */
export type MultistreamState = "idle" | "connecting" | "live" | "error";

/**
 * One status row reported by multistream.status / pushed on multistream.changed
 * (one per enabled binding). `lastError` is set only in the "error" state.
 */
export interface MultistreamStatus {
  bindingUuid: string;
  canvasUuid: string;
  profileLabel: string;
  canvasName: string;
  state: MultistreamState;
  lastError: string;
}

// --- audio mixer (per-source faders + volmeters, levels) --------------------

/** One active audio source as reported by audio.list. */
export interface AudioSource {
  uuid: string;
  name: string;
  /** Fader position, 0..1 (LOG mapping). */
  deflection: number;
  /** Current volume in dB. */
  volumeDb: number;
  muted: boolean;
}

/** One source's coalesced levels (dB) in an audio.levels push. */
export interface AudioLevel {
  uuid: string;
  /** Smoothed magnitude in dB (<= 0; ~-96 = silence). */
  magnitude: number;
  /** Peak in dB (<= 0). */
  peak: number;
}

/** A selectable audio device as reported by audio.listDevices (includes "Default"). */
export interface AudioDevice {
  id: string;
  name: string;
}

/**
 * One global audio channel slot as reported by audio.getGlobalDevices (6 entries:
 * ch1/2 desktop output, ch3-6 mic input). `deviceId: null` = the channel is disabled.
 */
export interface GlobalAudioSlot {
  channel: number;
  role: "desktop" | "mic";
  label: string;
  isInput: boolean;
  deviceId: string | null;
  active: boolean;
}

// --- transitions (scene transition type + duration) -------------------------

// --- scene-item transform (numeric Edit Transform dialog) -------------------

/** Addresses one scene item: omit `canvas` (or pass the Default uuid) for the
 * global channel-0 path; pass an additional canvas's uuid + that canvas's scene
 * for a per-canvas item. Mirrors the shape used by sceneItems.setVisible/setLocked. */
export interface TransformTarget {
  canvas?: string;
  scene?: string;
  id: number;
}

/**
 * A scene item's full geometry as reported by sceneItems.getTransform / returned
 * by setTransform + transformAction. `alignment` / `boundsAlignment` are OBS
 * bitfields (TL=5,TC=4,TR=6,CL=1,C=0,CR=2,BL=9,BC=8,BR=10); `boundsType` is the
 * OBS_BOUNDS_* enum (0=none,1=stretch,2=inner,3=outer,4=width,5=height,6=max).
 * source*=unscaled source pixels; base*=the canvas the item lives on.
 */
export interface Transform {
  pos: { x: number; y: number };
  rot: number;
  scale: { x: number; y: number };
  alignment: number;
  boundsType: number;
  boundsAlignment: number;
  bounds: { x: number; y: number };
  cropToBounds: boolean;
  crop: { left: number; top: number; right: number; bottom: number };
  sourceWidth: number;
  sourceHeight: number;
  baseWidth: number;
  baseHeight: number;
}

/** Quick-action verbs accepted by sceneItems.transformAction. */
export type TransformAction = "reset" | "center" | "fitToScreen" | "stretchToScreen" | "flipH" | "flipV";

// --- projectors (native standalone windows rendering a target, P3) ----------

/** A display/monitor as reported by display.listMonitors. `index` is the stable
 * ordinal the projector.open `monitor` param expects; `x`/`y` are the desktop
 * position (virtual-screen coords). */
export interface Monitor {
  index: number;
  name: string;
  x: number;
  y: number;
  width: number;
  height: number;
  primary: boolean;
}

/** What a projector renders. "program" = the program / default mix (no name);
 * "scene"/"source" address by name; "canvas" addresses an additional canvas by
 * its uuid (passed as `canvas`). */
export type ProjectorTarget =
  | { kind: "program" }
  | { kind: "scene"; name: string }
  | { kind: "source"; name: string }
  | { kind: "canvas"; canvas: string };

/** A live projector as reported by projector.list / returned by projector.open. */
export interface ProjectorInfo {
  projectorId: number;
  target: ProjectorTarget;
  mode: "fullscreen" | "windowed";
  monitor: number | null;
}

/** A registered transition type as reported by transitionTypes.list. */
export interface TransitionType {
  id: string;
  name: string;
}

/** The active transition (type + duration) as reported by transitions.getCurrent. */
export interface TransitionState {
  id: string;
  name: string;
  durationMs: number;
}

/** Known bridge methods. Extend as the C++ Bridge gains methods. */
export interface ObsMethods {
  getVersion: string;
  getCurrentScene: string | null;
  listScenes: string[];
  getStreamingState: { active: boolean };
  "streaming.start": { active: boolean };
  "streaming.stop": { active: boolean };
  // Native preview surfaces. Pass an optional `canvas` uuid to address one
  // canvas's surface; omit it (or pass the Default canvas uuid) for the Default
  // surface (global mix + output 0), preserving today's single-preview caller
  // (4.4.5b). setRect params: {x,y,w,h,dpr,canvas?}; hide/select: {...,canvas?}.
  "preview.setRect": null;
  "preview.hide": null;
  "preview.select": { selected: number | null };
  // Scenes. `current` = the scene bound to channel 0 of the addressed canvas. Pass
  // an optional `canvas` uuid to operate on an additional canvas's own scenes;
  // omit it (or pass the Default canvas uuid) for the global channel-0 path (4.4.5b).
  "scenes.list": SceneInfo[];
  "scenes.create": { name: string };
  "scenes.remove": { removed: string };
  "scenes.setCurrent": { name: string };
  "scenes.rename": { name: string };
  // Duplicate a scene (global channel-0 path only; additional canvases unsupported).
  "scenes.duplicate": { name: string };
  // Scene reorder is NOT supported by the backend: libobs enumerates scenes in
  // creation order and the new frontend has no scene-collection persistence to
  // store a custom order, so this method always rejects with a clear error. Kept
  // typed so a caller gets a compile-time shape; expect the call to throw.
  "scenes.reorder": never;
  // Scene items (top-first draw order; omit `scene` to target the current scene).
  // Pass an optional `canvas` uuid to target an additional canvas's current scene.
  "sceneItems.list": SceneItem[];
  "sceneItems.setVisible": { id: number; visible: boolean };
  "sceneItems.setLocked": { id: number; locked: boolean };
  "sceneItems.remove": { removed: number };
  "sceneItems.reorder": { id: number; direction: ReorderDirection };
  // Numeric transform read/edit (Edit Transform dialog). getTransform loads the
  // full geometry; setTransform applies a partial (send only changed fields) and
  // echoes the full updated transform; transformAction runs a quick action and
  // echoes the result. All three emit sceneItems.changed after mutating.
  "sceneItems.getTransform": Transform;
  "sceneItems.setTransform": Transform;
  "sceneItems.transformAction": Transform;
  // Source types + creation (4.3.3). Omit `scene` to target the current scene; pass
  // an optional `canvas` uuid to add into an additional canvas's current scene.
  "sourceTypes.list": SourceType[];
  "sources.create": { id: number; source: string };
  // Rename a scene item's underlying source (canvas/scene optional, default current).
  "sources.rename": { id: number; source: string };
  "sources.listExisting": string[];
  "sources.addExisting": { id: number; source: string };
  // Generic obs_properties renderer (4.3.2).
  "properties.get": PropertiesResult;
  "properties.set": PropertiesResult;
  "properties.button": PropertiesResult;
  // Native OS file dialog (path / editable_list Browse). `mode` picks an open,
  // save, or directory chooser; `filter` is an OBS-style filter string. Returns
  // { path: null } when the user cancels.
  "dialog.openFile": { path: string | null };
  // Core video/audio settings (4.3.5). set* return the applied (post-reset) values.
  "settings.getVideo": VideoSettings;
  "settings.setVideo": VideoSettings;
  "settings.getAudio": AudioSettings;
  "settings.setAudio": AudioSettings;
  // Canvases (native multistream encode targets, 4.4.1).
  "canvas.list": CanvasInfo[];
  "canvas.create": { uuid: string };
  "canvas.update": CanvasInfo;
  "canvas.remove": { removed: string };
  "encoderTypes.list": EncoderType[];
  // Source filters. filterTypes.list enumerates creatable filter types (optionally
  // narrowed by kind); filters.list returns one source's chain in draw order. add/
  // remove/setEnabled/reorder/rename mutate the chain; the selected filter's
  // obs_properties are edited via PropertyForm kind="filter" (ref = its uuid).
  "filterTypes.list": FilterType[];
  "filters.list": FilterInfo[];
  "filters.add": { name: string; uuid: string };
  "filters.remove": { removed: string };
  "filters.setEnabled": { name: string; enabled: boolean };
  "filters.reorder": { name: string; direction: ReorderDirection };
  "filters.rename": { name: string };
  // Stream profiles (reusable destination credentials, 4.4.2).
  "streamProfile.list": StreamProfileInfo[];
  "streamProfile.create": { uuid: string };
  "streamProfile.update": StreamProfileInfo;
  "streamProfile.remove": { removed: string };
  "streamProfile.setPrimary": { uuid: string; isPrimary: boolean };
  "serviceTypes.list": ServiceType[];
  // Output bindings (profile x canvas routing edges, 4.4.3).
  "outputBinding.list": OutputBindingInfo[];
  "outputBinding.create": { uuid: string };
  "outputBinding.update": OutputBindingInfo;
  "outputBinding.setEnabled": { uuid: string; enabled: boolean };
  "outputBinding.remove": { removed: string };
  // Multistream live status + per-row control (fan-out engine, 4.4.4).
  "multistream.status": { outputs: MultistreamStatus[] };
  "multistream.startOutput": { ok: boolean };
  "multistream.stopOutput": { ok: boolean };
  // Audio mixer (per-source faders + volmeters). list returns the active audio
  // sources; set* return the applied value. Levels arrive via the audio.levels
  // push (throttled to ~30 Hz), not a method.
  "audio.list": { sources: AudioSource[] };
  "audio.setDeflection": { uuid: string; deflection: number; volumeDb: number };
  "audio.setMuted": { uuid: string; muted: boolean };
  // Global audio device pickers (Desktop Audio / Mic channels). setGlobalDevice
  // applies LIVE: the backend creates/updates/removes the source, rebuilds the
  // mixer, and emits audio.changed. deviceId null/"" disables the channel.
  "audio.listDevices": AudioDevice[];
  "audio.getGlobalDevices": GlobalAudioSlot[];
  "audio.setGlobalDevice": { channel: number; deviceId: string | null };
  // Scene transitions. transitionTypes.list enumerates the registered transition
  // types; getCurrent returns the active type + duration. setCurrent/setDuration
  // mutate and echo the applied value; both also emit transitions.changed.
  "transitionTypes.list": TransitionType[];
  "transitions.getCurrent": TransitionState;
  "transitions.setCurrent": { id: string; name: string };
  "transitions.setDuration": { durationMs: number };
  // Native projectors (standalone windows rendering a target on a monitor, P3).
  // listMonitors enumerates the displays a fullscreen projector can target.
  // open spawns a projector (fullscreen needs `monitor`); the window closes
  // itself (Esc / window close), so there is no UI-driven close path beyond
  // projector.close. list enumerates the live projectors. projector.changed is
  // pushed whenever the set opens/closes.
  "display.listMonitors": { monitors: Monitor[] };
  "projector.open": { projectorId: number };
  "projector.close": { closed: boolean };
  "projector.list": { projectors: ProjectorInfo[] };
  // Shell persistence (P1). theme.* stores an opaque JSON blob the JS theme store
  // stringifies/parses into its own schema (active id + live tokens + custom
  // themes); layout.* stores the serialized Dockview state (a JSON string). load
  // returns "" when nothing is saved yet, so the store falls back to defaults; a
  // missing handler also resolves to null (treated as "nothing saved").
  "theme.save": { saved: boolean };
  "theme.load": { state: string };
  "layout.save": { saved: boolean };
  "layout.load": { layout: string };
  // Floating dock tear-out (P3a). detach opens a new OS window whose browser
  // loads index.html?window=<id>&dock=<dock>; redock destroys that window. list
  // enumerates the live detached windows.
  "window.detach": { windowId: number };
  "window.redock": { redocked: number };
  "window.list": { windows: { windowId: number; dock: string }[] };
}

/** Known server->client push events and their payload shapes. */
export interface ObsEvents {
  "streaming.changed": { active: boolean };
  "obs.event": { event: string };
  // `canvas` is the addressed canvas uuid, or null for the global channel-0 path;
  // a per-canvas panel filters to its own canvas before reacting (4.4.5b).
  "scenes.changed": { canvas: string | null };
  "sceneItems.changed": { scene: string | null; canvas: string | null };
  // `canvas` = the addressed canvas uuid, or null for the Default surface (global
  // channel-0 path); a per-canvas dock filters to its own canvas (scene names
  // collide across canvases).
  "sceneItem.selected": { scene: string | null; id: number | null; canvas: string | null };
  // Right-click in a native preview overlay (WM_RBUTTONUP). Broadcast to ALL
  // windows; the host dock filters by `window === WINDOW_ID` + its own canvas
  // (null = Default surface) and maps the device-px cursor to viewport coords via
  // the preview rect + devicePixelRatio. `id == null` = empty area (ignore).
  "preview.contextMenu": {
    canvas: string | null;
    window: number;
    x: number;
    y: number;
    id: number | null;
    scene: string | null;
    source: string | null;
    visible: boolean;
    locked: boolean;
  };
  "settings.videoChanged": VideoSettings;
  "settings.audioChanged": AudioSettings;
  "canvas.changed": Record<string, never>;
  "streamProfile.changed": Record<string, never>;
  "outputBinding.changed": Record<string, never>;
  "multistream.changed": { outputs: MultistreamStatus[] };
  // Coalesced per-source audio levels, pushed at most ~30 Hz off the volmeter
  // callbacks. The UI maps dB -> meter width and applies peak hold.
  "audio.levels": { levels: AudioLevel[] };
  // The active audio source set changed (source activated/deactivated); the UI
  // re-runs audio.list to rebuild its rows.
  "audio.changed": Record<string, never>;
  // The active transition type and/or its duration changed; the UI re-runs
  // transitions.getCurrent to refresh its dropdown + duration field.
  "transitions.changed": Record<string, never>;
  // The set of live native projectors changed (opened/closed, incl. user OS-close
  // or Esc); a consumer re-runs projector.list to refresh.
  "projector.changed": Record<string, never>;
  // Floating dock tear-out (P3a). Broadcast to ALL browsers (main + detached).
  // opened fires after a detached window's browser exists; closed fires on
  // explicit redock AND on user OS-close (NOT during app shutdown).
  "window.opened": { windowId: number; dock: string };
  "window.closed": { windowId: number; dock: string };
}

export interface BridgeError extends Error {
  code?: number;
}

export type Unsubscribe = () => void;

export interface ObsBridge {
  call<K extends keyof ObsMethods>(method: K, params?: unknown): Promise<ObsMethods[K]>;
  call<T = unknown>(method: string, params?: unknown): Promise<T>;
  on<K extends keyof ObsEvents>(event: K, handler: (payload: ObsEvents[K]) => void): Unsubscribe;
  on(event: string, handler: (payload: unknown) => void): Unsubscribe;
}

// --- implementation ----------------------------------------------------------

const subscribers = new Map<string, Set<(payload: unknown) => void>>();

function call<T = unknown>(method: string, params?: unknown): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    if (!window.cefQuery) {
      reject(new Error("bridge unavailable (cefQuery missing)"));
      return;
    }
    let request: string;
    try {
      request = JSON.stringify({ method, params: params === undefined ? null : params });
    } catch (e) {
      reject(new Error("failed to encode params: " + (e as Error).message));
      return;
    }
    window.cefQuery({
      request,
      onSuccess(response: string) {
        // C++ returns the result already JSON-encoded; empty string => null.
        if (response === "" || response === undefined) {
          resolve(null as T);
          return;
        }
        try {
          resolve(JSON.parse(response) as T);
        } catch {
          // Tolerate a bare (non-JSON) string result.
          resolve(response as unknown as T);
        }
      },
      onFailure(code: number, message: string) {
        const err: BridgeError = new Error(message || "bridge error " + code);
        err.code = code;
        reject(err);
      },
    });
  });
}

function on(event: string, handler: (payload: unknown) => void): Unsubscribe {
  let set = subscribers.get(event);
  if (!set) {
    set = new Set();
    subscribers.set(event, set);
  }
  set.add(handler);
  return () => {
    const s = subscribers.get(event);
    if (s) {
      s.delete(handler);
      if (s.size === 0) {
        subscribers.delete(event);
      }
    }
  };
}

// Server-push entry point. C++ Bridge::EmitEvent ExecuteJavaScripts a call to
// window.__obsEmit; it fans out to subscribers registered via obs.on().
function emit(event: string, payload: unknown): void {
  const set = subscribers.get(event);
  if (!set) {
    return;
  }
  // Copy so a handler unsubscribing mid-dispatch doesn't invalidate iteration.
  for (const handler of Array.from(set)) {
    try {
      handler(payload);
    } catch (e) {
      console.log("OBSBRIDGE: handler for '" + event + "' threw: " + (e as Error).message);
    }
  }
}

export const obs: ObsBridge = { call, on };

// Install the push sink and keep window.obs assigned for parity with the
// vanilla client / any non-module consumer.
window.__obsEmit = emit;
window.obs = obs;
