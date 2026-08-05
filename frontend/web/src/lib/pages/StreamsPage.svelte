<script lang="ts">
  import StreamsTab from "$lib/settings/StreamsTab.svelte";
  import PageShell from "$lib/ui/PageShell.svelte";
</script>

<PageShell title="Streams" sub="Reusable destination credentials">

  <div class="body">
    <StreamsTab />
  </div>
</PageShell>

<style>
  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }
</style>
