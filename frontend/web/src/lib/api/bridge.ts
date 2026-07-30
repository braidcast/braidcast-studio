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

import type { BridgeEvent } from "$lib/utils/eventNames";

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

/** One selectable choice: `value` is what gets submitted, `label` what is shown
 * verbatim. Shared by every descriptor that carries choices (OAuthProviderField
 * enum/labelset options, OverlayField dropdown options). */
export interface LabeledOption {
  value: string;
  label: string;
}

/** A scene as reported by scenes.list. */
export interface SceneInfo {
  name: string;
  current: boolean;
}

/** A scene item's show/hide transition (type + duration), as reported by
 * sceneItems.list and set via sceneItems.setShowTransition/setHideTransition.
 * `type` is a registered transition-type id (see transitionTypes.list). */
export interface ItemTransition {
  type: string;
  duration: number;
}

/** Scale-filter / blending vocabularies shared by the sceneItems.list fields and
 * the matching sceneItems.setScaleFilter `filter`, setBlendingMode `mode` and
 * setBlendingMethod `method` params — the host maps each token to its libobs enum
 * and back through one table per vocabulary, so these are the whole set. */
export type ScaleFilter = "disable" | "point" | "bilinear" | "bicubic" | "lanczos" | "area";
export type BlendMode = "normal" | "additive" | "subtract" | "screen" | "multiply" | "lighten" | "darken";
export type BlendMethod = "default" | "srgbOff";

/** A scene item (source within a scene) as reported by sceneItems.list. */
export interface SceneItem {
  id: number;
  source: string | null;
  visible: boolean;
  locked: boolean;
  scaleFilter: ScaleFilter;
  blendMode: BlendMode;
  blendMethod: BlendMethod;
  interactive?: boolean;
  // Per-item color tag (hex like "#RRGGBB"; "" when unset).
  color: string;
  // Per-item show/hide transitions (type + duration), or null when unset
  // (libobs falls back to a hard cut / the default 300ms when unset).
  showTransition: ItemTransition | null;
  hideTransition: ItemTransition | null;
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
export type PropertyKind = "source" | "encoder" | "service" | "output" | "filter" | "transition";

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
  filter: string | null;
  default_path: string | null;
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
 * and echoes the full post-apply state. `processPriority` is one of auto/normal/aboveNormal/
 * high (auto = High while any output is live, Above Normal when idle); `bindIP` is
 * "default" or an IP string; the rest are booleans except the three
 * uints (streamDelaySec, reconnectRetryDelaySec, reconnectMaxRetries). */
export interface AdvancedSettings {
  processPriority: string;
  disableAudioDucking: boolean;
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
  color?: Partial<CanvasColor>;
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
  color?: Partial<CanvasColor>;
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
  /** Id of the claimed target, "" when unclaimed. What the picker marks as current. */
  targetId: string;
  /** The target this destination claims -- a Facebook Page today. Empty when the
   * provider has no targets (Twitch/Kick/YouTube: the account IS the destination) or
   * when nothing has been claimed yet. Prefer it over the account's name: several
   * profiles share one account, and only this distinguishes them. */
  targetName: string;
  /** The target's own picture, empty when the platform has none or the claim predates
   * avatars being carried. Falls back to the account avatar, never to a blank. */
  targetAvatarUrl: string;
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

/** One field a provider exposes in its capability descriptor. `tier`/`scope`
 * drive how the 8b stream-info modal groups/persists the field; 8a only reads the
 * provider id/displayName, so the rest is carried loosely. */
export interface OAuthProviderField {
  key: string;
  label: string;
  type: string;
  tier?: string;
  /** How far one value for this field may travel — the reach of its VALUE SPACE, which
   * is the provider's to declare:
   * - `"all"` — one value across every provider. For a field whose values are free text
   *   any platform accepts (a title, a description).
   * - `"provider"` — one value shared by every channel of THIS provider, never crossing
   *   into another's. For a field whose valid values are the provider's own: a category
   *   id namespace, or a tag vocabulary one platform validates and the next does not.
   * - `"channel"` — no layer below the channel; each channel holds its own.
   *
   * Absent = `"channel"`, the only default that cannot leak a value to a provider whose
   * rules forbid it. Orthogonal to `perDestination` below, which is about ADDRESSING. */
  scope?: "all" | "provider" | "channel";
  /** enum/labelset choices. */
  options?: LabeledOption[];
  /** Placeholder for the field's text control ("Search category…" when unset). */
  placeholder?: string;
  /** `category` lookups only: the choices are a short list to pick from rather than a
   * catalog to search, so run the lookup on focus with an empty query. Providers whose
   * lookup rejects an empty query (Twitch/Kick category search) leave it unset. */
  browsable?: boolean;
  /** The field addresses WHERE a stream posts rather than describing what it says
   * (Facebook's Page: one account administers several, each with its own RTMPS
   * endpoint). Such a value belongs to the individual stream, never to the account the
   * streams share, so it is edited, pushed and remembered per stream and never
   * inherits from a layer below. Not set = an ordinary channel-level field. */
  perDestination?: boolean;
  /** Soft max length (text/tags) carried for hint/validation; advisory only. */
  max?: number;
  /** Provider-supplied default. Prefill seeds it only where saved + live left the
   * field empty, so a remembered value always wins over it. */
  default?: unknown;
  /** The field has no valid empty state — the provider will substitute something for
   * an absent value, so an unset control would show one thing and send another. An
   * `enum` marked this way offers no empty option and resolves an absent/unrecognized
   * value to `default` (else its first option). Not set = empty is a real state. */
  required?: boolean;
  /** Option value → the consequence of having that value selected, rendered under the
   * control. For choices whose cost is invisible until it bites (YouTube's privacy
   * setting decides whether live chat is free or quota-billed), so the warning is at
   * the point of choice rather than discovered mid-broadcast. */
  optionNotes?: Record<string, string>;
}

/** A streaming platform that supports account connection (oauth.providers). The
 * list is empty in a build without the platform's client id, so callers must treat
 * an absent provider as "stream-key only". */
export interface OAuthProvider {
  id: string;
  displayName: string;
  auth: { strategy: string; scopes: string[]; needsSecret: boolean };
  fields: OAuthProviderField[];
  /** The key under which a stream's bag carries WHICH destination it posts to, when this
   * provider addresses one at all (absent otherwise). Separate from a `perDestination`
   * field because a provider can address a destination without rendering a control for it:
   * Facebook's Page is claimed on the destination itself, so no field exists, yet the key
   * still rides in every stream's remembered bag. Anything deciding what a stream keeps
   * when it is NOT diverging from its channel must treat this key as the address. */
  targetFieldKey?: string;
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
  avatarUrl: string;
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
 * streamMeta.get. Every field is per-provider: Twitch reports title/category/
 * language, Kick reports title/category/tags, and YouTube (create-per-go-live)
 * reports an empty object because there is no current broadcast to read. Treat a
 * missing field as "no live value", not as an empty one. `category` is a nested
 * {id,name} (id "" when unset); `language` is the broadcaster language code. */
export interface StreamMeta {
  title?: string;
  category?: { id: string; name: string };
  language?: string;
  tags?: string[];
}

/** One category/game match (streamMeta.searchCategories). `boxArt` is the box-art
 * image URL (may contain {width}/{height} placeholders); YouTube's category list
 * carries no artwork, so it is absent there. */
export interface StreamCategory {
  id: string;
  name: string;
  boxArt?: string;
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
export type MultistreamState = "idle" | "connecting" | "live" | "error" | "reconnecting";

/**
 * One status row reported by multistream.status / pushed on multistream.changed
 * (one per enabled binding). `lastError` is set in the "error" and
 * "reconnecting" states (the drop reason while the engine retries).
 */
export interface MultistreamStatus {
  bindingUuid: string;
  canvasUuid: string;
  profileLabel: string;
  canvasName: string;
  state: MultistreamState;
  lastError: string;
}

// --- transport health (chat/events/overlay connection surface, R14/G1) -------

/** Connection lifecycle of one chat/events/overlay transport. Lowercase mirror of
 * the native Transports::TransportHealth::State (StateName). */
export type TransportHealthState = "connecting" | "connected" | "reconnecting" | "failed" | "disconnected";

/**
 * One transport health row reported by transports.health / pushed on
 * transports.healthChanged. `id` is a stable transport key ("chat:twitch",
 * "events:kick", "overlay"). `lastError` carries the drop/failure reason in the
 * "reconnecting"/"failed" states. `updatedAt` is epoch ms of the last transition.
 */
export interface TransportHealth {
  id: string;
  state: TransportHealthState;
  lastError: string;
  updatedAt: number;
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
  /** Hidden from the mixer (audio.setHidden); rows filter these out by default. */
  hidden: boolean;
  /** Fader locked (audio.setVolumeLocked); setDeflection no-ops while true. */
  volumeLocked: boolean;
  /** Pinned to the top of the mixer (audio.setPinned); audio.list sorts pinned first. */
  pinned: boolean;
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
 * for a per-canvas item. Mirrors the shape used by sceneItems.setVisible/setLocked.
 *
 * `scene` accepts null as well as omission because the preview hit payload and the
 * dock scene stores carry "no scene" as null; the host reads it through
 * JsonUtil::Str, which folds null, absent, and "" into the same empty name. */
export interface TransformTarget {
  canvas?: string;
  scene?: string | null;
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

/** Per-output live stats row reported in stats.get's `outputs`. `state` is the
 * same lowercase state name the multistream status events report. */
export interface OutputStat {
  bindingUuid: string;
  profileLabel: string;
  canvasName: string;
  state: MultistreamState;
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
 * name color (hex "#RRGGBB"; "" when unset -> fall back to a platform color).
 *
 * `id` is the sender's stable per-user id on `platform` (Twitch's `user-id` tag,
 * YouTube's author channel id, Kick's sender id) -- the key to tally a chatter on,
 * since `name` is a display name the user can change mid-stream and two people on
 * one platform may hold names differing only in case. Absent when the platform did
 * not supply one (the host OMITS the key rather than sending ""), so a consumer must
 * fall back to the name for those and must never treat a missing id as a shared
 * empty-string identity. Do not log it: it identifies a real viewer. */
export interface ChatAuthor {
  name: string;
  id?: string;
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
 * channel it arrived on.
 *
 * `accountId` ("providerId:userId") names which connected account received it --
 * `platform` alone cannot distinguish two accounts on the same platform.
 * `profileUuid` is present only on a platform that runs one chat per broadcast
 * (YouTube creates a broadcast per stream profile, so two orientations on one channel
 * are two separate chats); it is absent for one-chat-per-channel platforms. */
export interface ChatMessage {
  platform: ChatPlatform;
  accountId: string;
  profileUuid?: string;
  channelId: string;
  id: string;
  ts: number;
  author: ChatAuthor;
  fragments: ChatFragment[];
}

/** Per-transport chat connection state. The `chat.state` METHOD returns the full
 * array (one row per active transport); the `chat.state` EVENT carries a SINGLE
 * transport's row. An empty method result means nothing is connected (not live).
 *
 * Merge by (`accountId`, `profileUuid`), not by `platform`: one platform can have
 * several live transports (two accounts, or two broadcasts on one account), and
 * merging those on `platform` alone collapses them into one row. See ChatMessage for
 * when `profileUuid` is present. */
export interface ChatState {
  platform: ChatPlatform;
  accountId: string;
  profileUuid?: string;
  connected: boolean;
  error?: string;
}

/** Params for `chat.send`. Two ways to address a send, and `accountId` wins:
 *
 * - With `accountId`, the text goes to EXACTLY ONE destination -- that account's
 *   broadcast under `profileUuid` -- and `platforms` is ignored entirely. Pass back
 *   the same `accountId` / `profileUuid` pair the ChatMessage or ChatState row you
 *   are replying to carried, so an absent/empty/null `profileUuid` means the
 *   account-wide destination (Twitch and Kick have one chat per account and carry
 *   no profile). Use this for any reply: routing by platform reaches every live
 *   chat on that platform, so a streamer running two YouTube broadcasts cannot
 *   tell which one they answered.
 * - Without `accountId`, the text is broadcast to every active transport whose
 *   platform is in `platforms` (an empty or omitted array = every connected
 *   platform). This is the "say it everywhere" path, not a reply path.
 *
 * The two halves are passed separately rather than as a joined destination key:
 * the host constructs its DestinationId from them directly and never parses the
 * key. `destinationKey()` (see api/destinationKeys.ts) stays a lookup/join id. */
export interface ChatSendParams {
  text: string;
  platforms?: string[];
  accountId?: string;
  profileUuid?: string | null;
}

/** One live destination's concurrent viewers: an account streaming under one stream
 * profile. `key` is the host's stable identifier for the pair ("accountId@profileUuid",
 * or just the accountId when the platform has a single channel per account) and is
 * safe to use as a list key. `profileUuid` is "" for such account-wide rows. */
export interface ViewerDestination {
  key: string;
  accountId: string;
  profileUuid: string;
  count: number;
}

/** Aggregate viewer count (the `viewers.changed` event). `perAccount` maps an
 * accountId ("providerId:userId") to that account's current concurrent viewers, summed
 * over every broadcast it has live; `total` is the sum across accounts. A per-platform
 * breakdown is derived by summing the entries whose accountId prefix matches a
 * providerId (two accounts on one platform add up).
 *
 * `perDestination` is the same figures broken out per broadcast, for a consumer that
 * wants to label each one. It is additive detail -- `total` and `perAccount` remain
 * authoritative and need no knowledge of destinations. */
export interface ViewerCounts {
  perAccount: Record<string, number>;
  total: number;
  perDestination?: ViewerDestination[];
}

// `channels.stats` event: audience totals per account. audienceCount === -1 means
// unknown/hidden; audienceKind labels it ("followers" | "subscribers"); viewers is
// merged in from viewers.changed by the store, not carried here.
/** What an account's audience total counts; "" when the platform reports neither. */
export type AudienceKind = "followers" | "subscribers" | "";

export interface ChannelStatEntry {
  audienceCount: number;
  audienceKind: AudienceKind;
  audienceHidden: boolean;
  audienceUpdatedNs: number;
}

/** One destination's audience total: an account under one stream profile. `key` is the
 * host's stable identifier for the pair ("accountId@profileUuid", or just the accountId
 * when the account IS its single destination) and is safe to use as a list key.
 * `profileUuid` is "" for such account-wide rows. Same shape and same keying as
 * ViewerDestination, so a consumer joins the two by `key`. */
export interface ChannelStatDestination extends ChannelStatEntry {
  key: string;
  accountId: string;
  profileUuid: string;
}

export interface ChannelStats {
  perAccount: Record<string, ChannelStatEntry>;
  /** The same totals broken out per destination, for a consumer that labels each place an
   * account streams to (a Facebook Page each). Additive detail -- `perAccount` remains
   * authoritative, carrying the account's rollup, and needs no knowledge of destinations. */
  perDestination?: ChannelStatDestination[];
}

/** One destination the current broadcast is going out to. Only outputs in an ACTIVE state
 * are listed (connecting / live / reconnecting); an idle or errored binding is absent
 * rather than present as "not live". */
export interface StreamDestination {
  bindingUuid: string;
  /** platformColors.ts key ("youtube" | "twitch" | "kick"); a profile with no linked
   * account yields whatever its service word normalizes to ("rtmp", "whip", "custom"),
   * which has no brand mark and renders as the neutral one. */
  platform: string;
  /** The profile's raw label -- NOT the host's DisplayName(), which prefixes the platform
   * and would double up with a mark. `null` when the profile carries no label at all, so
   * a consumer prints the platform alone rather than an empty line. */
  name: string | null;
  canvasName: string;
  /** MultistreamEngine::StateName, narrowed to the active ones by the filter above. */
  state: "connecting" | "live" | "reconnecting";
  /** Epoch ms this destination's output signaled start, or `null` while it is still
   * connecting -- a start time it does not have yet, never 0. */
  startedAt: number | null;
}

/** Broadcast state: the `streaming.changed` event, and the overlay server's named `stream`
 * SSE channel, which carry the identical object. */
export interface StreamState {
  /** Whether ANY output is active. Independent of `destinations` being non-empty: an
   * output live under a binding disabled mid-broadcast counts here yet enumerates
   * nowhere, and "live to nothing we can name" is not "not live". */
  active: boolean;
  /** Wall-clock epoch ms the FIRST destination went live -- the uptime anchor, computed
   * for `Date.now() - startedAt`. `null` until an output actually signals start (at
   * go-live everything is still connecting), never 0: a zero epoch renders as decades. */
  startedAt: number | null;
  destinations: StreamDestination[];
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
 * platform color when absent; `message` is rendered as plain text, never HTML.
 *
 * `accountId` names the account the event belongs to (absent only for a host-synthesized
 * event). `profileUuid` appears only when the source knew which broadcast it arrived on
 * -- YouTube's live-chat sink does; channel-wide REST reads cannot. */
export interface NormalizedEvent {
  id: string;
  platform: ChatPlatform;
  accountId?: string;
  profileUuid?: string;
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
  options?: LabeledOption[];
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
  // Reorder a scene within the persisted scene_order (Default canvas only; an
  // additional canvas's scene list is not yet reorderable). Either
  // {name, direction} (relative) or {name, to} (absolute, top-first UI index
  // matching scenes.list order, for drag-and-drop).
  "scenes.reorder": { name: string; direction: ReorderDirection | ""; to: number | null };
  // Scene items (top-first draw order; omit `scene` to target the current scene).
  // Pass an optional `canvas` uuid to target an additional canvas's current scene.
  "sceneItems.list": SceneItem[];
  "sceneItems.setVisible": { id: number; visible: boolean };
  "sceneItems.setLocked": { id: number; locked: boolean };
  "sceneItems.setScaleFilter": Record<string, never>;
  "sceneItems.setBlendingMode": Record<string, never>;
  "sceneItems.setBlendingMethod": Record<string, never>;
  // Set a scene item's show/hide transition ({ scene, id, canvas?, transition:
  // <registered type id> | null, duration: <ms> }; null/"" clears it). Emits
  // sceneItems.changed.
  "sceneItems.setShowTransition": Record<string, never>;
  "sceneItems.setHideTransition": Record<string, never>;
  // Set a per-item color tag ({ scene, id, canvas?, color }; color "" clears it).
  // Emits sceneItems.changed.
  "sceneItems.setColor": { ok: true };
  "sceneItems.remove": { removed: number };
  "sceneItems.reorder": { id: number; direction: ReorderDirection };
  // Group selected items into a new group / dissolve a group (neither is undoable yet).
  "sceneItems.group": { id: number; source: string };
  // Create a new empty group in the target scene ({ scene?, canvas?, name? }).
  "sceneItems.createGroup": { id: number; source: string };
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
  // Rename a source addressed by uuid/name, not a scene-item id ({ uuid?|source?,
  // name }) — for the audio mixer, whose rows have no scene-item locator. Echoes
  // the new name; emits audio.changed.
  "sources.renameByName": { source: string };
  "sources.listExisting": ExistingSource[];
  "sources.addExisting": { id: number; source: string };
  // Existing-source picker thumbnail: renders the named source standalone at a
  // capped size and returns it inlined as a PNG data URI. Rejects (caller falls
  // back to a type icon) when the source has no video.
  "sources.thumbnail": { dataUri: string };
  // Duplicate the source of a scene item in place (undo-recorded).
  "sources.duplicate": { id: number; source: string };
  // Duplicate a source (by uuid/name) into a TARGET scene ({ uuid?|source?, scene,
  // canvas?, name? }) — for cross-scene paste-duplicate (undo-recorded).
  "sources.duplicateInto": { id: number; source: string };
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
  "properties.defaults": PropertiesResult;
  "properties.button": PropertiesResult;
  // Native OS file dialog (path / editable_list Browse). `mode` picks an open,
  // save, or directory chooser; `filter` is an OBS-style filter string. Returns
  // { path: null } when the user cancels.
  "dialog.openFile": { path: string | null; size?: number };
  // Reveal a path in the system file manager (file highlighted in its folder;
  // directory opened directly).
  "shell.revealPath": { ok: boolean };
  // Read a local file ({path}) and return it as a base64 data: URI. CEF's app://
  // origin refuses to load file:// resources, so a local-image preview must be
  // inlined. Caps at 10 MB and rejects missing/unreadable files (throws to JS).
  "file.readDataUri": { dataUri: string; size: number };
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
  // Reverts a settings.snapshot blob (partial-application: earlier sections can
  // commit before a later one fails, so `failed` lists every section that didn't
  // apply rather than an all-or-nothing result). No current caller -- the
  // Settings page's OK/Apply/Cancel footer this backed was dropped for the
  // Phase 7 live-apply page model (see braidcast-notes/roadmap.md); kept typed for the
  // C++ round-trip self-test and any future revert UI.
  "settings.restore": { ok: boolean; failed?: { section: string; error: string }[] };
  // Canvases (native multistream encode targets, 4.4.1).
  "canvas.list": CanvasInfo[];
  "canvas.create": { uuid: string };
  "canvas.update": CanvasInfo;
  "canvas.remove": { removed: string };
  // Persisted drag order (echoes applied uuids).
  "canvas.reorder": { order: string[] };
  "encoderTypes.list": EncoderType[];
  // Source filters. filterTypes.list enumerates creatable filter types (optionally
  // narrowed by kind); filters.list returns one source's chain in draw order. add/
  // remove/setEnabled/reorder/rename/duplicate mutate the chain; the selected filter's
  // obs_properties are edited via PropertyForm kind="filter" (ref = its uuid).
  "filterTypes.list": FilterType[];
  "filters.list": FilterInfo[];
  "filters.add": { name: string; uuid: string };
  "filters.remove": { removed: string };
  "filters.setEnabled": { name: string; enabled: boolean };
  "filters.reorder": { name: string; direction: ReorderDirection };
  "filters.rename": { name: string };
  "filters.duplicate": { name: string; uuid: string };
  // Copy/paste a whole filter chain between sources (paste is not yet undoable).
  "filters.copyChain": { filters: CopiedFilter[] };
  "filters.pasteChain": { pasted: number };
  // Stream profiles (reusable destination credentials, 4.4.2).
  "streamProfile.list": StreamProfileInfo[];
  "streamProfile.create": { uuid: string };
  "streamProfile.update": StreamProfileInfo;
  "streamProfile.remove": { removed: string };
  "streamProfile.setPrimary": { uuid: string; isPrimary: boolean };
  // Persisted drag order (echoes applied uuids).
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
  // Every target one account can stream to (Facebook's Pages). A blocking platform read,
  // so it runs on demand rather than being held and kept fresh. Includes targets other
  // destinations already claim -- the picker has to show a taken one as taken.
  "oauth.targets": { fieldKey: string; targets: StreamTargetRow[] };
  // Point one destination at one target. Refused when another destination of the same
  // account already claims it: two profiles on one Page would create two live videos
  // there and contend for a single ingest.
  "oauth.setTarget": { ok: true };
  "oauth.linkAccount": { ok: true };
  "oauth.disconnect": { ok: true } | { needsConfirm: true; profiles: { uuid: string; name: string }[] };
  "oauth.status": OAuthStatus[];
  // Stream metadata (Go Live "Stream Information": title / category / language).
  // get ({accountId}) loads the account's current metadata ({title, category:{id,name},
  // language}); searchCategories ({providerId, query, accountId?}) resolves a query to
  // lookup matches ({id, name, boxArt}) — pass accountId whenever the caller has one,
  // since a provider whose lookup is account-scoped (Facebook's Pages) answers
  // differently per account; set ({accountId, profileUuid, fields, goingLive?}) persists and returns
  // {ok:true}, emitting streamMeta.changed. accountId keys the token/provider; profileUuid
  // is only forwarded into the write's UI-thread-marshalled ingest writeback. goingLive
  // (default false) is true only when the push immediately precedes streaming.start, so a
  // create-per-go-live provider (YouTube) skips creating a broadcast for a standalone edit.
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
  // Transport health snapshot (chat/events/overlay connection state, R14/G1). One row
  // per reporting transport; pushed on transports.healthChanged when any row changes.
  "transports.health": { transports: TransportHealth[] };
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
  "audio.setDeflection": { uuid: string; deflection: number; volumeDb: number; locked?: boolean };
  "audio.setMuted": { uuid: string; muted: boolean };
  // Per-source mixer state. setHidden/setPinned toggle the row's hidden/pinned flag;
  // unhideAll clears hidden on every source (returns the count cleared); setVolumeLocked
  // locks the fader so audio.setDeflection no-ops. All emit audio.changed.
  "audio.setHidden": { uuid: string; hidden: boolean };
  "audio.unhideAll": { cleared: number };
  "audio.setVolumeLocked": { uuid: string; locked: boolean };
  "audio.setPinned": { uuid: string; pinned: boolean };
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
  // Rebase the "since reset" counters (render lag, encode skip, per-output drop) to
  // now, like OBS's Stats Reset. Instantaneous readings (cpu/fps/bitrate) are unaffected.
  "stats.reset": { ok: boolean };
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
  // Multichat (creator engagement, Phase 9.0). send takes ChatSendParams: with
  // `accountId` it posts to EXACTLY ONE destination (`platforms` ignored); without it
  // it posts to the given platforms (empty array = every connected platform). Either
  // way it returns {ok:true} immediately -- the per-transport send runs on a worker and
  // a send failure surfaces later as a chat.state error event. The one synchronous
  // failure is a targeted send whose destination has no live transport: the call
  // REJECTS rather than falling back to the platform or silently dropping, so a reply
  // never lands in the wrong chat. state returns the current per-transport connection
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
  // Gated DEBUG logging (Phase 11 Part 1). get seeds the diagnostics store with the
  // live gate + the current session-log path (SessionLog::CurrentPath); setDebug
  // persists + flips the live gate + emits debug.changed, echoing the applied state;
  // openLogFolder reveals the logs/ directory in the OS file manager.
  "diagnostics.get": { debug: boolean; logPath: string };
  "diagnostics.setDebug": { debug: boolean };
  "diagnostics.openLogFolder": { ok: boolean };
}

/** Known server->client push events and their payload shapes. */
export interface ObsEvents {
  "streaming.changed": StreamState;
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
  // A transport's connection health changed (chat/events/overlay); carries the full
  // current row set so a consumer replaces its state wholesale.
  "transports.healthChanged": { transports: TransportHealth[] };
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
  // The DEBUG gate flipped (Settings toggle or any setDebug caller); the diagnostics
  // store updates `debug` from the payload without a re-fetch.
  "debug.changed": { debug: boolean };
}

/** The scene item a preview right-click landed on, as much of it as the event
 * carries: the hit payload has no scale/blend/color/transition detail, so a menu
 * builder re-fetches those from sceneItems.list. Derived from the event rather
 * than respelled, so a change to the payload reaches every dock that hosts a
 * preview surface. */
export type PreviewHitTarget = Pick<
  ObsEvents["preview.contextMenu"],
  "scene" | "id" | "source" | "visible" | "locked"
>;

// Every payload-typed event key must be a known EV constant (eventNames.ts); this
// fails to compile if an ObsEvents key is renamed or typed for a non-existent event.
// EV may hold more (fire-and-forget events with no TS payload).
type AssertTrue<T extends true> = T;
type _EventNamesInSync = AssertTrue<[keyof ObsEvents] extends [BridgeEvent] ? true : false>;

export interface BridgeError extends Error {
  code?: number;
  // Streamer-facing reason decoded from the host's error envelope (op_error.hpp);
  // absent when the failure carries only a diagnostic. `message` always holds the
  // full diagnostic chain, so existing consumers are unaffected.
  userMessage?: string;
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
        // The host may pack {diagnostic, user message} into a JSON envelope (see
        // frontend/src/util/op_error.hpp). The decode REQUIRES the __err
        // discriminator, so a plain error that merely starts with "{" is never
        // misparsed and keeps today's behavior byte-identically.
        let diagnostic = message;
        let userMessage: string | undefined;
        if (message && message.startsWith("{")) {
          try {
            const env = JSON.parse(message) as { __err?: unknown; d?: unknown; u?: unknown };
            if (env && env.__err === 1 && typeof env.d === "string") {
              diagnostic = env.d;
              if (typeof env.u === "string" && env.u !== "") {
                userMessage = env.u;
              }
            }
          } catch {
            // Not JSON at all: the whole string is the diagnostic.
          }
        }
        const err: BridgeError = new Error(diagnostic || "bridge error " + code);
        err.code = code;
        if (userMessage !== undefined) {
          err.userMessage = userMessage;
        }
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

/** One place an account can stream to. `avatarUrl` is "" where the platform has none. */
export interface StreamTargetRow {
  id: string;
  name: string;
  avatarUrl: string;
}

/** The targets an account can stream to; empty for a provider that has none. */
export const oauthTargets = (accountId: string): Promise<{ fieldKey: string; targets: StreamTargetRow[] }> =>
  obs.call("oauth.targets", { accountId });

/** Point a destination at a target. Name and avatar are cached with the claim so the row
 * renders without a platform call, and come from oauthTargets rather than being re-read. */
export const oauthSetTarget = (profileUuid: string, target: StreamTargetRow): Promise<{ ok: true }> =>
  obs.call("oauth.setTarget", {
    profileUuid,
    targetId: target.id,
    name: target.name,
    avatarUrl: target.avatarUrl,
  });

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
