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

  import type { TransportHealthState } from "$lib/api/bridge";

  /** Optional per-destination transport state. `note` is mandatory in practice
   * whenever `state` or `unavailable` is set: the state is an edge color, and a color
   * alone cannot be the only carrier of it. */
  export interface DestinationChipStatus {
    state?: TransportHealthState;
    note?: string;
    unavailable?: boolean;
  }
</script>

<script lang="ts">
  import { unarmedLabel, type DestinationIdentity } from "$lib/stores/destinationIdentityStore.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";
  import { TRANSPORT_STATE_COLOR } from "$lib/theme/stateColors";
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

  // Row 2 (platform scope chips) repeats what Row 1's All chip already says once there
  // is only one platform in play -- it adds no information, so it is suppressed rather
  // than rendered redundantly.
  let withPlatforms = $derived(groups.length + unarmedPlatforms.length >= 2);

  // Row 3 (individual streams) has nothing to show once there are no destinations.
  let withStreams = $derived(groups.length > 0);

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
    if (d.canvasUuid !== null) {
      return d.canvasName ?? ABSENT_LABEL;
    }
    return unarmedLabel(d);
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
    <div class="row" role="group" aria-label="All destinations">
      <button
        class="chip scope"
        class:on={value.kind === "all"}
        aria-pressed={value.kind === "all"}
        aria-label={"All " + destinations.length + " destinations"}
        onclick={() => onSelect(ALL_DESTINATIONS)}
      >
        All{destinations.length >= 2 ? " " + destinations.length : ""}
      </button>
    </div>
  {/if}

  {#if withAll && withPlatforms}
    <div class="row-divider" aria-hidden="true"></div>
  {/if}

  {#if withPlatforms}
    <!-- Every platform with at least one destination gets a scope chip here, even one
         with a single destination: a chip that only shows up once a platform has 2+
         members would make this row's very existence unpredictable. A dedicated row
         makes the redundancy with its one member legible instead of confusing. -->
    <div class="row" role="group" aria-label="Platforms">
      {#each groups as g (g.platform)}
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
      {/each}
      {#each unarmedPlatforms as p (p)}
        {@const hint = (unarmedHint ?? fallbackUnarmedHint)(p)}
        <button class="chip" disabled title={hint} aria-label={hint}>
          <PlatformMark platform={p} size={12} />
          <span class="cname">{PLATFORM_LABELS[platformKey(p)] ?? p}</span>
        </button>
      {/each}
    </div>
  {/if}

  {#if (withAll || withPlatforms) && withStreams}
    <div class="row-divider" aria-hidden="true"></div>
  {/if}

  {#if withStreams}
    <div class="row" role="group" aria-label="Streams">
      {#each groups as g (g.platform)}
        {#each g.members as d (d.profileUuid)}
          {@const canvas = canvasTag(d)}
          {@const status = statusOf?.(d)}
          {@const tone = status?.state ? TRANSPORT_STATE_COLOR[status.state] : ""}
          {@const selected = value.kind === "destination" && value.profileUuid === d.profileUuid}
          <button
            class="chip"
            class:on={selected}
            class:toned={tone !== ""}
            disabled={status?.unavailable ?? false}
            aria-pressed={selected}
            aria-label={accessibleName(d, canvas, status)}
            title={hoverText(d, canvas, status)}
            style:--chip={g.color}
            style:--tone={tone}
            onclick={() => onSelect({ kind: "destination", profileUuid: d.profileUuid })}
          >
            <!-- Always on, never hoisted to a group chip: this row is flat across
                 platforms now, so the mark is the only thing telling apart two
                 same-named channels on different platforms (e.g. two "AnimeCruizer"s,
                 one on Twitch, one on Kick). -->
            <PlatformMark platform={d.platform} size={12} />
            <span class="cname">{d.displayName}</span>
            {#if canvas}<span class="ccanvas">{canvas}</span>{/if}
          </button>
        {/each}
      {/each}
    </div>
  {/if}
</div>

<style>
  .chips {
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .row {
    display: flex;
    flex-wrap: wrap;
    gap: 4px 6px;
    min-width: 0;
  }
  /* Full-width hairline between adjacent non-empty rows -- same border idiom as the
     chips themselves, just on the shared edge instead of all four sides. */
  .row-divider {
    flex: none;
    border-top: var(--border-weight) solid var(--color-border);
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
  /* Transport state rides the chip's leading edge rather than a mark of its own: the
     other three sides are already spoken for (brand color = selected, dashed =
     unselectable), and at this size a second indicator crowds the name it describes.
     Placed last so the edge survives both. Padding gives back the extra pixel, so a
     chip with a state lines up with one that has none. */
  .chip.toned {
    border-left: 2px solid var(--tone);
    padding-left: 7px;
  }
</style>
