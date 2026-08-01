<script lang="ts">
  // Shared "these readings are frozen" notice for host-pushed telemetry. Any panel
  // showing such readings needs the same cue when its samples stop arriving, and it
  // has to read identically everywhere -- a panel that words it differently, or omits
  // it, lets a stale snapshot pass for a live one. ONE wording, ONE live region.
  //
  // The announced sentence is deliberately NOT the visible one. The visible copy
  // carries the age, which ticks once a second, and a polite live region re-announces
  // on every content change -- so a counter inside one would read the whole sentence
  // aloud every second for as long as the freeze lasts. The live region below holds a
  // sentence with no counter in it, and stays mounted while the readings are fresh
  // (holding the empty string), so its content changes exactly twice per freeze: once
  // when it starts and once when it ends. The visible banner is aria-hidden so the
  // same words aren't also met a second time by a reader browsing the panel.
  interface Props {
    /** Whether the readings are currently frozen. */
    stale: boolean;
    /** Age of the newest sample in whole seconds. Shown, never announced. */
    ageSec: number;
    /** What the frozen readings are, for the sentence ("numbers", "statistics"). */
    subject?: string;
    /** Draw the visible banner. Pass false for a host that already draws its own
     * inline cue and wants only the announcement (the Studio bottom bar). */
    banner?: boolean;
  }
  let { stale, ageSec, subject = "numbers", banner = true }: Props = $props();
</script>

<span class="announce" role="status" aria-live="polite">{stale ? `Frozen: these ${subject} are not current.` : ""}</span>

{#if stale && banner}
  <p class="stale-notice" aria-hidden="true">
    Frozen — no update for {ageSec}s. These {subject} are not current.
  </p>
{/if}

<style>
  /* Present to assistive tech, absent from layout and from sight. */
  .announce {
    position: absolute;
    width: 1px;
    height: 1px;
    margin: -1px;
    padding: 0;
    overflow: hidden;
    clip-path: inset(50%);
    white-space: nowrap;
    border: 0;
  }

  .stale-notice {
    margin: 0;
    padding: 8px 11px;
    font-size: 11px;
    line-height: 1.4;
    color: var(--color-warn);
    border-left: 2px solid var(--color-warn);
    background: color-mix(in srgb, var(--color-warn) 12%, transparent);
  }
</style>
