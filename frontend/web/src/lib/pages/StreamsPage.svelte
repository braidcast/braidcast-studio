<script lang="ts">
  import StreamsTab from "../StreamsTab.svelte";
</script>

<div class="page">
  <header class="head">
    <span class="title">Streams</span>
  </header>

  <div class="body">
    <div class="pane">
      <StreamsTab />
    </div>
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
  }
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .pane {
    flex: 1;
    min-width: 0;
    overflow: auto;
    padding: 24px 28px;
  }
</style>
