<script lang="ts">
  import StreamsTab from "$lib/settings/StreamsTab.svelte";
  import PageHeader from "$lib/ui/PageHeader.svelte";
</script>

<div class="page">
  <PageHeader title="Streams" sub="reusable destination credentials" />

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
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
</style>
