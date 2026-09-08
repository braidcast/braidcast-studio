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
  //
  // The individual-destination row groups by CANVAS instead, and carries nothing but
  // marks: a canvas number said once for its group, then one platform mark per
  // destination under it. Spelling out either name is what made one destination occupy
  // three stacked rows of a feed that has none to spare.

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
  import { PLATFORM_LABELS, platformChipColor, platformKey } from "$lib/theme/platformColors";
  import { TRANSPORT_STATE_COLOR } from "$lib/theme/stateColors";
  import CanvasMark from "$lib/ui/CanvasMark.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import { ABSENT_LABEL, ALL_DESTINATIONS, type DestinationSelection } from "$lib/ui/destinationSelection";

  interface Props {
    /** The caller owns ordering. Both grouped rows regroup this list rather than
     * reordering it: a group takes the position of its first member, and members keep
     * their given order within it, so a profile's place survives as long as its
     * neighbours share its group. A flat left-to-right reading of the last row is
     * therefore the given order only when no two groups interleave. */
    destinations: readonly DestinationIdentity[];
    value: DestinationSelection;
    onSelect: (next: DestinationSelection) => void;
    /** Platforms with a connected account but no destination, as a disabled chip that
     * says why rather than no chip. Whether they belong here is the caller's call and
     * differs by surface: the events transports run per connected ACCOUNT
     * (`frontend/src/events/event_hub.hpp`), so an unarmed platform can still put rows
     * in that feed, while chat runs per enabled output BINDING
     * (`frontend/src/chat/chat_hub.hpp`) and cannot. */
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
          color: platformChipColor(platform),
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

  /** What the canvas mark draws. Null for a destination that has no canvas at all --
   * there is no number to stand in for one, and CanvasMark's "?" means "not numbered
   * yet", which is a different claim. */
  type CanvasFace = { number: number; name: string; width: number; height: number } | null;

  interface CanvasGroup {
    key: string;
    canvas: CanvasFace;
    members: DestinationIdentity[];
  }

  // Row 3 groups by canvas rather than by platform: the canvas is what a destination
  // shares with its siblings, so its number can be said once for the whole group and the
  // members reduced to their marks. Each group takes the position of its first member
  // and members keep their given order inside it -- a regrouping of the caller's order,
  // not a re-sort of it.
  let canvasGroups = $derived.by<CanvasGroup[]>(() => {
    const out: CanvasGroup[] = [];
    const byCanvas = new Map<string, CanvasGroup>();
    for (const d of destinations) {
      if (d.canvasUuid === null) {
        // Nothing to group under, so it stands alone rather than being pooled with the
        // other canvas-less destinations into a scope none of them share.
        out.push({ key: d.profileUuid, canvas: null, members: [d] });
        continue;
      }
      const group = byCanvas.get(d.canvasUuid);
      if (group) {
        group.members.push(d);
      } else {
        const fresh: CanvasGroup = {
          key: d.canvasUuid,
          canvas: {
            number: d.canvasNumber,
            name: d.canvasName ?? ABSENT_LABEL,
            width: d.canvasWidth,
            height: d.canvasHeight,
          },
          members: [d],
        };
        byCanvas.set(d.canvasUuid, fresh);
        out.push(fresh);
      }
    }
    return out;
  });

  // The canvas in words, for the accessible name and the tooltip only -- the strip now
  // carries it as a number, and the name is the one thing that number cannot be resolved
  // to without hovering. This states what the destination currently points at, which is a
  // different question from what an event can be attributed to; hence "not armed", never
  // "channel-wide".
  function canvasWord(d: DestinationIdentity): string {
    return d.canvasUuid !== null ? (d.canvasName ?? ABSENT_LABEL) : unarmedLabel(d);
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

  <!-- 16 rather than the 12-13 the marks around it use: this numeral is the only visible
       carrier of which canvas a chip points at, and CanvasMark scales its digit to 0.58
       of the box, so 13 would set it at about 7.5px. -->
  {#snippet canvasFace(c: NonNullable<CanvasFace>)}
    <CanvasMark number={c.number} name={c.name} width={c.width} height={c.height} size={16} />
  {/snippet}

  {#if withStreams}
    <div class="row bycanvas" role="group" aria-label="Streams">
      {#each canvasGroups as g (g.key)}
        <!-- A canvas-less destination is always alone in its group, so only a group with
             siblings ever leads with a mark of its own; the single case carries its face
             inside its one chip instead. -->
        {@const lead = g.members.length >= 2 ? g.canvas : null}
        <div class="cgroup">
          {#if lead}
            <!-- Said once for the group instead of once per member: every chip under it
                 points at this canvas, so repeating the number would be the width the
                 channel name used to eat. -->
            {@render canvasFace(lead)}
            <span class="csep" aria-hidden="true">|</span>
          {/if}
          {#each g.members as d (d.profileUuid)}
            {@const canvas = canvasWord(d)}
            <!-- The face this chip draws itself; null when the group's lead already drew
                 it, and when there is no canvas to draw. -->
            {@const inline = g.members.length === 1 ? g.canvas : null}
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
              style:--chip={platformChipColor(d.platform)}
              style:--tone={tone}
              onclick={() => onSelect({ kind: "destination", profileUuid: d.profileUuid })}
            >
              <!-- Platform as a mark, never as a word: the name is carried by aria-label,
                   which is then the only carrier and must not be dropped. -->
              <PlatformMark platform={d.platform} size={12} />
              {#if inline}
                {@render canvasFace(inline)}
              {:else if !g.canvas}
                <!-- No canvas, so there is no number to reduce this chip to and the
                     channel name has to stay. A caller that passes only armed
                     destinations never reaches this branch and gets the compact form
                     throughout; a caller that deliberately keeps unarmed chips (Events,
                     whose transports are account-scoped) would otherwise render every
                     destination of one platform as the same anonymous mark.
                     The state word rather than a dash: unarmedLabel distinguishes
                     "disabled" from "not armed" because they ask for opposite actions,
                     and this is the branch that exists to say which. -->
                <span class="cname">{d.displayName}</span>
                <span class="ccanvas">{canvas}</span>
              {/if}
            </button>
          {/each}
        </div>
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
  /* Canvas groups sit twice as far apart as the chips inside one, so a canvas's marks
     read as belonging to its number rather than to the next group along. */
  .row.bycanvas {
    column-gap: 8px;
  }
  .cgroup {
    display: flex;
    /* Wraps inside itself rather than pushing the strip wider: a detached dock can be
       narrower than one canvas's worth of marks, and the 8px between groups still reads
       as the larger break. */
    flex-wrap: wrap;
    align-items: center;
    gap: 4px;
    min-width: 0;
    /* The group's canvas mark sits outside any chip, so it takes its color from here; a
       chip's own mark inherits the chip's instead and tracks selected/disabled. Full
       text color, not the chips' dim: it is the only thing naming the canvas. */
    color: var(--color-text);
  }
  .csep {
    flex: none;
    font-family: var(--font-mono);
    font-size: 11px;
    line-height: 1;
    color: var(--color-muted);
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
