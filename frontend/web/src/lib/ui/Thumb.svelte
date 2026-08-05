<script lang="ts">
  // Rectangular image tile with a failed-URL fallback. Avatar.svelte has the same
  // fallback pattern but is a hardcoded 50% circle built for channel avatars, so
  // this copies the pattern rather than the component.
  interface Props {
    src?: string;
    alt?: string;
    ratio?: number;
  }
  let { src = "", alt = "", ratio = 16 / 9 }: Props = $props();
  let failed = $state("");
</script>

<div class="thumb" style="aspect-ratio: {ratio}">
  {#if src && src !== failed}
    <img {src} {alt} onerror={() => (failed = src)} />
  {:else}
    <div class="fallback" aria-hidden="true"></div>
  {/if}
</div>

<style>
  .thumb {
    position: relative;
    width: 100%;
    overflow: hidden;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
  }
  .thumb img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }
  .fallback {
    width: 100%;
    height: 100%;
    background: repeating-linear-gradient(
      45deg,
      var(--color-base),
      var(--color-base) 6px,
      var(--color-surface) 6px,
      var(--color-surface) 12px
    );
  }
</style>
