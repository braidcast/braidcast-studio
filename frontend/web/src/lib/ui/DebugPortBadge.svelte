<script lang="ts">
  import Icon from "$lib/ui/Icon.svelte";
  import { diagnosticsStore } from "$lib/stores/diagnosticsStore.svelte";
  import { devToolsExposureLabel } from "$lib/utils/devToolsExposure";

  // Mandatory sign that the CEF remote-debugging port is listening, carried by the
  // title bar so it is on every page and in every detached dock window without
  // opening Settings.
  //
  // Deliberately not a control. The port comes from CefSettings at CefInitialize and
  // cannot be closed from here, so this is a static span: a button, or a dismiss X,
  // would promise a shutdown the app cannot perform, and a warning that can be
  // dismissed is exactly the failure this exists to prevent.
  //
  // Start is idempotent. App.svelte already starts the store in the main window, but a
  // detached dock is its own CEF browser with its own module instances and starts nothing.
  $effect(() => {
    diagnosticsStore.start();
  });

  // 0 means closed, and the value is fixed for the session, so this goes false->true
  // at most once and never back -- there is no state in which it can flicker. Until the
  // seeding diagnostics.get resolves it reads 0 and nothing is drawn, which is the side
  // to fail on: a warning that arrives a round-trip late is recoverable, one that
  // appears falsely teaches the operator to ignore it.
  const port = $derived(diagnosticsStore.devToolsPort);

  // A rejected seed is a different case, and silence does not cover it: refresh() runs
  // exactly once (start() is #started-guarded and nothing else calls it), so `error`
  // never clears and the port never arrives. Drawing nothing would then be
  // indistinguishable from "closed" for the whole session, in every window -- each
  // detached dock seeds independently and gets its own chance to fail. Say unknown
  // instead. That reasoning applies only once the read has settled; the pre-seed window
  // is still the recoverable one and still draws nothing.
  const unknown = $derived(diagnosticsStore.loaded && diagnosticsStore.error !== null);

  const announcement = $derived(
    port > 0 ? devToolsExposureLabel(port) : unknown ? "Remote debugging port status unknown." : "",
  );
</script>

<!-- The live region is mounted unconditionally and starts empty, because the badge
     materialises a round-trip after mount and a role="status" created in the same
     mutation as its text is generally not announced. Same shape as StaleNotice. The
     visible span keeps `title` for pointer users but carries no role: it is not
     focusable, so a tooltip alone never reaches a keyboard or screen-reader user. -->
<span class="sr-only" role="status" aria-live="polite">{announcement}</span>

{#if port > 0}
  <span class="badge" title={devToolsExposureLabel(port)}>
    <span aria-hidden="true"><Icon name="warn" size={11} /></span>
    <span class="text">DEBUG PORT {port}</span>
  </span>
{:else if unknown}
  <span class="badge unknown" title="Remote debugging port status unknown.">
    <span class="text">DEBUG PORT ?</span>
  </span>
{/if}

<style>
  .badge {
    display: flex;
    align-items: center;
    align-self: center;
    gap: 5px;
    flex: 0 1 auto;
    min-width: 0;
    margin-left: 4px;
    padding: 3px 7px;
    /* Filled, not tinted. The tinted form put --color-warn text on a color-mix of itself
       with the rail, which lands at 1.39:1 once --color-rail is the light mode's #dededf
       -- and --color-warn is not themeable, so no mode could correct it. Ink on the fill
       is 10.03:1 in both modes; the ink border carries the 3:1 boundary in light mode,
       where the fill itself is only 1.34:1 against the rail. */
    color: var(--color-warn-ink);
    background: var(--color-warn);
    border: var(--border-weight) solid var(--color-warn-ink);
    /* The title bar is one big -webkit-app-region: drag surface, which swallows the
       hover CEF needs to raise the tooltip carrying the exposure text. */
    -webkit-app-region: no-drag;
    cursor: default;
  }
  /* Unknown is not an alert: it withholds a claim rather than making one, so it takes
     the dim neutral and drops the fill and the warn triangle entirely. */
  .badge.unknown {
    color: var(--color-dim);
    background: transparent;
    border-color: color-mix(in srgb, var(--color-dim) 55%, transparent);
  }
  .text {
    font-family: var(--font-mono, monospace);
    font-size: 10px;
    font-weight: 600;
    letter-spacing: 0.1em;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
</style>
