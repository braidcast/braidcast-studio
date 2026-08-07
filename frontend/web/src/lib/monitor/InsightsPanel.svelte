<script lang="ts">
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import { ADVISOR_RULES, SEVERITY, SEVERITY_ORDER, type AdvisorSeverity } from "$lib/monitor/advisorRules";
  import { advisorStore } from "$lib/monitor/advisorStore.svelte";

  // Read-only insights over the loaded scene collection. The store owns the sweep and
  // its cadence (mount + scene-graph events + Rescan, never the 1 Hz stats tick); this
  // adds only the presentation. Subscribed while mounted, like every other panel on
  // this page, so navigating away releases the subscription.
  const rows = $derived(advisorStore.rows);
  $effect(() => advisorStore.subscribe());

  // Every reason this answer is short of complete: a check that could not run at all,
  // or one that ran without covering every subject. This panel must never read as a
  // verified all-clear when either happened, so the state rides in the head strip,
  // each shortfall states its reason above the list, and the no-findings copy is
  // worded differently the moment anything is missing.
  const skipped = $derived(advisorStore.skipped);
  // A partial check DID run, so it still counts toward coverage.
  const notRun = $derived(skipped.filter((s) => s.partial === null).length);
  const coverage = $derived(
    skipped.length === 0
      ? null
      : notRun > 0
        ? `${ADVISOR_RULES.length - notRun}/${ADVISOR_RULES.length} CHECKS RAN`
        : "PARTIAL COVERAGE",
  );
  const emptyTitle = $derived(skipped.length === 0 ? "Nothing to flag" : "Nothing found by the checks that ran");
  const emptySub = $derived(
    skipped.length === 0
      ? "No breakage, risk, or waste found in this scene collection."
      : "Some checks could not run or could not see every source, so this is not an all-clear.",
  );

  // "1 BREAKAGE · 3 WASTE" — bands in SEVERITY rank order, empty ones omitted.
  const tally = $derived.by<{ band: AdvisorSeverity; label: string; color: string; n: number }[]>(() => {
    const counts = new Map<AdvisorSeverity, number>();
    for (const r of rows) {
      counts.set(r.rule.severity, (counts.get(r.rule.severity) ?? 0) + 1);
    }
    return SEVERITY_ORDER.filter((band) => counts.has(band)).map((band) => ({
      band,
      label: SEVERITY[band].label,
      color: SEVERITY[band].color,
      n: counts.get(band) ?? 0,
    }));
  });
</script>

<div class="insights">
  <div class="ihead">
    <span class="itally">
      {#if tally.length === 0}
        <span class="idim">NO FINDINGS</span>
      {:else}
        {#each tally as t, i (t.band)}
          {#if i > 0}<span class="idim">·</span>{/if}
          <span style:color={t.color}>{t.n} {t.label}</span>
        {/each}
      {/if}
      {#if coverage}
        <span class="idim">·</span>
        <span class="iskipcount">{coverage}</span>
      {/if}
    </span>
    <button class="rescan" onclick={() => advisorStore.rescan()} disabled={advisorStore.scanning}>
      {advisorStore.scanning ? "Scanning…" : "Rescan"}
    </button>
  </div>

  <div class="ibody">
    {#if advisorStore.error}
      <p class="ierror">{advisorStore.error}</p>
    {:else if !advisorStore.scanned}
      <p class="idim">Reading the scene collection…</p>
    {:else}
      {#each skipped as s (s.rule.id)}
        <p class="iskip">
          {#if s.partial}
            <span class="iskip-k">Partly checked</span>
            · {s.rule.title} — {s.partial.unread}
            {s.partial.unread === 1 ? "source was" : "sources were"} left out: {s.reason}.
          {:else}
            <span class="iskip-k">Not checked</span> · {s.rule.title} — {s.reason}.
          {/if}
        </p>
      {/each}

      {#if rows.length === 0}
        <div class="iempty">
          <EmptyState title={emptyTitle} sub={emptySub} />
        </div>
      {/if}

      {#each rows as row (row.id)}
        {@const sev = SEVERITY[row.rule.severity]}
        <button class="cv-ci irow" title={row.rule.title} onclick={() => row.rule.link?.go()}>
          <span class="cv-ci__dot" style:background={sev.color}></span>
          <span class="cv-ci__body">
            <span class="cv-ci__name">{row.finding.title}</span>
            <span class="cv-ci__sub">{row.finding.cost}</span>
            {#if row.finding.detail}
              <span class="cv-ci__sub idetail">{row.finding.detail}</span>
            {/if}
          </span>
          {#if row.rule.link}
            <span class="igo">{row.rule.link.label}</span>
          {/if}
          <span class="cv-ci__badge" style:color={sev.color} style:border-color={sev.color}>{sev.label}</span>
        </button>
      {/each}
    {/if}
  </div>
</div>

<style>
  /* Boxed like the PER-OUTPUT STREAMS table above it so the fourth section reads as
     part of the same page rather than a bolted-on panel. */
  .insights {
    border: var(--border-weight) solid var(--color-border);
  }
  .ihead {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 7px 16px;
    background: var(--color-surface-2);
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }
  .itally {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
    overflow: hidden;
  }
  .rescan {
    flex: 0 0 auto;
    padding: 3px 9px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    cursor: pointer;
  }
  .rescan:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .rescan:disabled {
    cursor: default;
    opacity: 0.5;
  }
  .ibody {
    padding: 6px;
    border-top: var(--border-weight) solid var(--color-border-2);
    background: var(--color-surface);
  }
  .idim {
    margin: 0;
    padding: 12px 10px;
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 10.5px;
  }
  /* The tally's separators/placeholder sit inline in the head strip, which already
     supplies the mono type — so only the color applies there. */
  .ihead .idim {
    padding: 0;
    font-size: inherit;
  }
  /* A skipped check is not a finding, so it gets no severity dot and no badge — it
     is a caveat on the result, dimmer than a row and set apart by a dashed rule. */
  .iskip {
    margin: 0;
    padding: 8px 10px;
    border-bottom: var(--border-weight) dashed var(--color-border);
    color: var(--color-muted);
    font-size: 11px;
    line-height: 1.5;
  }
  .iskip-k {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-dim);
  }
  .iskipcount {
    color: var(--color-dim);
  }
  /* Same breathing room the per-output table gives its own empty state. */
  .iempty {
    padding: 16px 10px;
  }
  .ierror {
    margin: 0;
    padding: 12px 10px;
    color: var(--color-live);
    font-size: 11px;
  }
  .igo {
    flex: 0 0 auto;
    align-self: center;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .irow:hover .igo {
    color: var(--color-accent);
  }
  /* The shared list row is built for one-line entries and ellipsizes its subline.
     An advisor row's payload IS that sentence, so it wraps here and the row grows
     with it; the dot and badge pin to the first line instead of centering. */
  .irow {
    align-items: flex-start;
  }
  .insights :global(.cv-ci__sub) {
    white-space: normal;
    overflow: visible;
    line-height: 1.5;
  }
  .insights :global(.cv-ci__dot) {
    margin-top: 5px;
  }
  .insights :global(.cv-ci__badge) {
    margin-top: 2px;
  }
  .idetail {
    color: var(--color-dim);
  }
</style>
