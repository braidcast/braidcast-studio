<script lang="ts">
  import { sessionsStore } from "$lib/stores/sessionsStore.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import HistoryCard from "./HistoryCard.svelte";

  sessionsStore.start();

  function onOpen(_id: string): void {
    // The detail view lands in the next task; the card is already the affordance.
  }
</script>

<div class="list">
  {#if sessionsStore.error}
    <!-- A database that could not be opened has to say so with a reason. Falling
         through to the empty state would present it as "you have never streamed". -->
    <div class="err">History is unavailable: {sessionsStore.error}</div>
  {:else if sessionsStore.loaded && sessionsStore.sessions.length === 0}
    <EmptyState
      compact
      title="No streams yet"
      sub="Broadcasts you run will be recorded here automatically."
    >
      {#snippet icon()}
        <Icon name="film" size={22} />
      {/snippet}
    </EmptyState>
  {:else}
    {#each sessionsStore.sessions as s (s.id)}
      <HistoryCard session={s} {onOpen} />
    {/each}
  {/if}
</div>

<style>
  .list {
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .err {
    padding: 16px;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-live);
  }
</style>
