<script lang="ts">
  // What a go-live actually carried, when it did not carry what was asked. Persistent and
  // dismissible rather than a toast: the sibling refusal (streaming.startFailed) can be a toast
  // because nothing started and the streamer is still sitting at the button, whereas this one
  // stays true for the whole broadcast -- a 4-second banner for "your broadcast is public under
  // last week's title" is a notice designed to be missed. It also answers a push made from the
  // Stream Information dialog before anything is live, which is why the wording is not fixed.
  // Its host clears it on the stop edge, on a refused go-live and on a modal abort, and replaces a
  // destination's entry when that destination is pushed again, so it never outlives what it
  // describes.
  //
  // DIFFERS, NOT APPLIED and UNCONFIRMED are kept apart everywhere, including in the announced
  // sentence. "The platform has X instead", "the push never landed" and "we could not read the
  // platform at all" lead to different actions, and this whole change exists to stop the app
  // asserting a platform state it does not have.
  import { streamProfileStore } from "$lib/stores/streamProfileStore.svelte";

  interface DivergedField {
    field: string;
    label: string;
    requested: string;
    actual: string;
    safety: boolean;
    remedy: string;
  }

  export interface MismatchRow {
    destination: string;
    profileUuid: string;
    reason: string;
    unconfirmed: string;
    fields: DivergedField[];
  }

  interface Props {
    rows: MismatchRow[];
    /** Whether a stream is actually running. A push made from the Stream Information dialog is
     * answered before anything goes live, and the notice must not say "streaming" then. */
    streaming: boolean;
    ondismiss: () => void;
  }
  let { rows, streaming, ondismiss }: Props = $props();

  // The Stream Information path emits an empty label -- it runs off the UI thread and cannot
  // read the profile store that owns the name. Resolved here instead of guessed there.
  function nameFor(row: MismatchRow): string {
    if (row.destination) {
      return row.destination;
    }
    const profile = streamProfileStore.profiles.find((p) => p.uuid === row.profileUuid);
    return profile?.targetName || profile?.label || "This destination";
  }

  // Counted for the announcement, which must not restate every field: the same split StaleNotice
  // draws, and for the same reason -- a live region holding this whole table would read every
  // destination and every value aloud at the moment the stream starts. The announced sentence
  // carries the shape of the problem; the table below is browsed normally.
  let divergedCount = $derived(rows.reduce((n, r) => n + r.fields.length, 0));
  let notAppliedCount = $derived(rows.filter((r) => r.reason).length);
  let unconfirmedCount = $derived(rows.filter((r) => r.unconfirmed).length);
  let announcement = $derived(
    [
      divergedCount > 0 ? `${divergedCount} stream info field${divergedCount === 1 ? "" : "s"} differ` : "",
      notAppliedCount > 0 ? `${notAppliedCount} destination${notAppliedCount === 1 ? "" : "s"} not updated` : "",
      unconfirmedCount > 0 ? `${unconfirmedCount} destination${unconfirmedCount === 1 ? "" : "s"} unconfirmed` : "",
    ]
      .filter(Boolean)
      .join(", "),
  );
  // The one sentence a screen reader gets, so it says which of the two situations this is: a push
  // that was answered before anything went live reads differently from one that arrived with the
  // stream already going out.
  let lead = $derived(streaming ? "Now streaming" : "Stream info not fully applied");
</script>

<!-- Mounted unconditionally, its text toggled, so the live region exists before it has anything
     to say -- a region inserted together with its content is not reliably announced. -->
<span class="sr-only" role="alert">{announcement && rows.length > 0 ? `${lead}: ${announcement}.` : ""}</span>

{#if rows.length > 0}
  <!-- Focusable because it scrolls: a keyboard user has no other way to reach the rows below the
       fold (WCAG 2.1.1). The lint rule does not model scroll containers. -->
  <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
  <div class="mismatch" role="group" tabindex="0" aria-label="Stream info that did not fully apply">
    <div class="head">
      <svg class="warn-icon" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
        <path d="M12 9v5" />
        <path d="M12 17.5v.01" />
        <path d="M10.3 3.9 1.8 18.4A2 2 0 0 0 3.5 21.4h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z" />
      </svg>
      <span class="title">
        {streaming ? "Streaming, but the stream info did not fully apply" : "The stream info did not fully apply"}
        {#if divergedCount > 0}<span class="tally">{divergedCount} field{divergedCount === 1 ? "" : "s"} differ</span>{/if}
        {#if notAppliedCount > 0}<span class="tally">{notAppliedCount} not updated</span>{/if}
        {#if unconfirmedCount > 0}<span class="tally">{unconfirmedCount} unconfirmed</span>{/if}
      </span>
      <button class="dismiss" type="button" aria-label="Dismiss stream info notice" onclick={ondismiss}>
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
          <path d="M6 6l12 12M18 6 6 18" />
        </svg>
      </button>
    </div>

    <ul class="rows">
      {#each rows as row (row.profileUuid + ":" + row.destination)}
        <li>
          <span class="dest">{nameFor(row)}</span>
          <span class="detail">
            {#each row.fields as f (f.field)}
              <span class="entry">
                <!-- The state word carries the meaning, not the color: the two conditions have to
                     stay apart for a reader who sees no styling at all. -->
                <span class="tag diverged">differs</span>
                <span class="fname">{f.label}</span>
                <span class="was">asked {f.requested || "(empty)"}</span>
                <span class="arrow" aria-hidden="true">→</span>
                <span class="now">platform has {f.actual || "(empty)"}</span>
                {#if f.remedy}<span class="remedy">{f.remedy}</span>{/if}
              </span>
            {/each}
            {#if row.reason}
              <span class="entry">
                <span class="tag unknown">not applied</span>
                <span class="fname">{row.reason}</span>
              </span>
            {/if}
            {#if row.unconfirmed}
              <span class="entry">
                <span class="tag unknown">unconfirmed</span>
                <span class="fname">{row.unconfirmed}</span>
              </span>
            {/if}
          </span>
        </li>
      {/each}
    </ul>
  </div>
{/if}

<style>
  .mismatch {
    flex: 0 0 auto;
    max-height: 21vh;
    overflow-y: auto;
    padding: 6px 10px 7px;
    font-size: 11px;
    line-height: 1.45;
    color: var(--color-warn);
    border-top: 2px solid var(--color-warn);
    background: color-mix(in srgb, var(--color-warn) 11%, var(--color-surface));
  }

  .head {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  .warn-icon {
    flex: 0 0 auto;
  }

  .title {
    display: flex;
    align-items: center;
    gap: 6px;
    flex: 1 1 auto;
    min-width: 0;
    font-weight: 600;
  }

  .tally {
    padding: 0 5px;
    font-weight: 500;
    font-variant-numeric: tabular-nums;
    border: 1px solid color-mix(in srgb, var(--color-warn) 45%, transparent);
  }

  .dismiss {
    flex: 0 0 auto;
    display: grid;
    place-items: center;
    width: 24px;
    height: 24px;
    padding: 0;
    color: inherit;
    background: none;
    border: 0;
    cursor: pointer;
  }

  .dismiss:hover {
    background: color-mix(in srgb, var(--color-warn) 22%, transparent);
  }

  .rows {
    margin: 4px 0 0;
    padding: 0 0 0 19px;
    list-style: none;
  }

  .rows li {
    display: flex;
    gap: 8px;
    padding: 2px 0;
  }

  .dest {
    flex: 0 0 auto;
    max-width: 22ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 600;
  }

  .detail {
    display: flex;
    flex-wrap: wrap;
    gap: 3px 8px;
    min-width: 0;
  }

  .entry {
    display: flex;
    align-items: baseline;
    flex-wrap: wrap;
    gap: 0 5px;
    min-width: 0;
  }

  .tag {
    padding: 0 4px;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }

  .tag.diverged {
    color: var(--color-accent-contrast);
    background: var(--color-warn);
  }

  .tag.unknown {
    background: color-mix(in srgb, var(--color-warn) 20%, transparent);
    border: 1px solid color-mix(in srgb, var(--color-warn) 50%, transparent);
  }

  .fname {
    font-weight: 600;
  }

  .was,
  .now,
  .arrow,
  .remedy {
    color: var(--color-dim);
  }

  .remedy {
    flex: 1 1 100%;
    font-style: italic;
  }
</style>
