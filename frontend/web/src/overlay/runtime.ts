// The clean native widget API injected into every served overlay document. Runs
// inside an OBS Browser Source (its own CEF process), NOT the app. Reads the
// host-injected window.__OVERLAY__ = {id, token, port, fields} and streams
// NormalizedEvents over SSE. Compiled to dist/overlay/runtime.js by bun build.

// ChatHub fans ONE payload object to both the overlay server and the bridge, so the
// wire shape here is the bridge's shape. Type-only, so it erases at build time and the
// runtime still bundles standalone (no bridge module pulled in).
import type {
  AudienceKind,
  ChannelStats,
  ChatMessage,
  EventType,
  NormalizedEvent,
  StreamState,
  ViewerCounts,
} from "$lib/api/bridge";
// A value import, so it is bundled into runtime.js rather than erased: relative, because
// the $lib alias is Vite's and this file is built by `bun build` on its own.
import { cssForSlots } from "./textStyle";
import { fmtCount, fmtMoney, fmtTally, isTally } from "../lib/utils/format";

interface OverlayBootstrap {
  id: string;
  token: string;
  port: number;
  fields: Record<string, unknown>;
  /** Every URL this widget could play, taken off its schema's sound fields by the host
   * (AssembleDocument in overlay_server.cpp). Optional because a document assembled by an
   * older build carries no such key -- an absent list means nothing preloads, not that the
   * widget is silent. */
  sounds?: string[];
}

/** A viewer-count cycle as a widget sees it: the host payload verbatim, plus the
 * per-platform sum every widget would otherwise derive for itself. */
export interface ViewerSnapshot extends ViewerCounts {
  /** providerId -> viewers summed over that platform's accounts. A platform none of
   * whose accounts reported is ABSENT rather than present at 0, so a widget cannot put
   * a live zero on stream for a platform that said nothing. */
  perPlatform: Record<string, number>;
}

/** One platform's audience total, grouped from the per-account entries. Never a plain
 * number: a platform can withhold the figure, and a withheld figure must not be able to
 * reach a widget as a 0. */
export interface AudienceGroup {
  /** Summed over this platform's accounts that reported a real figure, or `null` when
   * NONE did. Null and 0 are different answers -- null means "nobody would tell us". */
  count: number | null;
  /** What `count` counts. A property of the platform rather than the channel, so it is
   * the same across a provider's accounts and summing within a provider stays meaningful
   * (summing ACROSS providers would not, which is why no grand total is derived). */
  kind: AudienceKind;
  /** Accounts of this platform present in the payload, i.e. read at least once. */
  accounts: number;
  /** How many of those contributed to `count`. `accounts - counted` were withheld or
   * unknown, so a widget can render "3 of 4 channels" rather than a wrong total. */
  counted: number;
  /** At least one of this platform's accounts has the figure hidden by its owner. With
   * `counted < accounts` and `hidden` false, the rest were simply never read. */
  hidden: boolean;
}

/** A channel-stats cycle as a widget sees it: the host payload verbatim, plus the
 * per-platform grouping every widget would otherwise derive for itself. */
export interface ChannelStatsSnapshot extends ChannelStats {
  /** providerId -> that platform's audience group. A platform with no account in the
   * payload is ABSENT rather than present at 0. */
  perPlatform: Record<string, AudienceGroup>;
}

type LoadCtx = { fields: Record<string, unknown> };
type LoadHandler = (ctx: LoadCtx) => void;
type EventHandler = (e: NormalizedEvent) => void;
type ChatHandler = (m: ChatMessage) => void;
type ViewersHandler = (v: ViewerSnapshot) => void;
type ChannelStatsHandler = (s: ChannelStatsSnapshot) => void;
// No Snapshot type alongside this one: the host already sends per-destination rows carrying
// the platform key, so there is no per-account split for a widget to re-derive.
type StreamHandler = (s: StreamState) => void;

const boot: OverlayBootstrap = (window as unknown as { __OVERLAY__: OverlayBootstrap }).__OVERLAY__ ?? {
  id: "",
  token: "",
  port: 43000,
  fields: {},
};

const loadHandlers: LoadHandler[] = [];
const eventHandlers: EventHandler[] = [];
const chatHandlers: ChatHandler[] = [];
const viewersHandlers: ViewersHandler[] = [];
const channelStatsHandlers: ChannelStatsHandler[] = [];
const streamHandlers: StreamHandler[] = [];

// One <style> for every slot rule, created on first use: a widget whose slots are all
// untouched compiles to nothing and never gets an element. Appended to <head> after the
// widget's own stylesheet, so a slot rule and a template rule of equal specificity resolve
// in the slot's favour. A fork's own higher-specificity CSS (an id selector) still wins,
// which is the fork author's call to make.
let slotStyleEl: HTMLStyleElement | null = null;

// --- Sound -------------------------------------------------------------------------
// One decoded AudioBuffer per URL, reused for every play. The path this replaces built a
// `new Audio(url)` per alert, which refetched, re-demuxed and re-decoded the clip and
// opened a fresh output stream while the alert was already on screen -- audible as a cut
// partway through, a stall, then the remainder.
//
// WebAudio rather than a pool of media elements: an AudioBuffer is decoded PCM already in
// memory, so a play costs a node allocation and nothing else, and a new
// AudioBufferSourceNode per play gives overlapping alerts real polyphony rather than one
// element's single playback cursor. Nothing is serialized and no play is dropped.

interface SoundEntry {
  /** Decoded PCM, once the decode below has resolved. */
  buffer: AudioBuffer | null;
  /** `buffer`'s size in bytes, 0 until it lands. The cache budgets on this, not on a count. */
  bytes: number;
  /** The queued-or-in-flight decode, so a second play during it cannot start a second fetch.
   * Cleared when it settles -- `buffer`, `elementOnly` and `attempts` are the durable
   * answers, and holding a settled promise would pin its closure for the page's life. */
  decode: Promise<void> | null;
  /** This URL will never get a decoded buffer: its PCM was over the per-sound ceiling, or
   * transient failures used up kMaxSoundDecodeAttempts. Every play of it uses the media
   * element instead. One-way, which is what stops anything retrying forever -- so only a
   * genuinely permanent condition may set it. */
  elementOnly: boolean;
  /** Transient decode failures so far: a rejected or timed-out fetch, a non-OK status, or a
   * decode of bytes that arrived damaged. None of those say anything about the NEXT attempt,
   * so they leave the entry retryable until this reaches kMaxSoundDecodeAttempts. Eviction
   * resets it by dropping the entry, which is the right answer: a URL coming back after a
   * long absence is a new question. */
  attempts: number;
}

/** Two independent bounds, because they fail in different ways.
 *
 * `kMaxDecodedSoundBytes` is the memory bound: an asset may be up to the server's 8 MB cap
 * and decodes to far more than that as float32 PCM (48 kHz stereo is 384 KB per second), so
 * counting entries bounds nothing. One 2 s alert clip is about 700 KB decoded, so this holds
 * roughly twenty of them.
 *
 * `kMaxSoundEntries` bounds entry CHURN rather than memory: an entry that never decodes
 * costs almost nothing, but a widget minting sound URLs at runtime could otherwise
 * accumulate them without limit.
 *
 * Insertion order is the LRU order for both: every lookup re-inserts, so eviction always
 * drops the least recently used. */
const kMaxDecodedSoundBytes = 16 * 1024 * 1024;
/** Per sound, checked after decode -- the decoded size cannot be known before. About 21 s of
 * 48 kHz stereo. A clip over it is a stinger rather than an alert sound, and it falls back to
 * the media element, which streams instead of holding the whole PCM resident. */
const kMaxOneDecodedSoundBytes = 8 * 1024 * 1024;
const kMaxSoundEntries = 8;
/** Total attempts a URL gets before it settles into elementOnly for good. Nothing schedules
 * them: a retry rides the next playSound miss, so a sound nobody plays again never spends the
 * rest of its budget, and a sound played every alert gets its retry immediately. */
const kMaxSoundDecodeAttempts = 3;
/** Deadline on one preload's fetch, body included. The chain runs one decode at a time, so a
 * request that never answers would otherwise park every later sound behind it for the life of
 * the page -- nothing else releases it. Cold fetches against the in-process overlay server on
 * loopback measured 9.3-13.9 ms, so this is well over two orders of magnitude of headroom and
 * still fires long before a broadcast could notice. A timeout is a transient failure. */
const kSoundFetchTimeoutMs = 3000;
const soundCache = new Map<string, SoundEntry>();
/** Keyed "<stage>|<url>", so one durable condition reports once per stage instead of once
 * per alert -- an alert box fires hundreds of times a broadcast and this channel is the
 * session log. Bounded for the same reason kMaxSoundEntries is, and against the same
 * adversary: a widget minting sound URLs at runtime would otherwise grow this set without
 * limit and emit a line per URL per stage forever. Evicting a key only risks one repeated
 * line, long after the first. */
const kMaxLoggedSoundNotices = 32;
const loggedSoundNotices = new Set<string>();
let audioCtx: AudioContext | null = null;

/** The token in the query must never reach the log. */
function soundLogPath(url: string): string {
  return url.split("?")[0];
}

function describeSoundError(e: unknown): string {
  return e instanceof Error ? `${e.name}: ${e.message}` : String(e);
}

/** One line per (stage, url). console.error rather than console.log because obs-browser
 * forwards nothing else to the session log -- including the lines that report a policy
 * decision rather than a fault, so `detail` has to carry that distinction in its wording. */
function logSoundOnce(url: string, stage: string, detail: string) {
  const key = stage + "|" + url;
  if (loggedSoundNotices.has(key)) {
    return;
  }
  // Oldest-first; a Set iterates in insertion order, and deleting the key being visited is
  // defined behaviour.
  for (const old of loggedSoundNotices) {
    if (loggedSoundNotices.size < kMaxLoggedSoundNotices) {
      break;
    }
    loggedSoundNotices.delete(old);
  }
  loggedSoundNotices.add(key);
  console.error(`OBSOverlay ${detail} ${soundLogPath(url)}`);
}

/** A suspended context is the external-browser case only: the app owns the CEF process and
 * starts it with `--autoplay-policy=no-user-gesture-required` (frontend/src/app.cpp), which
 * covers browser sources and the editor preview iframe alike -- obs-browser has its own copy of
 * that switch, but it never runs. So in-app the context is running from creation and this is a
 * no-op. In a plain browser tab it stays suspended until a gesture, which the listeners in
 * soundContext() wait for. */
function resumeSoundContext(ctx: AudioContext) {
  if (ctx.state !== "suspended") {
    return;
  }
  // A refused resume leaves the context suspended and the play silent; there is nothing to
  // recover to, since a media element is blocked by the same policy.
  void ctx.resume().catch(() => {});
}

const kSoundResumeEvents: (keyof WindowEventMap)[] = ["pointerdown", "keydown"];

/** Created on first use rather than at load: every widget type loads this runtime and most
 * never play a sound. Null when the platform has no WebAudio at all, which puts every play
 * on the media-element path. */
function soundContext(): AudioContext | null {
  if (audioCtx) {
    return audioCtx;
  }
  const Ctor =
    window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
  if (!Ctor) {
    return null;
  }
  try {
    audioCtx = new Ctor();
  } catch {
    return null;
  }
  const ctx = audioCtx;
  for (const name of kSoundResumeEvents) {
    window.addEventListener(name, () => resumeSoundContext(ctx), { passive: true });
  }
  return ctx;
}

/** float32 PCM, one array per channel. */
function decodedBytes(buffer: AudioBuffer): number {
  return buffer.length * buffer.numberOfChannels * 4;
}

/** Evict least-recently-used entries until both bounds hold. The total is summed fresh each
 * time rather than carried in a counter, over at most a handful of entries, so it cannot
 * drift out of step with the map.
 *
 * Exactly one entry is exempt: the one `decodeSound` is running right now, whose fetch is
 * already paid for and whose buffer is about to land. A merely QUEUED entry is not exempt --
 * it has spent nothing yet, its chain link no-ops once it finds itself evicted, and exempting
 * the whole queue is what would make these bounds unenforceable for as long as the queue
 * takes to drain. */
function enforceSoundBudget() {
  let total = 0;
  for (const e of soundCache.values()) {
    total += e.bytes;
  }
  // Oldest-first, and deleting the key being visited is defined behaviour for a Map.
  for (const [url, e] of soundCache) {
    if (total <= kMaxDecodedSoundBytes && soundCache.size <= kMaxSoundEntries) {
      break;
    }
    if (e === decodingEntry) {
      continue;
    }
    total -= e.bytes;
    soundCache.delete(url);
  }
}

function soundEntry(url: string): SoundEntry {
  const hit = soundCache.get(url);
  if (hit) {
    soundCache.delete(url);
    soundCache.set(url, hit);
    return hit;
  }
  // Before the insert, never after. After, the entry just created is the newest thing in the
  // map and the likeliest candidate the pass would take -- deleting it at birth and leaving
  // the decode that follows with nothing to write into. A widget with more sound fields than
  // kMaxSoundEntries reaches that state from the parse-time preload loop alone.
  //
  // Overshoot after this pass, by budget: kMaxSoundEntries by two -- the one about to be
  // inserted, plus the one currently decoding if it would otherwise have been evicted --
  // and kMaxDecodedSoundBytes by one, since the entry inserted below carries no bytes
  // until its decode lands. Neither is bounded by the queue depth: a queued entry is
  // evictable, which is what keeps the bound enforceable while a long queue drains.
  enforceSoundBudget();
  const entry: SoundEntry = { buffer: null, bytes: 0, decode: null, elementOnly: false, attempts: 0 };
  soundCache.set(url, entry);
  return entry;
}

/** Every decode runs through this chain, one at a time.
 *
 * The per-sound ceiling can only be checked AFTER decodeAudioData has allocated the PCM, so
 * decoding concurrently would let N whole clips exist at once before any of them could be
 * rejected: eight in flight against an 8 MB ceiling is a ~64 MB transient inside a 16 MB
 * budget, in a renderer that is already software-compositing every other overlay. Serializing
 * makes the peak one decode rather than one per declared sound, and the budget then bounds
 * both the retained bytes and the peak.
 *
 * The cost is warm-up order on a fork with several sounds: the second and later ones are
 * ready a few milliseconds apart rather than together, and an alert that beats its own sound
 * to the finish plays through the media element instead. */
let decodeChain: Promise<void> = Promise.resolve();
/** The entry the chain is executing right now, or null between links. The one thing
 * enforceSoundBudget will not evict, and the only reason this is a variable rather than a
 * flag on the entry: at most one can hold it, which is the invariant the chain exists for. */
let decodingEntry: SoundEntry | null = null;

/** Fetch the encoded bytes under a deadline. The signal covers the body read as well as the
 * headers, because a response that stalls mid-body parks the chain exactly as one that never
 * arrives does. decodeAudioData stays outside it deliberately: it is CPU-bound on bytes
 * already in hand and bounded by the server's own upload cap, so it cannot hang the way a
 * socket can. */
async function fetchSoundBytes(url: string): Promise<ArrayBuffer> {
  const controller = new AbortController();
  let timedOut = false;
  const timer = window.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, kSoundFetchTimeoutMs);
  try {
    const res = await fetch(url, { signal: controller.signal });
    if (!res.ok) {
      throw new Error("HTTP " + res.status);
    }
    return await res.arrayBuffer();
  } catch (e: unknown) {
    // An abort surfaces as "AbortError: signal is aborted without reason", which names
    // neither the deadline nor its value -- and the log line is the only evidence anyone gets.
    throw timedOut ? new Error(`no response within ${kSoundFetchTimeoutMs} ms`) : e;
  } finally {
    window.clearTimeout(timer);
  }
}

/** Never rejects: the chain must not break, and every outcome is recorded on `entry`. */
async function decodeSound(url: string, entry: SoundEntry, ctx: AudioContext): Promise<void> {
  // A queued link can find its entry already gone: a budget pass evicts queued entries, and
  // a later miss on the same URL puts a DIFFERENT entry under that key. Decoding into either
  // one would be work nothing can look up, so the link simply drops. `buffer`/`elementOnly`
  // are re-read for the same reason -- a second queue attempt for one entry is guarded
  // against in preloadSound, but this is the cheap end of that invariant.
  if (soundCache.get(url) !== entry || entry.buffer || entry.elementOnly) {
    return;
  }
  decodingEntry = entry;
  try {
    const buffer = await ctx.decodeAudioData(await fetchSoundBytes(url));
    const bytes = decodedBytes(buffer);
    if (bytes > kMaxOneDecodedSoundBytes) {
      // A policy decision, not a fault: caching this would spend half the page's whole PCM
      // budget on one clip, so it plays from a media element, which streams. Still logged,
      // because a sound that quietly stopped being preloaded is worth knowing about.
      entry.elementOnly = true;
      logSoundOnce(
        url,
        "decode",
        `sound not cached: ${bytes} B of decoded PCM is over the ${kMaxOneDecodedSoundBytes} B per-sound ceiling, so it plays from a media element instead --`,
      );
      return;
    }
    entry.buffer = buffer;
    entry.bytes = bytes;
  } catch (e: unknown) {
    // Transient by default. A rejected fetch, a timeout, a non-OK status and a decode of
    // damaged bytes all report the same thing -- this attempt did not work -- and none of
    // them says the next one will not. Leaving the entry retryable is the difference between
    // a dropped connection costing one alert and costing every alert of a broadcast, which is
    // what a one-way latch here would mean. Two stages, so the first failure and the giving
    // up are separate keys and a URL that spends its whole budget says so exactly once.
    entry.attempts += 1;
    if (entry.attempts >= kMaxSoundDecodeAttempts) {
      logSoundOnce(
        url,
        "decode-exhausted",
        `sound decode failed ${entry.attempts} times, so it plays from a media element from now on -- last was ${describeSoundError(e)}:`,
      );
      entry.elementOnly = true;
    } else {
      logSoundOnce(
        url,
        "decode",
        `sound decode failed, attempt ${entry.attempts} of ${kMaxSoundDecodeAttempts} -- a media element covers it and the next play retries -- ${describeSoundError(e)}:`,
      );
    }
  } finally {
    // Released before the pass, so this entry is an ordinary eviction candidate again the
    // moment it stops being the one in flight.
    decodingEntry = null;
    entry.decode = null;
    enforceSoundBudget();
  }
}

/** Queue `url` for decoding if it is not already answered or queued. Idempotent, synchronous,
 * and never throws: a failure marks the URL so it falls back to a media element from then on. */
function preloadSound(url: string): void {
  if (!url) {
    return;
  }
  const entry = soundEntry(url);
  if (entry.buffer || entry.decode || entry.elementOnly) {
    return;
  }
  const ctx = soundContext();
  if (!ctx) {
    // A platform with no WebAudio at all is permanent, but the same null also means
    // `new AudioContext()` threw -- which Chromium does while a document is not fully
    // active, and which the next preload in a live document would not hit. Counting it
    // as an attempt rather than latching keeps `elementOnly` honest about being
    // one-way and reserved for genuinely permanent conditions.
    entry.attempts += 1;
    if (entry.attempts >= kMaxSoundDecodeAttempts) {
      entry.elementOnly = true;
    }
    logSoundOnce(url, "decode", "no AudioContext available; playing from a media element");
    return;
  }
  decodeChain = decodeChain.then(() => decodeSound(url, entry, ctx));
  // Marks the entry as queued-or-in-flight, which is what stops a second play queueing a
  // second link for it. It is deliberately NOT what the budget pass exempts -- that is
  // `decodingEntry`, which is only ever the one link actually running.
  entry.decode = decodeChain;
}

/** The pre-WebAudio path, kept as the fallback for a URL that has no decoded buffer. */
function playSoundElement(url: string, volume: number) {
  const a = new Audio(url);
  a.volume = volume;
  void a.play().catch((e: unknown) => logSoundOnce(url, "play", `playSound failed: ${describeSoundError(e)}`));
}

function playSound(url: string, volume = 1) {
  if (!url) {
    return;
  }
  const gain = Math.max(0, Math.min(1, volume));
  const entry = soundEntry(url);
  const ctx = entry.elementOnly ? null : soundContext();
  if (ctx && entry.buffer) {
    resumeSoundContext(ctx);
    const src = ctx.createBufferSource();
    src.buffer = entry.buffer;
    const vol = ctx.createGain();
    vol.gain.value = gain;
    src.connect(vol).connect(ctx.destination);
    // Source nodes are one-shot; dropping the graph on end is what keeps a long broadcast
    // from accumulating one node pair per alert.
    src.onended = () => {
      src.disconnect();
      vol.disconnect();
    };
    src.start();
    return;
  }
  // No decoded buffer: queue the decode so the NEXT play is instant, and serve this one from
  // a media element rather than going silent. Reached by a URL no schema declared, by one
  // whose decode has not come round yet, and permanently by one that will not decode.
  preloadSound(url);
  playSoundElement(url, gain);
}

// Queue every declared sound at parse time -- before DOM ready, and normally long before the
// first alert. Normally, not always: the decodes run one at a time, and a source reloaded
// mid-broadcast starts this window over, so an alert can still arrive inside it. That alert
// plays through the media element; see playSound.
for (const url of boot.sounds ?? []) {
  preloadSound(String(url));
}

function applyStyles(fields: Record<string, unknown>) {
  const css = cssForSlots(fields);
  if (!css && !slotStyleEl) {
    return;
  }
  if (!slotStyleEl) {
    slotStyleEl = document.createElement("style");
    document.head.appendChild(slotStyleEl);
  }
  slotStyleEl.textContent = css;
}

// What an event's `amount` counts, per type: money in hundredths of `currency`, or a plain
// tally (bits cheered, raiding viewers). A type not listed reads as a plain count, so
// nothing is ever dressed as currency unless it is listed here.
const kMoneyTypes = new Set<EventType>(["superchat", "supersticker"]);

// The host omits a zero amount from the wire, so for a tally (a 0-viewer raid) a missing
// amount is zero. Anything else without one carries no amount at all.
function amountOf(e: NormalizedEvent): number | null {
  if (e.amount != null) {
    return e.amount;
  }
  return isTally(e.type) ? 0 : null;
}

function formatAmount(e: NormalizedEvent): string {
  const n = amountOf(e);
  if (n == null) {
    return "";
  }
  return kMoneyTypes.has(e.type) ? fmtMoney(n, e.currency) : fmtCount(n);
}

function formatAmountText(e: NormalizedEvent): string {
  const n = amountOf(e);
  if (n == null) {
    return "";
  }
  return kMoneyTypes.has(e.type) ? fmtMoney(n, e.currency) : fmtTally(e.type, n);
}

const kTemplateToken = /\{(\w+)\}/g;
// The lookbehind starts a match only at the head of a run of spaces, keeping a long run linear.
const kEmptyTemplateToken = /(?<! ) *\{(\w+)\}/g;

// Fill `{key}` tokens from `values`. A key absent from `values` is not a variable and stays
// verbatim. An empty value (null, undefined or "") leaves along with the spaces before it,
// so an absent value never leaves "sent  !" behind. The rest fill in one pass, so a value
// that itself contains "{name}" is shown as typed, never expanded.
function fillTemplate(text: string, values: Record<string, unknown>): string {
  const valueOf = (key: string): string | null => {
    if (!Object.prototype.hasOwnProperty.call(values, key)) {
      return null;
    }
    const v = values[key];
    return v == null ? "" : String(v);
  };
  return String(text ?? "")
    .replace(kEmptyTemplateToken, (m, key: string) => (valueOf(key) === "" ? "" : m))
    .replace(kTemplateToken, (m, key: string) => valueOf(key) ?? m)
    .trim();
}

const OBSOverlay = {
  fields: boot.fields,
  /** Recompile every slot rule from `fields`. Called with the resolved fields before
   * onLoad runs, so a widget that only declares slots needs no JS of its own. */
  applyStyles,
  /** Checkbox reader with a per-field fallback for when the key is missing entirely (an
   * older widget, or a fork whose author deleted the field). */
  isOn(fields: Record<string, unknown>, key: string, fallback: boolean): boolean {
    const v = fields[key];
    if (v == null) {
      return fallback;
    }
    return v === true || v === "true";
  },
  /** Text reader. An empty string the user typed deliberately is a valid answer, so only
   * a missing key falls back. */
  textField(fields: Record<string, unknown>, key: string, fallback: string): string {
    const v = fields[key];
    return v != null ? String(v) : fallback;
  },
  /** Two-digit clock field. */
  pad(n: number): string {
    return n < 10 ? "0" + n : String(n);
  },
  /** Money given in hundredths of `currency`'s major unit -- the scale every money event
   * carries, zero-decimal currencies included, so 100000 JPY reads "¥1,000". With no code
   * it reads as the bare figure to two decimals; a malformed code is kept after it. */
  formatMoney: fmtMoney,
  /** A whole count (bits, viewers, a running total), grouped per locale: 1000 -> "1,000". */
  formatCount: fmtCount,
  /** An event's `amount` as its type means it: currency for a Super Chat or Sticker, a
   * grouped count for bits or raiders. Empty when the event carries no amount; a tally
   * type with none reads as zero, since the host omits a zero amount. */
  formatAmount,
  /** formatAmount with the unit a tally counts in: "1 viewer", "1,000 bits". Money reads
   * exactly as formatAmount does. Empty when the event carries no amount; a tally type
   * with none reads as zero, since the host omits a zero amount. */
  formatAmountText,
  /** Fill a user-typed template's `{key}` tokens from `values` in one pass. Unknown tokens
   * stay verbatim, empty values drop out with their leading spaces, and substituted text is
   * never re-expanded. Put the result in textContent, never innerHTML. */
  fillTemplate,
  onLoad(fn: LoadHandler) {
    loadHandlers.push(fn);
  },
  onEvent(fn: EventHandler) {
    eventHandlers.push(fn);
  },
  onChat(fn: ChatHandler) {
    chatHandlers.push(fn);
  },
  onViewers(fn: ViewersHandler) {
    viewersHandlers.push(fn);
  },
  onChannelStats(fn: ChannelStatsHandler) {
    channelStatsHandlers.push(fn);
  },
  onStream(fn: StreamHandler) {
    streamHandlers.push(fn);
  },
  /** Play `url` at `volume` (0..1). Overlapping calls mix rather than queue.
   *
   * Plays from memory once the URL has been decoded, which for a sound the host declared is
   * normally well before the first event. It is not guaranteed to be: an alert landing inside
   * the parse-time decode window plays through a media element instead, and that window is
   * genuinely reachable, because a save mid-broadcast re-mints the URL and reloads the source
   * (overlay_sources.cpp). A URL no schema declared is in the same position on its first play.
   * Either way nothing is dropped, and the next play of that URL is instant. */
  playSound,
};

(window as unknown as { OBSOverlay: typeof OBSOverlay }).OBSOverlay = OBSOverlay;

function fireLoad() {
  const ctx: LoadCtx = { fields: boot.fields };
  // Before the handlers, so a widget's own applyFields runs against markup already
  // carrying its slot styling rather than restyling it a frame later.
  applyStyles(boot.fields);
  for (const fn of loadHandlers) {
    try {
      fn(ctx);
    } catch (e) {
      console.log("OBSOverlay onLoad threw: " + (e as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:load", { detail: ctx }));
}

function fireEvent(e: NormalizedEvent) {
  for (const fn of eventHandlers) {
    try {
      fn(e);
    } catch (err) {
      console.log("OBSOverlay onEvent threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:event", { detail: e }));
}

function fireChat(m: ChatMessage) {
  for (const fn of chatHandlers) {
    try {
      fn(m);
    } catch (err) {
      console.log("OBSOverlay onChat threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:chat", { detail: m }));
}

// The per-platform sum lives here rather than in each template.js: the host payload is
// keyed per account, so every widget wanting platform chips would otherwise repeat this
// split and they would drift apart. Same derivation the app's viewerCountStore does for
// its own bundle -- an account key is the host's OAuth::AccountId, "<providerId>:<userId>".
function toSnapshot(counts: ViewerCounts): ViewerSnapshot {
  const perPlatform: Record<string, number> = {};
  for (const [accountId, n] of Object.entries(counts.perAccount ?? {})) {
    const providerId = accountId.split(":")[0];
    perPlatform[providerId] = (perPlatform[providerId] ?? 0) + n;
  }
  return { ...counts, perPlatform };
}

function fireViewers(counts: ViewerCounts) {
  const v = toSnapshot(counts);
  for (const fn of viewersHandlers) {
    try {
      fn(v);
    } catch (err) {
      console.log("OBSOverlay onViewers threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:viewers", { detail: v }));
}

// Grouped per platform, never summed across platforms: a Twitch follower count and a
// YouTube subscriber count measure different things, so their sum names nothing. Within a
// provider the kind is fixed, so that sum is meaningful. An account whose figure is hidden
// or unknown (-1) is excluded from `count` yet still counted in `accounts`, so "withheld"
// stays distinguishable from "never read" and can never collapse into a number. Account
// keys are the host's OAuth::AccountId, "<providerId>:<userId>".
function toChannelSnapshot(stats: ChannelStats): ChannelStatsSnapshot {
  const perPlatform: Record<string, AudienceGroup> = {};
  for (const [accountId, e] of Object.entries(stats.perAccount ?? {})) {
    const providerId = accountId.split(":")[0];
    const g = (perPlatform[providerId] ??= { count: null, kind: "", accounts: 0, counted: 0, hidden: false });
    g.accounts += 1;
    if (e.audienceHidden) g.hidden = true;
    if (e.audienceKind && !g.kind) g.kind = e.audienceKind;
    if (!e.audienceHidden && typeof e.audienceCount === "number" && e.audienceCount >= 0) {
      g.count = (g.count ?? 0) + e.audienceCount;
      g.counted += 1;
    }
  }
  return { ...stats, perPlatform };
}

function fireChannelStats(stats: ChannelStats) {
  const s = toChannelSnapshot(stats);
  for (const fn of channelStatsHandlers) {
    try {
      fn(s);
    } catch (err) {
      console.log("OBSOverlay onChannelStats threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:channelstats", { detail: s }));
}

function fireStream(state: StreamState) {
  for (const fn of streamHandlers) {
    try {
      fn(state);
    } catch (err) {
      console.log("OBSOverlay onStream threw: " + (err as Error).message);
    }
  }
  window.dispatchEvent(new CustomEvent("obs:stream", { detail: state }));
}

// EventSource auto-reconnects on drop; the host keepalive keeps it warm.
const src = new EventSource("/w/" + boot.id + "/events?t=" + boot.token);
src.onmessage = (msg) => {
  try {
    fireEvent(JSON.parse(msg.data) as NormalizedEvent);
  } catch {
    /* ignore a malformed frame */
  }
};
// Chat rides a NAMED SSE event, so it bypasses onmessage entirely -- alert-box
// widgets that never call onChat are unaffected.
src.addEventListener("chat", (msg) => {
  try {
    fireChat(JSON.parse((msg as MessageEvent).data) as ChatMessage);
  } catch {
    /* ignore a malformed frame */
  }
});
// Viewer counts ride their own named event for the same reason. The poller pushes only
// while live and never sends a closing zero, so a widget keeps the last cycle on screen
// until the source reloads -- it must not invent a 0 of its own.
src.addEventListener("viewers", (msg) => {
  try {
    fireViewers(JSON.parse((msg as MessageEvent).data) as ViewerCounts);
  } catch {
    /* ignore a malformed frame */
  }
});
// Audience totals ride their own named event. The poller is always-on rather than
// go-live-gated, so a widget gets ticks off-stream too -- but the cadence is ~15 minutes,
// so the first frame can be that far out from a source reload.
src.addEventListener("channels", (msg) => {
  try {
    fireChannelStats(JSON.parse((msg as MessageEvent).data) as ChannelStats);
  } catch {
    /* ignore a malformed frame */
  }
});
// Broadcast state rides its own named event, pushed at every live transition and replayed
// on connect, so a source added mid-broadcast learns `startedAt` at once rather than at the
// next change. It is also the closing signal the viewer channel lacks: the poller stops with
// the stream without a final zero, so a viewer widget clears on `active` going false instead
// of leaving the last counts up for the rest of the scene.
src.addEventListener("stream", (msg) => {
  try {
    fireStream(JSON.parse((msg as MessageEvent).data) as StreamState);
  } catch {
    /* ignore a malformed frame */
  }
});

// Fire load once the DOM + handlers are ready. Handlers registered synchronously in
// the user JS run before this microtask, so a raf defer is enough.
if (document.readyState === "complete" || document.readyState === "interactive") {
  requestAnimationFrame(fireLoad);
} else {
  window.addEventListener("DOMContentLoaded", () => requestAnimationFrame(fireLoad));
}

export {}; // isolatedModules module marker; --format iife strips this from the built output
