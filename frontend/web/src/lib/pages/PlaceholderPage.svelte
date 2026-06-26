<script lang="ts">
  // Shared scaffold for the not-yet-built views: the mock's per-view header bar
  // (title + Geist Mono sub-label) over a centered "coming in Phase 7.x" body.
  // Real content lands in later phases (see plan); each page file passes its own
  // title/sub/phase. Tokens only, zero-radius.
  let { title, sub, phase }: { title: string; sub: string; phase: string } = $props();
</script>

<div class="page">
  <header class="head">
    <span class="title">{title}</span>
    <span class="sub">{sub}</span>
  </header>
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
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: baseline;
    gap: 12px;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
    align-self: center;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
    align-self: center;
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
