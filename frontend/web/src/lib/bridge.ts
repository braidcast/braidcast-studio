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

/** Editable-object kind a property set belongs to. "source" is wired now. */
export type PropertyKind = "source" | "encoder" | "service" | "output";

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
/** Composite types serialized best-effort; rendered as "unsupported (TODO)". */
export interface UnsupportedProperty extends PropertyBase {
  type: "font" | "editable_list" | "frame_rate" | "invalid";
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
  // Shell persistence (P1). theme.* stores the active preset id; layout.* stores
  // the serialized Dockview state (a JSON string). load returns "" when nothing
  // is saved yet, so the shell falls back to the default preset / default layout;
  // a missing handler also resolves to null (treated as "nothing saved").
  "theme.save": { saved: boolean };
  "theme.load": { activePreset: string };
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
