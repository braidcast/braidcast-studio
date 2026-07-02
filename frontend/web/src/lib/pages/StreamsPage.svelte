<script lang="ts">
  import StreamsTab from "../StreamsTab.svelte";
</script>

<div class="page">
  <header class="head">
    <div class="head-titles">
      <span class="title">Streams</span>
      <span class="sub">reusable destination credentials</span>
    </div>
  </header>

  <div class="body">
    <StreamsTab />
  </div>
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
  .head {
    flex: 0 0 auto;
    height: 58px;
    display: flex;
    align-items: center;
    padding: 0 24px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .head-titles {
    display: flex;
    align-items: baseline;
    gap: 12px;
    min-width: 0;
  }
  .title {
    font-family: var(--font-ui);
    font-size: 16px;
    font-weight: 600;
    letter-spacing: -0.01em;
  }
  .sub {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
</style>
