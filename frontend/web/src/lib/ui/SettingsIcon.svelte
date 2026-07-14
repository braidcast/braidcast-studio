<script lang="ts">
  // One 16px monochrome line icon per settings category, keyed by id. Inherits
  // color via currentColor (grey when idle, amber when the row is active).
  let { id }: { id: string } = $props();
</script>

<svg viewBox="0 0 24 24" width="16" height="16" aria-hidden="true">
  {#if id === "canvases"}
    <rect x="3" y="3" width="13" height="13" />
    <rect x="8" y="8" width="13" height="13" />
  {:else if id === "streams"}
    <circle cx="12" cy="12" r="2" />
    <path d="M7 7 a7 7 0 0 0 0 10" />
    <path d="M17 7 a7 7 0 0 1 0 10" />
  {:else if id === "outputs"}
    <circle cx="6" cy="12" r="2" />
    <circle cx="18" cy="6" r="2" />
    <circle cx="18" cy="18" r="2" />
    <line x1="8" y1="11" x2="16" y2="7" />
    <line x1="8" y1="13" x2="16" y2="17" />
  {:else if id === "audio"}
    <path d="M4 9 V15 H8 L13 19 V5 L8 9 Z" />
    <path d="M17 8 a5 5 0 0 1 0 8" />
  {:else if id === "mcp"}
    <rect x="7" y="7" width="10" height="10" />
    <line x1="10" y1="3" x2="10" y2="7" />
    <line x1="14" y1="3" x2="14" y2="7" />
    <line x1="10" y1="17" x2="10" y2="21" />
    <line x1="14" y1="17" x2="14" y2="21" />
    <line x1="3" y1="10" x2="7" y2="10" />
    <line x1="3" y1="14" x2="7" y2="14" />
    <line x1="17" y1="10" x2="21" y2="10" />
    <line x1="17" y1="14" x2="21" y2="14" />
  {:else if id === "hotkeys"}
    <rect x="2" y="6" width="20" height="12" />
    <line x1="6" y1="10" x2="6" y2="10.1" />
    <line x1="10" y1="10" x2="10" y2="10.1" />
    <line x1="14" y1="10" x2="14" y2="10.1" />
    <line x1="18" y1="10" x2="18" y2="10.1" />
    <line x1="8" y1="14" x2="16" y2="14" />
  {/if}
</svg>

<style>
  svg {
    flex: 0 0 auto;
    fill: none;
    stroke: currentColor;
    stroke-width: 1.5;
  }
</style>
