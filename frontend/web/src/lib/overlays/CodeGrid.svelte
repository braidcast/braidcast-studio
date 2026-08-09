<script lang="ts" module>
  /** Which document a change came from, so the page needs one handler rather than three. */
  export type CodePart = "html" | "css" | "js";
</script>

<script lang="ts">
  // The Advanced pane's three code editors in their fixed arrangement: HTML across the
  // top, CSS and JS sharing the row below. Both states of the pane render the same grid —
  // a stock widget's read-only view of the shipped template, and a forked widget's own
  // editable copy — so the layout, the kickers and the labelling live here once and the
  // page passes only the source and whether it may be edited.
  import CodePane from "$lib/overlays/CodePane.svelte";

  let {
    html,
    css,
    js,
    readonly = false,
    onChange,
  }: {
    html: string;
    css: string;
    js: string;
    readonly?: boolean;
    onChange?: (part: CodePart, value: string) => void;
  } = $props();

  // One row per cell rather than three near-identical blocks below, so the CodeMirror
  // language, the kicker and the accessible name stay in step for every part.
  const CELLS: { part: CodePart; kicker: string; lang: "html" | "css" | "javascript" }[] = [
    { part: "html", kicker: "HTML", lang: "html" },
    { part: "css", kicker: "CSS", lang: "css" },
    { part: "js", kicker: "JS", lang: "javascript" },
  ];

  const sources = $derived<Record<CodePart, string>>({ html, css, js });
</script>

<div class="code-grid">
  {#each CELLS as cell (cell.part)}
    <div class="code-cell">
      <span class="cell-kicker">{cell.kicker}</span>
      <CodePane
        value={sources[cell.part]}
        lang={cell.lang}
        label={readonly ? `${cell.kicker} (built-in template, read-only)` : cell.kicker}
        {readonly}
        onChange={(v) => onChange?.(cell.part, v)}
      />
    </div>
  {/each}
</div>

<style>
  .code-grid {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: grid;
    grid-template-columns: 1fr 1fr;
    grid-template-rows: 1fr 1fr;
    gap: 12px;
  }
  /* HTML spans the full top row; CSS + JS share the bottom row. */
  .code-cell:first-child {
    grid-column: 1 / -1;
  }
  .code-cell {
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-height: 0;
    min-width: 0;
  }
  .cell-kicker {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .code-cell :global(.code-host) {
    flex: 1;
    min-height: 0;
  }
</style>
