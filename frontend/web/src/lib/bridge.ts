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
}

/** Known server->client push events and their payload shapes. */
export interface ObsEvents {
  "streaming.changed": { active: boolean };
  "obs.event": { event: string };
  "scenes.changed": Record<string, never>;
  "sceneItems.changed": { scene: string | null };
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
