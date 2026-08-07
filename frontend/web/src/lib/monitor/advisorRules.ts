// The Config Advisor rule registry: one table entry per insight, so adding rule
// N+1 is a single list entry and never a new branch in the sweep, the store, or the
// panel. A rule declares what it needs (`needsSettings`), how to spot it (`detect`
// over the one snapshot), how loud it is (`severity`), and where the user goes to
// deal with it (`link`). Rules explain and deep-link — nothing here writes.
//
// Same data-map shape as EV (utils/eventNames.ts) and Cat (utils/logCategories.ts).

import { openLogViewer } from "$lib/dialogs/logViewerOpener.svelte";
import { setPage } from "$lib/stores/pageStore.svelte";
import { SOURCE_TYPE, type AdvisorSnapshot } from "$lib/monitor/advisorScan";
import { Cat } from "$lib/utils/logCategories";
import { log } from "$lib/utils/log";
import { METER_RED, METER_YELLOW } from "$lib/utils/statsMeter";

/** The roadmap's three bands: something is erroring now, something will bite when
 * you go live, something costs resources for nothing. */
export type AdvisorSeverity = "breakage" | "risk" | "waste";

/** Band -> badge label, meter token, sort rank. The color layer is the existing
 * meter vocabulary (statsMeter METER_*), not a fourth palette — transport health
 * already set that precedent by remapping onto these tokens (theme/stateColors.ts).
 * The palette has three stops and there are three bands, but only breakage is a
 * red-grade "it is broken right now"; risk and waste are both warnings and share
 * the yellow token, with the badge label carrying the distinction. */
export const SEVERITY: Record<AdvisorSeverity, { label: string; color: string; rank: number }> = {
  breakage: { label: "BREAKAGE", color: METER_RED, rank: 0 },
  risk: { label: "RISK", color: METER_YELLOW, rank: 1 },
  waste: { label: "WASTE", color: METER_YELLOW, rank: 2 },
};

/** Bands in display order. `rank` is the one ordering — the row sort and the
 * summary strip both read it, so neither can drift from the other. */
export const SEVERITY_ORDER: readonly AdvisorSeverity[] = (Object.keys(SEVERITY) as AdvisorSeverity[]).sort(
  (a, b) => SEVERITY[a].rank - SEVERITY[b].rank,
);

/** One thing a rule found. */
export interface AdvisorFinding {
  /** Unique within its rule; the row key is `${rule.id}/${key}`. */
  key: string;
  /** What the finding is about — usually the source name. */
  title: string;
  /** What it costs, in plain language. The reason the panel exists. */
  cost: string;
  /** Supporting evidence, or where to fix it. Optional. */
  detail?: string;
}

/** Where a finding sends the user. Read-only navigation: this pass ships no Apply
 * and no write path. */
export interface AdvisorLink {
  label: string;
  go: () => void;
}

export interface AdvisorRule {
  /** Stable id; survives wording changes and keys the rendered rows. */
  id: string;
  severity: AdvisorSeverity;
  /** Names the class of problem (the row's hover title). */
  title: string;
  /** Source type ids whose obs settings this rule reads. The sweep issues one
   * `properties.get` per source of a declared type and none for the rest, so a rule
   * that reads no settings costs no extra bridge calls. */
  needsSettings?: readonly string[];
  /** Why this rule cannot be answered from this snapshot, or null when it can.
   *
   * A rule that returns no findings and a rule that could not run are different
   * results, and collapsing them is how a panel comes to report an all-clear it
   * never earned. Declaring the condition here instead of returning `[]` from
   * `detect` keeps the distinction all the way to the UI. */
  skipWhen?: (s: AdvisorSnapshot) => string | null;
  /** Subjects this rule ran but could not read, or null when it covered them all.
   *
   * Distinct from `skipWhen`, and the distinction is the whole point: a rule whose
   * claim is PER-SUBJECT stays valid for every subject it did read, so a hole costs
   * it coverage rather than validity. Excluding the unread subject is correct;
   * excluding it silently is not, because the remaining findings would then read as
   * a complete answer. Declaring the shortfall here routes it to the same
   * presentation as a wholly skipped rule.
   *
   * A rule whose claim needs the whole picture (rule 4's "in no scene") must use
   * `skipWhen` instead — one hole invalidates every claim it could make. */
  partialWhen?: (s: AdvisorSnapshot) => { unread: number; reason: string } | null;
  detect: (s: AdvisorSnapshot) => AdvisorFinding[];
  link?: AdvisorLink;
}

/** Join the gap channels a rule depends on into one clause, or null when every one of
 * them is clear. Deduped, because two channels can carry the same reason. */
function gapReason(...channels: readonly string[][]): string | null {
  const reasons = [...new Set(channels.flat())];
  return reasons.length > 0 ? reasons.join("; ") : null;
}

/** The off-air browser sources the shutdown rule would have judged but has no
 * settings for. ONE definition, shared by the detector's exclusion and the coverage
 * count, so a source can never be dropped from the findings without being counted in
 * the shortfall the panel shows. Membership means "properties.get did not answer" —
 * scanSettings records a value for every source it read, and nothing else writes to
 * that map. */
function unreadOffAirBrowsers(s: AdvisorSnapshot): string[] {
  return [...s.sources]
    .filter(([name, typeId]) => typeId === SOURCE_TYPE.browser && !s.onAir.has(name) && !s.settings.has(name))
    .map(([name]) => name);
}

const OPEN_SESSION_LOG: AdvisorLink = { label: "View log", go: openLogViewer };
const OPEN_STUDIO: AdvisorLink = { label: "Open Studio", go: () => setPage("studio") };

export const ADVISOR_RULES: readonly AdvisorRule[] = [
  {
    id: "browser.pageErrors",
    severity: "breakage",
    title: "Browser source logged page errors",
    link: OPEN_SESSION_LOG,
    // The findings ARE the log, and they are filtered by the source universe — a
    // failed read of either turns "no errors logged" into a claim the sweep never
    // checked. Both feed `logGaps`.
    skipWhen: (s) => gapReason(s.logGaps),
    detect: (s) =>
      [...s.pageErrors].map(([name, messages]) => ({
        key: name,
        title: name,
        cost:
          messages.length === 1
            ? "The page logged an error this session, so it is not rendering what it was built to render."
            : `The page logged ${messages.length} distinct errors this session, so it is not rendering what it was built to render.`,
        detail: messages.join(" · "),
      })),
  },
  {
    id: "browser.shutdownOffAndOffscreen",
    severity: "waste",
    title: "Off-air browser source stays resident",
    needsSettings: [SOURCE_TYPE.browser],
    link: OPEN_STUDIO,
    // Gated on the on-air set alone. NOT on the whole collection — an unreadable
    // scene the user is not showing cannot change what is showing, and gating there
    // would let one multi-scene extra canvas silently kill the rule this feature
    // exists for. NOT on the settings channel either: this claim is per-source, so a
    // settings read that failed for source X says nothing about source Y. That
    // shortfall is coverage, not validity, and it goes through partialWhen below.
    skipWhen: (s) => gapReason(s.onAirGaps),
    partialWhen: (s) => {
      const unread = unreadOffAirBrowsers(s).length;
      const reason = gapReason(s.settingsGaps);
      return unread > 0 && reason !== null ? { unread, reason } : null;
    },
    detect: (s) => {
      // The same set partialWhen reports, so the exclusion below and the number the
      // user is shown cannot disagree.
      const unread = new Set(unreadOffAirBrowsers(s));
      const found: AdvisorFinding[] = [];
      for (const [name, typeId] of s.sources) {
        if (typeId !== SOURCE_TYPE.browser || s.onAir.has(name)) {
          continue;
        }
        // Excluded because its settings never arrived — NOT because it looked fine.
        // These two exits stay separate on purpose: collapsing them is how an unread
        // source becomes indistinguishable from a healthy one.
        if (unread.has(name)) {
          continue;
        }
        // Read successfully, and the checkbox is already on (settings are
        // default-aware, so `shutdown` is present even if never touched).
        if (s.settings.get(name)?.shutdown !== false) {
          continue;
        }
        found.push({
          key: name,
          title: name,
          cost:
            "Not in any scene that is on air, but it holds a CEF renderer process anyway — " +
            "roughly 130 MB each, measured on this project's own collection.",
          detail: 'Ticking "Shutdown source when not visible" in its properties frees it while off air.',
        });
      }
      return found;
    },
  },
  {
    id: "scene.duplicateItems",
    severity: "waste",
    title: "One source placed twice in a scene",
    link: OPEN_STUDIO,
    // No gate, deliberately: this is the one POSITIVE claim in the table. It asserts
    // that two items it can see point at one source, which an unreadable region
    // elsewhere cannot falsify — at worst the rule under-reports for the scene it
    // could not read, and that scene's failure already surfaces through the rules
    // that do gate on it.
    detect: (s) => {
      const found: AdvisorFinding[] = [];
      for (const scene of s.scenes) {
        const counts = new Map<string, number>();
        for (const item of scene.items) {
          if (item.source) {
            counts.set(item.source, (counts.get(item.source) ?? 0) + 1);
          }
        }
        // Two canvases can hold same-named scenes, so name the canvas outside the
        // Default one rather than leave the finding ambiguous.
        const where = scene.canvasUuid === "" ? `"${scene.name}"` : `"${scene.name}" on ${scene.canvasName}`;
        for (const [name, n] of counts) {
          if (n < 2) {
            continue;
          }
          found.push({
            // JSON-encoded rather than delimiter-joined: a scene or source name may
            // contain any character, and an ambiguous join ("A/B" + "C" colliding
            // with "A" + "B/C") produces two rows with one key, which is a fatal
            // each_key_duplicate in the panel's keyed {#each}.
            key: JSON.stringify([scene.canvasUuid, scene.name, name]),
            title: name,
            cost: `${n} scene items in ${where} point at this one source, so it composites ${n} times per frame for one visible result.`,
            detail: "Usually an accidental duplicate — but which item to drop is yours to decide, so nothing here removes one.",
          });
        }
      }
      return found;
    },
  },
  {
    id: "source.unplaced",
    severity: "waste",
    title: "Source is in no scene",
    link: OPEN_STUDIO,
    // "In no scene at all" is a claim about the whole collection, so unlike the
    // off-air rule this one genuinely needs every region readable: a source hidden
    // in any unreadable corner would otherwise read as dead weight.
    skipWhen: (s) => gapReason(s.graphGaps),
    detect: (s) => {
      const found: AdvisorFinding[] = [];
      for (const name of s.sources.keys()) {
        if (s.placed.has(name)) {
          continue;
        }
        found.push({
          key: name,
          title: name,
          cost:
            "Loaded in this collection but placed in no scene. Nothing renders it, and a capture or " +
            "media source still holds its device or decoder open.",
        });
      }
      return found;
    },
  },
];

/** Union of every rule's `needsSettings`, so the sweep fetches source settings for
 * exactly the types the registry reads and no others. */
export const SETTINGS_TYPE_IDS: ReadonlySet<string> = new Set(
  ADVISOR_RULES.flatMap((r) => [...(r.needsSettings ?? [])]),
);

/** One finding, paired with the rule that produced it. */
export interface AdvisorRow {
  id: string;
  rule: AdvisorRule;
  finding: AdvisorFinding;
}

/** A shortfall in the answer, and why. Surfaced in the panel: "no findings" from a
 * rule that ran over everything and "no findings" from one that did not are
 * different answers, and only the first is an all-clear.
 *
 * `partial` is what separates the two shapes of shortfall — the whole rule could not
 * run, or it ran and its findings are real but its coverage was short. One type,
 * because both are the same thing to a reader: a reason this panel is not the
 * complete picture. */
export interface AdvisorSkip {
  rule: AdvisorRule;
  reason: string;
  /** null when the rule did not run at all; otherwise how many of its subjects it
   * could not read while still reporting on the rest. */
  partial: { unread: number } | null;
}

export interface AdvisorResult {
  rows: AdvisorRow[];
  skipped: AdvisorSkip[];
}

/** Run every rule over one snapshot. Rows come back ordered by severity band then
 * registry position (Array.sort is stable, so same-band rules keep their table
 * order); rules that could not run come back separately with their reason.
 *
 * A rule that throws is reported as skipped rather than dropped — silence is the
 * one outcome this function must never produce on its own. */
export function evaluate(snapshot: AdvisorSnapshot): AdvisorResult {
  const rows: AdvisorRow[] = [];
  const skipped: AdvisorSkip[] = [];
  for (const rule of ADVISOR_RULES) {
    try {
      const reason = rule.skipWhen?.(snapshot) ?? null;
      if (reason !== null) {
        skipped.push({ rule, reason, partial: null });
        continue;
      }
      // Collected before any row is pushed, so a detector that throws part-way
      // through leaves the result set untouched rather than half-populated.
      const findings = rule.detect(snapshot);
      const partial = rule.partialWhen?.(snapshot) ?? null;
      for (const finding of findings) {
        rows.push({ id: `${rule.id}/${finding.key}`, rule, finding });
      }
      if (partial !== null) {
        skipped.push({ rule, reason: partial.reason, partial: { unread: partial.unread } });
      }
    } catch (e) {
      const message = (e as Error).message;
      log.error(Cat.scene, `advisor rule ${rule.id} failed:`, message);
      skipped.push({ rule, reason: `the check itself errored (${message})`, partial: null });
    }
  }
  rows.sort((a, b) => SEVERITY[a.rule.severity].rank - SEVERITY[b.rule.severity].rank);
  return { rows, skipped };
}
