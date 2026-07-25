<script lang="ts" module>
  // The destination selector shared by the Events feed filter and the Multichat
  // strip. It is deliberately NOT called a filter: in Chat the same selection is
  // also the send target, so the component owns presentation and reports intent
  // while the parent owns what a selection means.
  //
  // Three levels, because platform separation is not replaced by destination
  // separation -- it is joined by it. "Everything on both YouTube channels" and
  // "only the vertical cut of this one" are different questions and both have to be
  // one click away. A flat chip per combination does not scale (two platforms x two
  // channels x two orientations already), so destinations are grouped under their
  // platform and the platform itself is the group's scope chip.

  // The selection vocabulary itself lives in destinationSelection.ts: this component
  // renders a selection, it does not define what one means.

  export type DestinationChipTone = "ok" | "warn" | "down";

  /** Optional per-destination transport state. `note` is mandatory in practice
   * whenever `tone` or `unavailable` is set: the tone is a dot, and a dot alone
   * cannot be the only carrier of the state. */
  export interface DestinationChipStatus {
    tone?: DestinationChipTone;
    note?: string;
    unavailable?: boolean;
  }

  const TONE_COLOR: Record<DestinationChipTone, string> = {
    ok: "var(--color-ok)",
    warn: "var(--color-warn)",
    down: "var(--color-live)",
  };
</script>

<script lang="ts">
  import type { DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import { ABSENT_LABEL, ALL_DESTINATIONS, type DestinationSelection } from "$lib/ui/destinationSelection";

  interface Props {
    /** Rendered in the order given -- the caller owns ordering, and groups appear in
     * first-appearance order of this list so a user's profile order survives. */
    destinations: readonly DestinationIdentity[];
    value: DestinationSelection;
    onSelect: (next: DestinationSelection) => void;
    /** Platforms with a connected account but no destination. They still run a
     * transport, so they get a disabled chip that says why rather than no chip. */
    unarmedPlatforms?: readonly string[];
    unarmedHint?: (platform: string) => string;
    /** Per-destination transport state; never consulted for the All/platform chips,
     * which are scopes rather than destinations. */
    statusOf?: (d: DestinationIdentity) => DestinationChipStatus | undefined;
    /** Hover text, so Events can say "only events from" where Chat says "reply in".
     * The accessible name is composed here regardless and is never overridden. */
    titleOf?: (d: DestinationIdentity, canvas: string) => string;
    /** Defaults to "there is more than one chip", the point at which All means
     * something. */
    showAll?: boolean;
  }
  let {
    destinations,
    value,
    onSelect,
    unarmedPlatforms = [],
    unarmedHint,
    statusOf,
    titleOf,
    showAll,
  }: Props = $props();

  const NOT_ARMED = "not armed";

  interface Group {
    platform: string;
    label: string;
    color: string;
    members: DestinationIdentity[];
  }

  let groups = $derived.by<Group[]>(() => {
    const byPlatform = new Map<string, Group>();
    for (const d of destinations) {
      const platform = platformKey(d.platform);
      const group = byPlatform.get(platform);
      if (group) {
        group.members.push(d);
      } else {
        byPlatform.set(platform, {
          platform,
          label: PLATFORM_LABELS[platform] ?? d.platform,
          color: PLATFORM_COLORS[platform] || "var(--color-accent)",
          members: [d],
        });
      }
    }
    return [...byPlatform.values()];
  });

  let withAll = $derived(showAll ?? destinations.length + unarmedPlatforms.length >= 2);

  // accountId -> how many destinations share it, for the disambiguation rule below.
  let siblingCount = $derived.by(() => {
    const m = new Map<string, number>();
    for (const d of destinations) {
      m.set(d.accountId, (m.get(d.accountId) ?? 0) + 1);
    }
    return m;
  });

  // The canvas earns chip space only where it disambiguates -- one destination per
  // channel means the channel name already identifies it. This states what the
  // destination currently points at, which is a different question from what an
  // event can be attributed to; hence "not armed", never "channel-wide".
  function canvasTag(d: DestinationIdentity): string {
    if ((siblingCount.get(d.accountId) ?? 1) < 2) {
      return "";
    }
    return d.canvasUuid === null ? NOT_ARMED : (d.canvasName ?? ABSENT_LABEL);
  }

  // `status` is spelled `| undefined` rather than `status?`: Svelte's built-in TS
  // erasure drops the annotation but leaves the question mark behind, so an optional
  // parameter emits a bare marker in the parameter list -- invalid JS that only the
  // Vite build catches, never svelte-check.
  function accessibleName(d: DestinationIdentity, canvas: string, status: DestinationChipStatus | undefined): string {
    const platform = PLATFORM_LABELS[platformKey(d.platform)] ?? d.platform;
    return (
      [platform, d.displayName, canvas].filter((part) => part !== "").join(" · ") +
      (status?.note ? " — " + status.note : "")
    );
  }

  function hoverText(d: DestinationIdentity, canvas: string, status: DestinationChipStatus | undefined): string {
    // The note is appended below, so the base name is composed without it.
    const base = titleOf?.(d, canvas) ?? accessibleName(d, canvas, undefined);
    return status?.note && !base.endsWith(status.note) ? base + " — " + status.note : base;
  }

  function fallbackUnarmedHint(platform: string): string {
    return (PLATFORM_LABELS[platformKey(platform)] ?? platform) + " has no destination configured.";
  }
</script>

<div class="chips">
  {#if withAll}
    <button
      class="chip scope"
      class:on={value.kind === "all"}
      aria-pressed={value.kind === "all"}
      aria-label={"All " + destinations.length + " destinations"}
      onclick={() => onSelect(ALL_DESTINATIONS)}
    >
      All{destinations.length >= 2 ? " " + destinations.length : ""}
    </button>
  {/if}

  {#each groups as g (g.platform)}
    <!-- Members share one border line so the group reads as one unit rather than as
         three unrelated chips; the platform chip only appears when selecting it is
         distinguishable from selecting its single member. -->
    <div class="group" role="group" aria-label={g.label + " destinations"}>
      {#if g.members.length >= 2}
        {@const selected = value.kind === "platform" && value.platform === g.platform}
        <button
          class="chip scope"
          class:on={selected}
          aria-pressed={selected}
          aria-label={"All " + g.label + " destinations"}
          title={"All " + g.label + " destinations"}
          style:--chip={g.color}
          onclick={() => onSelect({ kind: "platform", platform: g.platform })}
        >
          <PlatformMark platform={g.platform} size={12} />
          {g.label}
        </button>
      {/if}
      {#each g.members as d (d.profileUuid)}
        {@const canvas = canvasTag(d)}
        {@const status = statusOf?.(d)}
        {@const selected = value.kind === "destination" && value.profileUuid === d.profileUuid}
        <button
          class="chip"
          class:on={selected}
          disabled={status?.unavailable ?? false}
          aria-pressed={selected}
          aria-label={accessibleName(d, canvas, status)}
          title={hoverText(d, canvas, status)}
          style:--chip={g.color}
          onclick={() => onSelect({ kind: "destination", profileUuid: d.profileUuid })}
        >
          <!-- The mark hoists to the group's scope chip once a platform has two
               destinations; keeping it on every member would spend the width the
               channel name needs and repeat what the group already says. -->
          {#if g.members.length < 2}
            <PlatformMark platform={d.platform} size={12} />
          {/if}
          <span class="cname">{d.displayName}</span>
          {#if canvas}<span class="ccanvas">{canvas}</span>{/if}
          {#if status?.tone}<span class="sdot" style:background={TONE_COLOR[status.tone]}></span>{/if}
        </button>
      {/each}
    </div>
  {/each}

  {#each unarmedPlatforms as p (p)}
    {@const hint = (unarmedHint ?? fallbackUnarmedHint)(p)}
    <button class="chip" disabled title={hint} aria-label={hint}>
      <PlatformMark platform={p} size={12} />
      <span class="cname">{PLATFORM_LABELS[platformKey(p)] ?? p}</span>
    </button>
  {/each}
</div>

<style>
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 6px;
    min-width: 0;
  }
  .group {
    display: flex;
    flex-wrap: wrap;
    min-width: 0;
  }
  /* Hairline box, brand color applied through --chip on select. */
  .chip {
    display: flex;
    align-items: center;
    gap: 4px;
    max-width: 100%;
    min-width: 0;
    padding: 3px 8px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  /* Overlap the shared edge instead of dropping a border, so a selected chip's own
     brand-colored box still paints all four sides. */
  .group .chip + .chip {
    margin-left: calc(-1 * var(--border-weight));
  }
  .chip.on {
    position: relative;
    z-index: 1;
    border-color: var(--chip, var(--color-accent));
    color: var(--color-text);
    background: color-mix(in srgb, var(--chip, var(--color-accent)) 14%, transparent);
  }
  /* Mono types a chip as a scope ("everything under this") rather than as one thing. */
  .chip.scope {
    font-family: var(--font-mono);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  /* One rule for every unselectable chip, whichever reason produced it: a connected
     platform with no destination, or a destination with no usable chat transport. The
     dashed edge is the carrier that survives when the hues do not -- opacity and the
     muted text are luminance, and the cursor only speaks to a mouse. Placed after
     .chip.on so a selected chip that loses its transport still reads unselectable. */
  .chip:disabled {
    cursor: not-allowed;
    opacity: 0.5;
    border-style: dashed;
    color: var(--color-muted);
  }
  .chip:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
    position: relative;
    z-index: 2;
  }
  .cname {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ccanvas {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    color: var(--color-dim);
    max-width: 9ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .chip.on .ccanvas {
    color: var(--color-text);
  }
  .chip:disabled .ccanvas {
    color: var(--color-muted);
  }
  .sdot {
    flex: 0 0 auto;
    width: 5px;
    height: 5px;
  }
</style>
