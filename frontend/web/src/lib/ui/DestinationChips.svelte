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
  import { platformChipColor, platformKey, platformName } from "$lib/theme/platformColors";
  import { TRANSPORT_STATE_COLOR } from "$lib/theme/stateColors";
  import Avatar from "$lib/ui/Avatar.svelte";
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
     * something. Passing `false` explicitly also removes the scope row's only
     * always-present visual anchor -- see withScopes -- leaving a row of bare platform
     * marks that a destination chip can match pixel for pixel. Neither dock passes it. */
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
          label: platformName(d.platform),
          color: platformChipColor(platform),
          members: [d],
        });
      }
    }
    return [...byPlatform.values()];
  });

  let withAll = $derived(showAll ?? destinations.length + unarmedPlatforms.length >= 2);

  // The platform chips repeat what the All chip already says once there is only one
  // platform in play -- they add no information, so they are suppressed rather than
  // rendered redundantly.
  let withPlatforms = $derived(groups.length + unarmedPlatforms.length >= 2);

  // The scope row holds both kinds, so it survives either one alone: `showAll: false`
  // with several platforms, and a single platform holding several destinations, each
  // still produce one populated row rather than an empty one with a divider under it.
  //
  // Left to itself, withPlatforms implies withAll, and that implication is what tells the
  // two rows apart on screen: `groups` partitions `destinations`, so groups.length is at
  // most destinations.length, so groups + unarmed >= 2 gives destinations + unarmed >= 2,
  // which is withAll's own test. The scope row therefore always opens with the `All` chip
  // (bare at one destination, `All N` above that) while the destination row never holds
  // one -- a text anchor no theme setting can switch off, which matters because every
  // chip after it is a bare mark that a destination chip can match exactly.
  // `showAll: false` is the one thing that breaks it.
  let withScopes = $derived(withAll || withPlatforms);

  // The destination row has nothing to show once there are no destinations.
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
    return (
      [platformName(d.platform), d.displayName, canvas].filter((part) => part !== "").join(" · ") +
      (status?.note ? " — " + status.note : "")
    );
  }

  function hoverText(d: DestinationIdentity, canvas: string, status: DestinationChipStatus | undefined): string {
    // The platform is led in even where the caller supplied its own wording: no chip in
    // this component prints a platform, a channel or a canvas as text any more, so the
    // hover text is the only place a pointer can recover any of the three. `titleOf`
    // already carries the channel and the canvas; the platform is the part it omits.
    // The note is appended below, so the base is composed without it.
    const base = titleOf ? platformName(d.platform) + " — " + titleOf(d, canvas) : accessibleName(d, canvas, undefined);
    return status?.note && !base.endsWith(status.note) ? base + " — " + status.note : base;
  }

  function fallbackUnarmedHint(platform: string): string {
    return platformName(platform) + " has no destination configured.";
  }
</script>

<div class="chips">
  {#if withScopes}
    <!-- One row for both kinds of scope. They were split only because a row of chips
         reading "All 7  YouTube  Twitch  Kick  Facebook Live" needed the width; with the
         platform reduced to its mark there is no longer a second line's worth to place,
         and "everything" and "everything on this platform" are the same question asked at
         two widths.
         Every platform with at least one destination gets a chip, even one with a single
         destination: a chip that appeared only once a platform had 2+ members would make
         its presence unpredictable. -->
    <div class="row" role="group" aria-label="Scopes">
      {#if withAll}
        <!-- Pluralized because withAll counts unarmed platforms too, so this chip can
             render over a single destination and read "All 1 destinations". -->
        {@const allLabel = "All " + destinations.length + (destinations.length === 1 ? " destination" : " destinations")}
        <button
          class="chip scope"
          class:on={value.kind === "all"}
          aria-pressed={value.kind === "all"}
          aria-label={allLabel}
          title={allLabel}
          onclick={() => onSelect(ALL_DESTINATIONS)}
        >
          <!-- Kept as a word: "all" is a scope, not the name of anything, and it has no
               mark to fall back to. -->
          All{destinations.length >= 2 ? " " + destinations.length : ""}
        </button>
      {/if}
      {#if withPlatforms}
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
          </button>
        {/each}
        {#each unarmedPlatforms as p (p)}
          {@const hint = (unarmedHint ?? fallbackUnarmedHint)(p)}
          <!-- `scope` too, though it can never be pressed: it sits among scope chips and
               a square chip here would read as one of the destinations below.
               aria-disabled with the styling by class rather than the `disabled`
               attribute: Chromium does not dispatch mouse events to a disabled form
               control, so `title` never fires on hover, and this chip is a bare mark
               whose entire identity is that tooltip -- it exists to explain a state
               (connected, no destination) that is otherwise invisible.
               It is therefore focusable and clickable, and is safe only because it has
               no handler. Anything added to it must check aria-disabled first. -->
          <button class="chip scope unselectable" aria-disabled="true" title={hint} aria-label={hint}>
            <PlatformMark platform={p} size={12} />
          </button>
        {/each}
      {/if}
    </div>
  {/if}

  {#if withScopes && withStreams}
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
            {@const unavailable = status?.unavailable ?? false}
            <!-- aria-disabled rather than the `disabled` attribute, same as the unarmed
                 scope chip above: a disabled control receives no mouse events in Chromium
                 and is skipped by the tab order, and this chip's title is where the
                 transport's note lives -- the reason a destination is down, which is the
                 one string a streamer goes looking for mid-broadcast. Unselectable is
                 unchanged; only reaching the explanation is new.
                 The early return below is the guard, NOT the styling: this button is
                 focusable, so Enter and Space activate it and no CSS intercepts that. -->
            <button
              class="chip"
              class:on={selected}
              class:toned={tone !== ""}
              class:unselectable={unavailable}
              aria-disabled={unavailable}
              aria-pressed={selected}
              aria-label={accessibleName(d, canvas, status)}
              title={hoverText(d, canvas, status)}
              style:--chip={platformChipColor(d.platform)}
              style:--tone={tone}
              onclick={() => {
                if (unavailable) {
                  return;
                }
                onSelect({ kind: "destination", profileUuid: d.profileUuid });
              }}
            >
              <!-- Platform as a mark, never as a word: the name is carried by aria-label,
                   which is then the only carrier and must not be dropped. -->
              <PlatformMark platform={d.platform} size={12} />
              {#if inline}
                {@render canvasFace(inline)}
              {:else if !g.canvas}
                <!-- No canvas, so there is no number to reduce this chip to. The channel
                     avatar takes the place the channel name held: a caller that keeps
                     unarmed chips (Events, whose transports are account-scoped) has four
                     YouTube destinations here, and without a per-channel mark they would
                     be four controls with identical visible presentation. Avatar's
                     monogram fallback still discriminates before an image loads. A
                     caller that passes only armed destinations never reaches this branch.
                     The state word rather than a dash: unarmedLabel distinguishes
                     "disabled" from "not armed" because they ask for opposite actions,
                     and this is the branch that exists to say which. -->
                <Avatar url={d.channelAvatarUrl} name={d.displayName} size={14} />
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
  /* What separates a scope chip from a destination chip is structural, not decorative:
     the scope row always opens with the `All N` text chip and the destination row never
     contains one (see withScopes above for why that holds), the divider sits between
     them, and a multi-member destination group opens with a canvas mark. The styling
     here is a second leg, not the carrier.

     Two decorative carriers were tried and do not work in this app, which is worth
     recording because both look available:
     - radius is unreachable. app.css applies `border-radius: 0 !important` to `*`, and
       author-origin !important outranks a scoped class.
     - a background band on the row is imperceptible. --color-surface against the two
       grounds the docks put it on measures 1.07-1.14:1 in every shipped preset, and the
       step inverts -- darker than EventsDock's .bar (--color-surface-2), lighter than
       MultichatDock's .dests (--color-base) -- so it reads as a recess in one dock and a
       lift in the other.

     The border is doubled as a RELATIONSHIP, not a constant: --border-weight is a
     user-editable Appearance token offering 1px and 2px, so a hardcoded 2px here
     silently collapses to no difference the moment a user picks the heavier setting.
     Mono still types the one scope chip that has text. */
  .chip.scope {
    border-width: calc(var(--border-weight) * 2);
    font-family: var(--font-mono);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  /* One rule for every unselectable chip, whichever reason produced it: a connected
     platform with no destination, or a destination with no usable chat transport. The
     dashed edge is the carrier that survives when the hues do not -- opacity and the
     muted text are luminance, and the cursor only speaks to a mouse. Placed after
     .chip.on because both set `color`, so a selected chip that loses its transport still
     reads unselectable. Order against .chip.scope is not load-bearing: dashed is
     border-style and the doubled edge is border-width, so a scope chip carries both.
     Keyed on a class rather than :disabled because no chip here uses the attribute: a
     `disabled` control receives no mouse events in Chromium and is skipped by the tab
     order, and every chip in this strip is a mark whose identity is its tooltip. */
  .chip.unselectable {
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
  .chip.unselectable .ccanvas {
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
