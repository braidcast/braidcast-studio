<script lang="ts" module>
  // Per-mark geometry. `viewBox` is per entry rather than one shared constant because
  // the brand paths are authored on different grids: Kick's glyph occupies
  // x 2..22, y 2..26, so the 24x24 the others use would flat-cut its legs. The
  // <svg> box stays size x size in every case -- preserveAspectRatio letterboxes a
  // non-square viewBox -- so a row of marks stays optically level. Adding a platform
  // is one entry here plus its color/label in platformColors.ts.
  interface Mark {
    viewBox: string;
    /** Draw order; a `knockout` path is the cutout inside the mark (YouTube's play
     * triangle), which takes PLATFORM_MARK_KNOCKOUT instead of the brand color. */
    paths: { d: string; knockout?: boolean }[];
  }

  const MARKS: Record<string, Mark> = {
    youtube: {
      viewBox: "0 0 24 24",
      paths: [
        {
          d: "M23.5 6.2a3 3 0 0 0-2.1-2.1C19.5 3.6 12 3.6 12 3.6s-7.5 0-9.4.5A3 3 0 0 0 .5 6.2C0 8.1 0 12 0 12s0 3.9.5 5.8a3 3 0 0 0 2.1 2.1c1.9.5 9.4.5 9.4.5s7.5 0 9.4-.5a3 3 0 0 0 2.1-2.1C24 15.9 24 12 24 12s0-3.9-.5-5.8z",
        },
        { d: "M9.6 15.6V8.4l6.2 3.6-6.2 3.6z", knockout: true },
      ],
    },
    twitch: {
      viewBox: "0 0 24 24",
      paths: [
        {
          d: "M4.3 0 1 3.3v17.4h5.7V24l3.3-3.3h4.4L21.9 14V0H4.3zm15.4 13.1-3.3 3.3h-5.4l-3 3v-3H4.5V1.6h15.2v11.5z",
        },
        { d: "M14.6 5.2h1.9v5.5h-1.9zM9.7 5.2h1.9v5.5H9.7z" },
      ],
    },
    kick: {
      viewBox: "2 2 20 24",
      paths: [
        {
          d: "M2 2h6.4v5.6h3.2V4.4h3.2V2H22v7.2h-3.2v3.2h-3.2v3.2h3.2v3.2H22V26h-6.4v-2.4h-3.2v-3.2H8.4V26H2z",
        },
      ],
    },
    facebook: {
      viewBox: "0 0 24 24",
      paths: [
        {
          d: "M24 12.07C24 5.4 18.63 0 12 0S0 5.4 0 12.07C0 18.1 4.39 23.09 10.13 24v-8.44H7.08v-3.49h3.05V9.41c0-3.02 1.79-4.69 4.53-4.69 1.31 0 2.68.24 2.68.24v2.97h-1.51c-1.49 0-1.96.93-1.96 1.89v2.25h3.33l-.53 3.49h-2.8V24C19.61 23.09 24 18.1 24 12.07z",
        },
      ],
    },
  };
</script>

<script lang="ts">
  // The platform's logo in its brand color, replacing the word "YouTube"/"Twitch"/
  // "Kick" everywhere a destination is named. The word and the channel name used to
  // sit in the same position and weight, so the platform did no work telling two
  // adjacent destinations apart; a mark does, and it gives the channel name back the
  // row width the "YouTube - " prefix was eating.
  //
  // The name is NOT dropped, only unshouted: role="img" + aria-label keeps it in the
  // accessibility tree, so a screen reader still hears the platform a sighted user
  // now reads from the logo.
  import { PLATFORM_COLORS, PLATFORM_LABELS, PLATFORM_MARK_KNOCKOUT, platformKey } from "$lib/theme/platformColors";

  interface Props {
    /** Platform keyed the way platformColors.ts keys it ("youtube" | "twitch" | "kick").
     * Normalized on the way in, so an OAuth providerId or a StreamProfileInfo.platform
     * prefix ("YouTube") can be passed straight through. Anything without a brand path
     * renders the neutral fallback. */
    platform: string;
    size?: number;
    /** Accessible name override; defaults to the platform's display label. */
    title?: string;
  }
  let { platform, size = 16, title }: Props = $props();

  const key = $derived(platformKey(platform));
  const mark = $derived(MARKS[key]);
  // Unknown platform: a neutral square, never a blank box. The label still resolves --
  // to the raw string the caller passed if it isn't one we have a mark for -- so an
  // unrecognized provider degrades to "a dot that announces its name".
  const color = $derived(PLATFORM_COLORS[key] || "var(--color-muted)");
  const label = $derived(title?.trim() || PLATFORM_LABELS[key] || platform.trim() || "Unknown platform");
</script>

{#if mark}
  <svg width={size} height={size} viewBox={mark.viewBox} role="img" aria-label={label}>
    {#each mark.paths as p (p.d)}
      <path fill={p.knockout ? PLATFORM_MARK_KNOCKOUT : color} d={p.d} />
    {/each}
  </svg>
{:else}
  <svg width={size} height={size} viewBox="0 0 24 24" role="img" aria-label={label}>
    <rect x="7" y="7" width="10" height="10" fill={color} />
  </svg>
{/if}

<style>
  /* Block so the mark never picks up a text baseline gap inside a row, and never
     shrinks when it lands directly in a flex line. */
  svg {
    display: block;
    flex: none;
  }
</style>
