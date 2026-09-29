<script lang="ts">
  import Icon from "$lib/ui/Icon.svelte";

  let { unseen, onclick }: { unseen: number; onclick: () => void } = $props();
</script>

<!-- Positions itself against the nearest positioned ancestor; a dock lifts it clear of
     anything pinned below the feed by setting --feedjump-bottom on that ancestor. -->
<button
  class="jump"
  aria-label={unseen > 0 ? `Jump to latest, ${unseen} new` : undefined}
  {onclick}
  ><Icon name="jump-down" size={11} />
  {unseen > 0 ? `${unseen} new` : "Jump to latest"}</button
>

<style>
  .jump {
    position: absolute;
    left: 50%;
    transform: translateX(-50%);
    bottom: var(--feedjump-bottom, 8px);
    z-index: 2;
    display: flex;
    align-items: center;
    gap: 5px;
    padding: 4px 12px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-accent-ink);
    background: var(--color-accent);
    border: 0;
    cursor: pointer;
  }
</style>
