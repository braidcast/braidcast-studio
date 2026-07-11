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

import type { BridgeEvent } from "./eventNames";

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
  scaleFilter: string;
  interactive?: boolean;
  // Per-item color tag (hex like "#RRGGBB"; "" when unset).
  color: string;
}

export type ReorderDirection = "up" | "down" | "top" | "bottom";

// Per-source deinterlacing mode + field order tokens (sources.get/setDeinterlace).
export type DeinterlaceMode =
  | "disable"
  | "discard"
  | "retro"
  | "blend"
  | "blend2x"
  | "linear"
  | "linear2x"
  | "yadif"
  | "yadif2x";
export type DeinterlaceFieldOrder = "top" | "bottom";

/** One source whose backing media file can no longer be found (sources.findMissing). */
export interface MissingFile {
  source: string;
  originalPath: string;
}

/** A creatable input source type as reported by sourceTypes.list. */
export interface SourceType {
  id: string;
  name: string;
  caps: { video: boolean; audio: boolean };
}

/** An existing source offered by sources.listExisting, with its type id and
 * coarse caps so the UI can render a per-type icon. */
export interface ExistingSource {
  name: string;
  typeId: string;
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

/** Core audio config. `monitoringDevice` is the device the audio-monitoring mix
 * is routed to (a leading {id:"default"} entry from audio.listMonitorDevices). */
export interface AudioSettings {
  sampleRate: number;
  speakers: SpeakerLayout;
  monitoringDevice: MonitorDevice;
}

/** A selectable monitoring device as reported by audio.listMonitorDevices
 * (includes a leading {id:"default", name:"Default"}). */
export interface MonitorDevice {
  id: string;
  name: string;
}

/** How a source's audio is monitored, mapping the OBS_MONITORING_TYPE_* enum. */
export type AudioMonitoringType = "none" | "monitorOnly" | "monitorAndOutput";

/** General app settings (snapping, projectors, go-live warnings, system tray,
 * multiview, importer prompts). A flat object; setGeneral applies any present
 * subset and echoes the full post-apply state. `snapDistance` is clamped 0..100
 * server-side; `multiviewLayout` is one of the multiview layout ids. */
export interface GeneralSettings {
  projectorAlwaysOnTop: boolean;
  snapEnabled: boolean;
  snapDistance: number;
  snapToEdge: boolean;
  snapToSource: boolean;
  snapToCenter: boolean;
  warnBeforeGoLive: boolean;
  warnBeforeStop: boolean;
  startMinimized: boolean;
  minimizeToTray: boolean;
  alwaysShowTray: boolean;
  multiviewLayout: string;
  multiviewDrawNames: boolean;
  multiviewDrawSafeAreas: boolean;
  importerPrompts: boolean;
  scenesGridMode: boolean;
}

/** Advanced app settings (process priority, stream delay, auto-reconnect, network,
 * browser HW accel). A flat object; setAdvanced applies any present subset, persists,
 * and echoes the full post-apply state. `processPriority` is one of normal/aboveNormal/
 * high; `bindIP` is "default" or an IP string; the rest are booleans except the three
 * uints (streamDelaySec, reconnectRetryDelaySec, reconnectMaxRetries). */
export interface AdvancedSettings {
  processPriority: string;
  streamDelayEnabled: boolean;
  streamDelaySec: number;
  streamDelayPreserve: boolean;
  reconnectEnabled: boolean;
  reconnectRetryDelaySec: number;
  reconnectMaxRetries: number;
  bindIP: string;
  newSocketLoop: boolean;
  lowLatencyMode: boolean;
  dynamicBitrate: boolean;
  browserHwAccel: boolean;
}

/** A source's advanced audio properties (Advanced Audio Properties dialog).
 * `volumeDb` is null when muted to silence (-inf); `tracks` is the 6 mixer-track
 * enable flags; `balance` is 0..1 (0.5 = center). */
export interface AdvancedAudio {
  volumeDb: number | null;
  forceMono: boolean;
  balance: number;
  syncOffsetMs: number;
  tracks: boolean[];
  monitoringType: AudioMonitoringType;
}

// --- canvases (native multistream encode targets, 4.4.1) --------------------

/** A canvas's color/format settings. `format`/`space`/`range` are the libobs
 * tokens (e.g. format "NV12", space "709", range "Partial"); changing any of the
 * three is structural (refused while the canvas is live). `sdrWhiteLevel`/
 * `hdrNominalPeakLevel` persist but have no pipeline effect. `useDefault` (non-
 * default canvases only) inherits the Default canvas's color settings. */
export interface CanvasColor {
  format: string;
  space: string;
  range: string;
  sdrWhiteLevel: number;
  hdrNominalPeakLevel: number;
  useDefault: boolean;
}

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
  scaleType: string;
  /** Inherit resolution/fps from the Default canvas (color inheritance lives on
   * `color.useDefault`; encoder inheritance on `videoUseDefault`/`audioUseDefault`). */
  useDefaultResolution: boolean;
  videoEncoder: string;
  audioEncoder: string;
  videoUseDefault: boolean;
  audioUseDefault: boolean;
  color: CanvasColor;
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
  scaleType?: string;
  useDefaultResolution?: boolean;
  videoEncoder?: string;
  audioEncoder?: string;
  videoUseDefault?: boolean;
  audioUseDefault?: boolean;
  color?: {
    format?: string;
    space?: string;
    range?: string;
    sdrWhiteLevel?: number;
    hdrNominalPeakLevel?: number;
    useDefault?: boolean;
  };
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
  scaleType?: string;
  useDefaultResolution?: boolean;
  videoEncoder?: string;
  audioEncoder?: string;
  videoUseDefault?: boolean;
  audioUseDefault?: boolean;
  color?: {
    format?: string;
    space?: string;
    range?: string;
    sdrWhiteLevel?: number;
    hdrNominalPeakLevel?: number;
    useDefault?: boolean;
  };
}

/** Fields accepted by canvas.reorder / streamProfile.reorder: the full ordered list of uuids. */
export interface ReorderParams {
  order: string[];
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

/** One filter in a copied chain (filters.copyChain entry / filters.pasteChain input). */
export interface CopiedFilter {
  id: string | null;
  name: string | null;
  settings: Record<string, unknown>;
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
  /** Full selected service string (e.g. "YouTube - RTMPS"); for WHIP/custom the
   * server URL or a generic label. Never empty. */
  serviceLabel: string;
  /** Linked OAuth account ("providerId:userId"); empty for key/RTMP/WHIP modes.
   * The reuse link: several profiles may carry the same accountId. */
  accountId: string;
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

// --- platform OAuth (Connect Account dual path, Phase 8) ---------------------

/** One field a provider exposes in its capability descriptor. `tier`/`shareable`
 * drive how the 8b stream-info modal groups/persists the field; 8a only reads the
 * provider id/displayName, so the rest is carried loosely. */
export interface OAuthProviderField {
  key: string;
  label: string;
  type: string;
  tier?: string;
  shareable?: boolean;
  /** enum/labelset choices as {value,label}; `value` is submitted, `label` is shown verbatim. */
  options?: { value: string; label: string }[];
  /** Soft max length (text/tags) carried for hint/validation; advisory only. */
  max?: number;
}

/** A streaming platform that supports account connection (oauth.providers). The
 * list is empty in a build without the platform's client id, so callers must treat
 * an absent provider as "stream-key only". */
export interface OAuthProvider {
  id: string;
  displayName: string;
  brandColor: string;
  auth: { strategy: string; scopes: string[]; needsSecret: boolean };
  fields: OAuthProviderField[];
}

/** Connection state of one connected account (oauth.status). Rows are keyed by
 * `accountId` ("providerId:userId") — one row per account, NOT per profile, so
 * several profiles that reuse the same account share a single row. A profile
 * resolves its state by matching its own `accountId` against these rows.
 * `connected` gates the green "linked" panel/chip; `displayName`/`login` name the
 * account. `needsReconnect` flags a token issued under an older scope set: it
 * reports `connected:false` but is distinct from "never linked" — the backend
 * refuses streamMeta for it, so the UI shows a warn "reconnect" state instead of a
 * plain key-only state. */
export interface OAuthStatus {
  accountId: string;
  providerId: string;
  connected: boolean;
  needsReconnect: boolean;
  login: string;
  displayName: string;
  avatarUrl: string;
}

/** One connected account for a provider (oauth.accounts). Powers the Streams
 * "reuse existing account" picker: link an already-connected account to a second
 * profile without a fresh grant. `needsReconnect` marks a stale-scope token. */
export interface OAuthAccountRow {
  accountId: string;
  login: string;
  displayName: string;
  needsReconnect: boolean;
}

/** Connect progress pushed during oauth.connect (oauth.connectProgress). `phase`
 * discriminates the auth strategy: "deviceCode" carries the `userCode` the user enters
 * in the already-opened `verificationUri` (counts down from `expiresSec`); "browser" is
 * a PKCE loopback flow with no code — a browser window was opened automatically and the
 * bridge waits, so the modal only shows `message`. */
export interface DeviceCodeProgress {
  profileUuid: string;
  providerId: string;
  phase: "deviceCode";
  userCode: string;
  verificationUri: string;
  expiresSec: number;
}
export interface BrowserAuthProgress {
  profileUuid: string;
  providerId: string;
  phase: "browser";
  message: string;
  timeoutSec?: number;
}
export type OAuthConnectProgress = DeviceCodeProgress | BrowserAuthProgress;

/** A profile's stream metadata (Go Live "Stream Information") as returned by
 * streamMeta.get. `category` is a nested {id,name} (id "" when unset); `language`
 * is the broadcaster language code. */
export interface StreamMeta {
  title: string;
  category: { id: string; name: string };
  language: string;
}

/** One category/game match (streamMeta.searchCategories). `boxArt` is the box-art
 * image URL (may contain {width}/{height} placeholders). */
export interface StreamCategory {
  id: string;
  name: string;
  boxArt: string;
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

// One scene-link row: a non-default canvas (uuid) follows a main scene (uuid).
// Names are resolved server-side for display; may be empty if a uuid no longer
// resolves (stale row pending prune).
export interface SceneLinkInfo {
  mainScene: string;
  mainSceneName: string;
  canvas: string;
  canvasScene: string;
  canvasSceneName: string;
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
export type TransformAction =
  | "reset"
  | "center"
  | "fitToScreen"
  | "stretchToScreen"
  | "flipH"
  | "flipV"
  | "rotate90cw"
  | "rotate90ccw"
  | "rotate180"
  | "centerVertical"
  | "centerHorizontal";

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
  | { kind: "canvas"; canvas: string }
  | { kind: "multiview"; canvas?: string };


// --- hotkeys (global + per-object key bindings) ------------------------------

/** Who registered a hotkey, used to group the list. "frontend" = app-level
 * actions; the rest are owned by a libobs object named by `owner`. */
export type HotkeyRegisterer = "frontend" | "source" | "output" | "encoder" | "service" | "unknown";

/** One resolved key binding for a hotkey. `display` is the human-readable combo
 * the backend formats (e.g. "Ctrl+Shift+A", "F1"). */
export interface HotkeyBinding {
  display: string;
}

/** A key combo to assign, expressed with a DOM KeyboardEvent.code plus modifier
 * flags. `code` is e.g. "KeyA" | "F1" | "Space". */
export interface HotkeyCombo {
  code: string;
  ctrl: boolean;
  shift: boolean;
  alt: boolean;
  meta: boolean;
}

/** A hotkey as reported by hotkeys.list. `owner` names the libobs object that
 * registered it (source/output/...), or null for frontend hotkeys. */
export interface Hotkey {
  id: string;
  name: string;
  description: string;
  registerer: HotkeyRegisterer;
  owner: string | null;
  bindings: HotkeyBinding[];
}

// --- stats (perf monitoring, general + per-output) --------------------------

/** General app/engine stats as reported in stats.get's `general`. */
export interface GeneralStats {
  cpu: number;
  memoryMB: number;
  fps: number;
  avgFrameMs: number;
  renderLagged: number;
  renderTotal: number;
  renderLagPct: number;
  encodeSkipped: number;
  encodeTotal: number;
  encodeSkipPct: number;
}

/** Per-output live stats row reported in stats.get's `outputs`. `state` mirrors
 * the multistream live state but is reported title-cased by the stats bridge. */
export interface OutputStat {
  bindingUuid: string;
  profileLabel: string;
  canvasName: string;
  state: "Idle" | "Connecting" | "Live" | "Error";
  bitrateKbps: number;
  droppedFrames: number;
  totalFrames: number;
  dropPct: number;
  congestionPct: number;
  durationMs: number;
}

/** Snapshot returned by stats.get (polled by the Stats dock). */
export interface Stats {
  general: GeneralStats;
  outputs: OutputStat[];
}

// --- MCP server (embedded AI-control HTTP endpoint, Phase 5) -----------------

/**
 * The embedded MCP server's live config + status, as reported by mcp.getConfig /
 * returned by mcp.setConfig / pushed via mcp.changed. `endpoint` is the URL a
 * client connects to (http://127.0.0.1:<port>/mcp); `listening` reflects whether
 * the localhost HTTP server is actually bound, with `lastError` set when it
 * failed to start.
 */
export interface McpConfig {
  enabled: boolean;
  port: number;
  token: string;
  allowMutations: boolean;
  allowGoLive: boolean;
  listening: boolean;
  lastError: string;
  endpoint: string;
}

/** Fields accepted by mcp.setConfig (all optional; omitted fields are unchanged). */
export interface McpSetConfigParams {
  enabled?: boolean;
  port?: number;
  allowMutations?: boolean;
  allowGoLive?: boolean;
}

// --- scene collections (named scene/source sets, switchable, Phase 6a) -------

/** A scene collection as reported by collections.list. Exactly one is active. */
export interface CollectionInfo {
  id: string;
  name: string;
  active: boolean;
}

// --- undo/redo (engine undo stack mirror) -----------------------------------

/** Undo-stack state reported by undo.state and pushed on undo.changed. `undoName`/
 * `redoName` name the next action either way undoes/redoes (empty when none). */
export interface UndoState {
  canUndo: boolean;
  canRedo: boolean;
  undoName: string;
  redoName: string;
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

// --- virtual camera (output one canvas's video as a system webcam) ----------

/** Virtual-camera live state as reported by virtualCam.status / pushed on
 * virtualCam.changed. `canvas` is the target canvas uuid (empty = Default). */
export interface VirtualCamStatus {
  active: boolean;
  canvas: string;
}

/** Virtual-camera config as reported by virtualCam.getConfig / returned by
 * setConfig. `canvas` = target canvas uuid (empty = Default / global program). */
export interface VirtualCamConfig {
  canvas: string;
}

// --- custom browser docks (user-defined web panels, Task 12) -----------------

/** One user-defined browser dock: an {id,title,url} that becomes a Dockview panel
 * hosting an <iframe src=url>. `id` is frontend-generated and stable across edits. */
export interface BrowserDock {
  id: string;
  title: string;
  url: string;
}

// --- OBS Studio importer (read-only, Item 17) --------------------------------

/** One scene collection found in an external OBS Studio install. `file` is the
 * collection's source filename (the import key); `scenes` lists its scene names. */
export interface ImporterCollection {
  name: string;
  file: string;
  scenes: string[];
}

/** Stream destination found in the active OBS Studio profile. */
export interface ImporterService {
  present: boolean;
  label: string;
}

/** Video pipeline settings read from the active OBS Studio profile. `fps` is the
 * resolved frames-per-second as a number (e.g. 30, 59.94). */
export interface ImporterVideo {
  baseWidth: number;
  baseHeight: number;
  outputWidth: number;
  outputHeight: number;
  fps: number;
}

/** Audio settings read from the active OBS Studio profile. */
export interface ImporterAudio {
  sampleRate: number;
  channels: string;
}

/** Result of scanning an OBS Studio install (`importer.scan`). `found: false` ->
 * no install at the resolved path (offer Browse). `service`/`video`/`audio` are
 * null when the active profile lacks that data. */
export interface ImporterScan {
  found: boolean;
  path: string;
  collections: ImporterCollection[];
  service: ImporterService | null;
  video: ImporterVideo | null;
  audio: ImporterAudio | null;
}

/** One collection in an `importer.import` request. Omit/empty `scenes` to import
 * the whole collection; otherwise the listed scenes (+ their dependency closure).
 * `name` optionally overrides the imported collection name. */
export interface ImporterImportCollection {
  file: string;
  name?: string;
  scenes?: string[];
}

/** `importer.import` request payload. */
export interface ImporterImportRequest {
  path?: string;
  collections: ImporterImportCollection[];
  importService: boolean;
  importVideo: boolean;
  importAudio: boolean;
  importGlobalAudio: boolean;
}

/** Result of an import run (`importer.import`). */
export interface ImporterImportResult {
  ok: boolean;
  imported: { collections: number; service: boolean; video: boolean; audio: boolean };
  warnings: string[];
}

// --- multichat + aggregate viewer count (creator engagement, Phase 9.0) ------

/** A streaming platform that participates in multichat / the viewer count. */
export type ChatPlatform = "twitch" | "youtube" | "kick";

/** One badge on a chat author. `kind` is the platform's badge id (e.g.
 * "subscriber", "moderator", "broadcaster"); `url` is the badge image when the
 * platform supplies one (render a short `kind` label when absent). */
export interface ChatBadge {
  kind: string;
  url?: string;
}

/** A chat author, normalized across platforms. `color` is the author's chosen
 * name color (hex "#RRGGBB"; "" when unset -> fall back to a platform color). */
export interface ChatAuthor {
  name: string;
  color: string;
  badges: ChatBadge[];
}

/** One piece of a message body: plain text (rendered as text, never HTML) or an
 * inline emote image (rendered as an <img> from the provided url only). */
export type ChatFragment =
  | { type: "text"; text: string }
  | { type: "emote"; code: string; url: string };

/** One normalized chat message (the `chat.message` event). `id` is the platform
 * message id (dedupe/list key); `ts` is epoch ms; `channelId` is the platform
 * channel it arrived on. */
export interface ChatMessage {
  platform: ChatPlatform;
  channelId: string;
  id: string;
  ts: number;
  author: ChatAuthor;
  fragments: ChatFragment[];
}

/** Per-platform chat connection state. The `chat.state` METHOD returns the full
 * array (one row per active transport); the `chat.state` EVENT carries a SINGLE
 * platform's row (merge it into the local set by `platform`). An empty method
 * result means nothing is connected (not live). */
export interface ChatState {
  platform: ChatPlatform;
  connected: boolean;
  error?: string;
}

/** Aggregate viewer count (the `viewers.changed` event). `perAccount` maps an
 * accountId ("providerId:userId") to its current concurrent viewers; `total` is the
 * sum across accounts. A per-platform breakdown is derived by summing the entries
 * whose accountId prefix matches a providerId (two accounts on one platform add up). */
export interface ViewerCounts {
  perAccount: Record<string, number>;
  total: number;
}

// `channels.stats` event: audience totals per account. audienceCount === -1 means
// unknown/hidden; audienceKind labels it ("followers" | "subscribers"); viewers is
// merged in from viewers.changed by the store, not carried here.
export interface ChannelStatEntry {
  audienceCount: number;
  audienceKind: "followers" | "subscribers" | "";
  audienceHidden: boolean;
  audienceUpdatedNs: number;
}
export interface ChannelStats {
  perAccount: Record<string, ChannelStatEntry>;
}

/** The kind of platform event surfaced in the cross-platform events feed. */
export type EventType =
  | "follow"
  | "sub"
  | "resub"
  | "subgift"
  | "cheer"
  | "raid"
  | "superchat"
  | "supersticker"
  | "member";

/** One normalized platform event (the `events.new` event; the `events.list`
 * method and `events.backfill` event carry arrays of these, newest-first).
 * Optional fields are omitted by the host when empty/zero: `amount` is cheer
 * bits / superchat minor units / raid viewers; `actorColor` falls back to a
 * platform color when absent; `message` is rendered as plain text, never HTML. */
export interface NormalizedEvent {
  id: string;
  platform: ChatPlatform;
  type: EventType;
  ts: number;
  actorName: string;
  actorColor?: string;
  amount?: number;
  currency?: string;
  tier?: string;
  months?: number;
  count?: number;
  message?: string;
}

// --- overlay widgets (loopback SSE overlays, Phase 9.3) ----------------------

/** One field in a widget's Fields system. `type` drives the values-panel editor;
 * `options` (dropdown), `min`/`max`/`step` (slider) are type-specific. `value` feeds
 * fieldData; for image/sound uploads it is the served "assets/<file>" path. */
export interface OverlayField {
  key: string;
  type:
    | "text"
    | "number"
    | "color"
    | "dropdown"
    | "checkbox"
    | "slider"
    | "image-upload"
    | "sound-upload"
    | "font";
  label: string;
  default: unknown;
  value: unknown;
  options?: { value: string; label: string }[];
  min?: number;
  max?: number;
  step?: number;
}

export interface OverlayAsset {
  key: string;
  kind: string;
  file: string;
}

/** Full widget definition (overlays.get / overlays.create). */
export interface OverlayWidget {
  id: string;
  token: string;
  name: string;
  type: string;
  html: string;
  css: string;
  js: string;
  fields: OverlayField[];
  assets: OverlayAsset[];
  url: string;
}

/** Compact list row (overlays.list). */
export interface OverlayListItem {
  id: string;
  name: string;
  type: string;
  token: string;
  url: string;
}

/** overlays.serverInfo — surfaces port instability so the UI can prompt re-copy. */
export interface OverlayServerInfo {
  port: number;
  listening: boolean;
  portChanged: boolean;
}

/** Partial patch for overlays.update. */
export interface OverlayUpdateParams {
  id: string;
  name?: string;
  html?: string;
  css?: string;
  js?: string;
  fields?: OverlayField[];
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
  "preview.destroy": null;
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
  // Deep-copy a scene (its own scene-level filters + every item's SOURCE, filters
  // included) from one canvas onto another (or the same one). Params: {name,
  // canvas?, destCanvas}; canvas omitted/empty means the Default canvas.
  "scenes.duplicateToCanvas": { name: string; uuid: string };
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
  "sceneItems.setScaleFilter": Record<string, never>;
  // Set a per-item color tag ({ scene, id, canvas?, color }; color "" clears it).
  // Emits sceneItems.changed.
  "sceneItems.setColor": { ok: true };
  "sceneItems.remove": { removed: number };
  "sceneItems.reorder": { id: number; direction: ReorderDirection };
  // Group selected items into a new group / dissolve a group (neither is undoable yet).
  "sceneItems.group": { id: number; source: string };
  "sceneItems.ungroup": { ungrouped: boolean };
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
  "sources.listExisting": ExistingSource[];
  "sources.addExisting": { id: number; source: string };
  // Duplicate the source of a scene item in place (undo-recorded).
  "sources.duplicate": { id: number; source: string };
  // Open a native Interact window forwarding input to an interactive source.
  "sources.interact": { ok: boolean; interactId: number };
  // Missing media repair. findMissing lists sources whose backing file is gone;
  // relinkMissing ({ source, originalPath, newPath }) repoints one and emits
  // scenes.changed.
  "sources.findMissing": MissingFile[];
  "sources.relinkMissing": { ok: true };
  // Per-source deinterlacing ({ source } / { source, mode?, fieldOrder? }). Both
  // echo the applied mode + field order.
  "sources.getDeinterlace": { mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder };
  "sources.setDeinterlace": { mode: DeinterlaceMode; fieldOrder: DeinterlaceFieldOrder };
  // Generic obs_properties renderer (4.3.2).
  "properties.get": PropertiesResult;
  "properties.set": PropertiesResult;
  "properties.button": PropertiesResult;
  // Native OS file dialog (path / editable_list Browse). `mode` picks an open,
  // save, or directory chooser; `filter` is an OBS-style filter string. Returns
  // { path: null } when the user cancels.
  "dialog.openFile": { path: string | null };
  // Reveal a path in the system file manager (file highlighted in its folder;
  // directory opened directly).
  "shell.revealPath": { ok: boolean };
  // Read a local file ({path}) and return it as a base64 data: URI. CEF's app://
  // origin refuses to load file:// resources, so a local-image preview must be
  // inlined. Caps at 10 MB and rejects missing/unreadable files (throws to JS).
  "file.readDataUri": { dataUri: string };
  // Core video/audio settings (4.3.5). set* return the applied (post-reset) values.
  "settings.getVideo": VideoSettings;
  "settings.setVideo": VideoSettings;
  "settings.getAudio": AudioSettings;
  "settings.setAudio": AudioSettings;
  // General app settings (snapping/projectors/go-live warnings/tray/multiview/
  // importer). setGeneral applies any present subset, persists, and echoes the
  // full post-apply state (snapDistance clamped 0..100 server-side).
  "settings.getGeneral": GeneralSettings;
  "settings.setGeneral": GeneralSettings;
  // Advanced app settings (process priority/stream delay/auto-reconnect/network/
  // browser HW accel). setAdvanced applies any present subset, persists, and echoes
  // the full post-apply state.
  "settings.getAdvanced": AdvancedSettings;
  "settings.setAdvanced": AdvancedSettings;
  // Canvases (native multistream encode targets, 4.4.1).
  "canvas.list": CanvasInfo[];
  "canvas.create": { uuid: string };
  "canvas.update": CanvasInfo;
  "canvas.remove": { removed: string };
  // Persisted drag order (echoes applied uuids); supported, unlike scenes.reorder (never).
  "canvas.reorder": { order: string[] };
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
  // Copy/paste a whole filter chain between sources (paste is not yet undoable).
  "filters.copyChain": { filters: CopiedFilter[] };
  "filters.pasteChain": { pasted: number };
  // Stream profiles (reusable destination credentials, 4.4.2).
  "streamProfile.list": StreamProfileInfo[];
  "streamProfile.create": { uuid: string };
  "streamProfile.update": StreamProfileInfo;
  "streamProfile.remove": { removed: string };
  "streamProfile.setPrimary": { uuid: string; isPrimary: boolean };
  // Persisted drag order (echoes applied uuids); supported, unlike scenes.reorder (never).
  "streamProfile.reorder": { order: string[] };
  "serviceTypes.list": ServiceType[];
  // Platform OAuth (Connect Account dual path, Phase 8; account entity, Phase 4).
  // providers enumerates the platforms that support account connection (empty in a
  // build without a client id -> stream-key only); connect ({providerId,
  // profileUuid}) kicks off the connect flow and returns immediately ({pending:true})
  // -- the real result arrives via the oauth.connectProgress / oauth.status /
  // oauth.connectError events; accounts ({providerId}) lists a provider's connected
  // accounts for the reuse picker; linkAccount ({profileUuid, accountId}) links an
  // already-connected account to another profile (connect-once reuse); disconnect
  // ({accountId, force?}) removes an account -- returns {needsConfirm, profiles} when
  // >1 profile references it (retry with force:true); status reports per-account link
  // state (keyed by accountId).
  "oauth.providers": OAuthProvider[];
  "oauth.connect": { ok: boolean; pending: boolean };
  // Cancel an in-flight connect (dialog closed before authorization);
  // aborts the backend flow so the profile is not linked after the modal is gone.
  "oauth.cancelConnect": { ok: true };
  "oauth.accounts": OAuthAccountRow[];
  "oauth.linkAccount": { ok: true };
  "oauth.disconnect": { ok: true } | { needsConfirm: true; profiles: { uuid: string; name: string }[] };
  "oauth.status": OAuthStatus[];
  // Stream metadata (Go Live "Stream Information": title / category / language).
  // get ({accountId}) loads the account's current metadata ({title, category:{id,name},
  // language}); searchCategories resolves a query to game/category matches ({id, name,
  // boxArt}); set ({accountId, profileUuid, fields}) persists and returns {ok:true},
  // emitting streamMeta.changed. accountId keys the token/provider; profileUuid is only
  // forwarded into the write's UI-thread-marshalled ingest writeback.
  "streamMeta.get": StreamMeta;
  "streamMeta.searchCategories": StreamCategory[];
  "streamMeta.set": { ok: true };
  // Remembered-metadata store (no provider/network). getSaved ({accountId,
  // profileUuids}) returns the persisted channel defaults plus per-profile overrides
  // (only uuids that have a stored bag); save ({accountId, channel, streams}) writes
  // both and persists to stream_meta.json.
  "streamMeta.getSaved": { channel: Record<string, unknown>; streams: Record<string, Record<string, unknown>> };
  "streamMeta.save": { ok: true };
  // Output bindings (profile x canvas routing edges, 4.4.3).
  "outputBinding.list": OutputBindingInfo[];
  "outputBinding.create": { uuid: string };
  "outputBinding.update": OutputBindingInfo;
  "outputBinding.setEnabled": { uuid: string; enabled: boolean };
  "outputBinding.remove": { removed: string };
  // Scene links (a non-default canvas follows a main scene). list returns every
  // link with names resolved for display; set creates/updates a link (mainScene =
  // main scene NAME, canvas = canvas UUID, canvasScene = canvas scene NAME); clear
  // removes one. set/clear return {} and emit sceneLink.changed.
  "sceneLink.list": { links: SceneLinkInfo[] };
  "sceneLink.set": Record<string, never>;
  "sceneLink.clear": Record<string, never>;
  // Multistream live status (fan-out engine, 4.4.4). Start/stop is a single global
  // action via streaming.start/stop; there is no per-row control method.
  "multistream.status": { outputs: MultistreamStatus[] };
  // Virtual camera (output one canvas's video as a system webcam). start/stop
  // toggle the output; status reports the live state; get/setConfig read/write the
  // target canvas (empty/unknown/Default uuid -> the global program video). start/
  // stop/setConfig all emit virtualCam.changed.
  "virtualCam.start": { ok: true };
  "virtualCam.stop": { ok: true };
  "virtualCam.status": VirtualCamStatus;
  "virtualCam.getConfig": VirtualCamConfig;
  "virtualCam.setConfig": VirtualCamConfig;
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
  // Advanced audio properties (per-source: volume/mono/balance/sync/tracks/
  // monitoring). getAdvanced loads the full state; setAdvanced applies a partial
  // (send only changed fields) and echoes the full post-apply state. Both also
  // emit audio.changed. listMonitorDevices enumerates the monitoring outputs.
  "audio.getAdvanced": AdvancedAudio;
  "audio.setAdvanced": AdvancedAudio;
  "audio.listMonitorDevices": MonitorDevice[];
  // Scene transitions. transitionTypes.list enumerates the registered transition
  // types; getCurrent returns the active type + duration. setCurrent/setDuration
  // mutate and echo the applied value; both also emit transitions.changed.
  "transitionTypes.list": TransitionType[];
  "transitions.getCurrent": TransitionState;
  "transitions.setCurrent": { id: string; name: string };
  "transitions.setDuration": { durationMs: number };
  // Hotkeys (global + per-object key bindings). list enumerates every registered
  // hotkey with its current bindings; set replaces a hotkey's bindings (one combo
  // per call is the MVP) and echoes the formatted displays; clear removes them.
  // All three emit hotkeys.changed after mutating.
  "hotkeys.list": { hotkeys: Hotkey[] };
  "hotkeys.set": { bindings: HotkeyBinding[] };
  "hotkeys.clear": { bindings: HotkeyBinding[] };
  // Embedded MCP server (AI-control localhost endpoint, Phase 5). getConfig
  // reports the live config + listening status; setConfig applies a partial and
  // echoes the full updated config; regenerateToken rotates the bearer token
  // (invalidating existing clients) and returns the new one. All mutations also
  // emit mcp.changed.
  "mcp.getConfig": McpConfig;
  "mcp.setConfig": McpConfig;
  "mcp.regenerateToken": { token: string };
  // Custom browser docks (user-defined {id,title,url} web panels, Task 12). list
  // returns the saved set; set overwrites the whole list (the frontend manages
  // add/edit/remove client-side then persists the full list) and echoes it back.
  "browserDocks.list": BrowserDock[];
  "browserDocks.set": BrowserDock[];
  // Screenshots (composited program / one source's video -> a timestamped PNG in
  // the config screenshots dir). takeProgram optionally targets a canvas (omit, or
  // pass the Default uuid, for the global program); takeSource addresses a scene
  // item ({ scene, id, canvas? }, mirroring sceneItems.setScaleFilter). Both echo
  // the saved path and emit screenshot.saved.
  "screenshot.takeProgram": { ok: boolean; path: string };
  "screenshot.takeSource": { ok: boolean; path: string };
  // Stats snapshot (general perf + per-output live stats). Polled by the Stats
  // dock on a ~1s interval; there is no push, so the dock owns the cadence.
  "stats.get": Stats;
  // Native projectors (standalone windows rendering a target on a monitor, P3).
  // listMonitors enumerates the displays a fullscreen projector can target. open
  // spawns a projector (fullscreen needs `monitor`); the window closes itself (Esc /
  // window close), and projector.changed is pushed whenever the set opens/closes.
  "display.listMonitors": { monitors: Monitor[] };
  "projector.open": { projectorId: number };
  // Shell persistence (P1). theme.* stores an opaque JSON blob the JS theme store
  // stringifies/parses into its own schema (active id + live tokens + custom
  // themes); layout.* stores the serialized Dockview state (a JSON string). load
  // returns "" when nothing is saved yet, so the store falls back to defaults; a
  // missing handler also resolves to null (treated as "nothing saved").
  "theme.save": { saved: boolean };
  "theme.load": { state: string };
  "layout.save": { saved: boolean };
  "layout.load": { layout: string };
  // Current session log (Item 16). getCurrent returns the active log file path plus
  // its contents, tail-capped at 512KB by the backend. No params; the viewer modal
  // re-calls it on Refresh.
  "log.getCurrent": { path: string; contents: string };
  // Scene collections (named, switchable scene/source sets, Phase 6a). list
  // enumerates every collection with the active one flagged; create/rename/remove
  // mutate the registry; switch tears down the current scene world and loads the
  // target (rejects while any output is live, refuses removing the last one). All
  // mutations emit collections.changed; switch additionally emits scenes.changed +
  // transitions.changed so the Scenes/Sources/preview resync on their own.
  "collections.list": CollectionInfo[];
  "collections.create": { id: string };
  "collections.duplicate": { id: string; name: string };
  "collections.rename": { id: string; name: string };
  "collections.switch": { active: string };
  "collections.remove": { removed: boolean };
  // Floating dock tear-out (P3a). detach opens a new OS window whose browser
  // loads index.html?window=<id>&dock=<dock>; redock destroys that window. list
  // enumerates the live detached windows.
  "window.detach": { windowId: number };
  "window.redock": { redocked: number };
  "window.list": { windows: { windowId: number; dock: string }[] };
  // Toggles the host window's borderless fullscreen; returns the new state.
  "window.toggleFullscreen": { fullscreen: boolean };
  // Custom-title-bar window controls. Each acts on the calling window (windowId 0 =
  // main shell; >0 = a detached window). minimize/close post their message and
  // return immediately; toggleMaximize echoes the resulting state (the authoritative
  // update arrives via the window.stateChanged push).
  "window.minimize": { ok: boolean };
  "window.toggleMaximize": { maximized: boolean };
  "window.close": { ok: boolean };
  // Engine undo stack. state reports can-undo/can-redo + the next action names;
  // undo/redo pop/replay the top entry. All emit undo.changed after mutating.
  "undo.state": UndoState;
  "undo.undo": Record<string, never>;
  "undo.redo": Record<string, never>;
  // OBS Studio importer (read-only, Item 17). scan inventories an external OBS
  // install (omit `path` to auto-detect; found:false -> offer Browse). import
  // creates NEW fork collections / a stream profile / canvas + audio state from the
  // selected items; nothing in the source OBS install is modified.
  "importer.scan": ImporterScan;
  "importer.import": ImporterImportResult;
  // Multichat (creator engagement, Phase 9.0). send posts `text` to the given
  // platforms (empty array = every connected platform) and returns {ok:true}
  // immediately (the per-transport send runs on a worker; a failure surfaces as a
  // chat.state error event). state returns the current per-platform connection
  // status array (empty when nothing is connected / not live). Chat transports are
  // started by the host on go-live and stopped on stop -- there is no connect method.
  "chat.send": { ok: boolean };
  "chat.state": ChatState[];
  // Cross-platform events feed (creator engagement, Phase 9.2). list returns the
  // retained events newest-first; clear empties the host store (the host then
  // emits an empty events.backfill so consumers reset their feed). New events
  // arrive via the events.new / events.backfill push events.
  "events.list": NormalizedEvent[];
  "events.clear": { ok: boolean };
  // Overlay widgets (loopback SSE overlays, Phase 9.3). list/get/create/duplicate/
  // delete/url/test/serverInfo/addToScene are sync; uploadAsset is async. create/
  // update/duplicate/delete emit overlays.changed. test broadcasts a synthetic event
  // to one widget's open SSE streams (never persisted). addToScene creates a Browser
  // Source at the widget URL in the current scene.
  "overlays.list": OverlayListItem[];
  "overlays.get": OverlayWidget;
  "overlays.create": OverlayWidget;
  "overlays.update": { ok: boolean };
  "overlays.duplicate": OverlayWidget;
  "overlays.delete": { removed: string };
  "overlays.url": { url: string };
  "overlays.test": { ok: boolean };
  "overlays.serverInfo": OverlayServerInfo;
  "overlays.uploadAsset": { path: string };
  "overlays.addToScene": { id: number; source: string };
}

/** Known server->client push events and their payload shapes. */
export interface ObsEvents {
  "streaming.changed": { active: boolean };
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
  // General app settings changed (any setGeneral apply); the full state is pushed.
  "settings.generalChanged": GeneralSettings;
  // Advanced app settings changed (any setAdvanced apply); the full state is pushed.
  "settings.advancedChanged": AdvancedSettings;
  "canvas.changed": Record<string, never>;
  "streamProfile.changed": Record<string, never>;
  // Platform OAuth connect flow (Phase 8). connectProgress reports progress for an
  // in-flight connect; `phase` discriminates the strategy ("deviceCode" carries the
  // user code + verification URL the bridge already opened, "browser" is a PKCE
  // loopback with no code, only a status message);
  // status fires whenever any profile's link state changes (re-fetch oauth.status);
  // connectError reports a failed/aborted connect for one profile.
  "oauth.connectProgress": OAuthConnectProgress;
  "oauth.status": OAuthStatus[];
  "oauth.connectError": { profileUuid: string; error: string };
  // A profile's stream metadata changed (8b modal apply / external edit); a consumer
  // re-runs streamMeta.get for that profile.
  "streamMeta.changed": { profileUuid: string };
  "outputBinding.changed": Record<string, never>;
  // A scene link was created/updated/removed; a consumer re-runs sceneLink.list.
  "sceneLink.changed": Record<string, never>;
  "multistream.changed": { outputs: MultistreamStatus[] };
  // The virtual camera started/stopped or its target canvas changed (fires on
  // start, stop, AND setConfig); the UI re-syncs its toggle + target label.
  "virtualCam.changed": VirtualCamStatus;
  // Coalesced per-source audio levels, pushed at most ~30 Hz off the volmeter
  // callbacks. The UI maps dB -> meter width and applies peak hold.
  "audio.levels": { levels: AudioLevel[] };
  // The active audio source set changed (source activated/deactivated); the UI
  // re-runs audio.list to rebuild its rows.
  "audio.changed": Record<string, never>;
  // The active transition type and/or its duration changed; the UI re-runs
  // transitions.getCurrent to refresh its dropdown + duration field.
  "transitions.changed": Record<string, never>;
  // A hotkey binding changed (set/clear, or an external edit); the UI re-runs
  // hotkeys.list to refresh its rows.
  "hotkeys.changed": Record<string, never>;
  // The set of live native projectors changed (opened/closed, incl. user OS-close
  // or Esc). Carries the projector id under `opened` or `closed` depending on which
  // side changed. Projectors are fire-and-forget windows, so nothing subscribes today.
  "projector.changed": { opened?: number; closed?: number };
  // The embedded MCP server's config or listening status changed (enable/disable,
  // port change, token regenerate, or a bind error); the UI re-runs mcp.getConfig.
  "mcp.changed": Record<string, never>;
  // A scene collection was created/renamed/removed or switched (active changed);
  // the menu re-runs collections.list to refresh its list + active checkmark.
  "collections.changed": Record<string, never>;
  // Floating dock tear-out (P3a). Broadcast to ALL browsers (main + detached).
  // opened fires after a detached window's browser exists; closed fires on
  // explicit redock AND on user OS-close (NOT during app shutdown).
  "window.opened": { windowId: number; dock: string };
  "window.closed": { windowId: number; dock: string };
  // The host window was maximized or restored; the custom title bar's maximize glyph
  // toggles. Broadcast to all windows; each filters by `windowId` (main = 0).
  "window.stateChanged": { windowId: number; maximized: boolean };
  // The undo stack changed (a recorded mutation, an undo, or a redo); the mirror
  // re-applies the pushed state to refresh the shortcuts + toolbar buttons.
  "undo.changed": UndoState;
  // A screenshot was saved (program or source); the app-root toast surfaces the
  // path. Fires on every successful capture.
  "screenshot.saved": { path: string };
  // Multichat (Phase 9.0). message = one normalized chat message appended to the
  // merged scrollback. state = a SINGLE platform's connection row (NOT the full
  // array -- the method returns the array; the event reports one platform at a
  // time, so a consumer merges it by `platform`). Chat is only active while live.
  "chat.message": ChatMessage;
  "chat.state": ChatState;
  // Aggregate viewer count (perAccount + total), pushed by the host's viewer
  // poller while live; the Multichat dock / Monitor card / Studio chip render off it.
  "viewers.changed": ViewerCounts;
  // Per-account audience totals (followers/subscribers), pushed by the host's
  // channels.stats poller. Merged with viewers.changed by the store, not carried here.
  "channels.stats": ChannelStats;
  // Cross-platform events feed (Phase 9.2). new = one normalized event appended to
  // the feed. backfill = a batch of events (newest-first) that REPLACES the feed;
  // it is also fired empty after events.clear, so treat it as "set the feed to
  // this array". Events run on the account-connect lifecycle (always-on for
  // connected accounts) -- they are NOT gated on Go Live and can arrive before or
  // after a broadcast.
  "events.new": NormalizedEvent;
  "events.backfill": NormalizedEvent[];
  // A widget was created/updated/duplicated/deleted; the Overlays page re-runs
  // overlays.list (and re-fetches the open widget if it changed elsewhere).
  "overlays.changed": Record<string, never>;
}

// Every payload-typed event key must be a known EV constant (eventNames.ts); this
// fails to compile if an ObsEvents key is renamed or typed for a non-existent event.
// EV may hold more (fire-and-forget events with no TS payload).
type AssertTrue<T extends true> = T;
type _EventNamesInSync = AssertTrue<[keyof ObsEvents] extends [BridgeEvent] ? true : false>;

export interface BridgeError extends Error {
  code?: number;
}

export type Unsubscribe = () => void;

export interface ObsBridge {
  call<K extends keyof ObsMethods>(method: K, params?: unknown): Promise<ObsMethods[K]>;
  call<T = unknown>(method: string, params?: unknown): Promise<T>;
  on<K extends keyof ObsEvents>(event: K, handler: (payload: ObsEvents[K]) => void): Unsubscribe;
  on(event: BridgeEvent, handler: (payload: unknown) => void): Unsubscribe;
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

// --- typed OAuth account helpers (Phase 4 reuse UI) --------------------------
// Thin wrappers over obs.call for the account-entity methods so callers don't
// repeat the method string + params shape. Return types flow from the ObsMethods
// map above.

/** A provider's already-connected accounts (Streams reuse picker source). */
export const oauthAccounts = (providerId: string): Promise<OAuthAccountRow[]> =>
  obs.call("oauth.accounts", { providerId });

/** Link an already-connected account to another profile (no fresh grant). */
export const oauthLinkAccount = (profileUuid: string, accountId: string): Promise<{ ok: true }> =>
  obs.call("oauth.linkAccount", { profileUuid, accountId });

/** Disconnect an account. Resolves to {needsConfirm, profiles} when >1 profile
 * references it (unless `force`); retry with force:true to unlink them all. */
export const oauthDisconnect = (
  accountId: string,
  force = false,
): Promise<{ ok: true } | { needsConfirm: true; profiles: { uuid: string; name: string }[] }> =>
  obs.call("oauth.disconnect", { accountId, force });

// Install the push sink and keep window.obs assigned for parity with the
// vanilla client / any non-module consumer.
window.__obsEmit = emit;
window.obs = obs;
