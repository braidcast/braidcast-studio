<script lang="ts">
  // Shared scaffold for the not-yet-built views: the shared PageHeader over a
  // centered "coming in Phase 7.x" body. Real content lands in later phases (see
  // plan); each page file passes its own title/sub/phase. Tokens only, zero-radius.
  import PageHeader from "../PageHeader.svelte";
  let { title, sub, phase }: { title: string; sub: string; phase: string } = $props();
</script>

<div class="page">
  <PageHeader {title} {sub} />
  <div class="body">
    <p class="coming">Coming in Phase {phase}</p>
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    background: var(--color-base);
    color: var(--color-text);
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .coming {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 12px;
    letter-spacing: 0.04em;
    color: var(--color-muted);
  }
</style>
