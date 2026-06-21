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

/** Known bridge methods. Extend as the C++ Bridge gains methods. */
export interface ObsMethods {
  getVersion: string;
  getCurrentScene: string | null;
  listScenes: string[];
  getStreamingState: { active: boolean };
  "streaming.start": { active: boolean };
  "streaming.stop": { active: boolean };
  "preview.setRect": null;
  "preview.hide": null;
  "preview.select": { selected: number | null };
  // Scenes (current = the scene bound to output channel 0).
  "scenes.list": SceneInfo[];
  "scenes.create": { name: string };
  "scenes.remove": { removed: string };
  "scenes.setCurrent": { name: string };
  "scenes.rename": { name: string };
  // Scene items (top-first draw order; omit `scene` to target the current scene).
  "sceneItems.list": SceneItem[];
  "sceneItems.setVisible": { id: number; visible: boolean };
  "sceneItems.setLocked": { id: number; locked: boolean };
  "sceneItems.remove": { removed: number };
  "sceneItems.reorder": { id: number; direction: ReorderDirection };
  // Source types + creation (4.3.3). Omit `scene` to target the current scene.
  "sourceTypes.list": SourceType[];
  "sources.create": { id: number; source: string };
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
}

/** Known server->client push events and their payload shapes. */
export interface ObsEvents {
  "streaming.changed": { active: boolean };
  "obs.event": { event: string };
  "scenes.changed": Record<string, never>;
  "sceneItems.changed": { scene: string | null };
  "sceneItem.selected": { scene: string | null; id: number | null };
  "settings.videoChanged": VideoSettings;
  "settings.audioChanged": AudioSettings;
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
