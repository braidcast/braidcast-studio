<script lang="ts">
  // Avatar with a monogram fallback: shows the image when a URL loads, else the
  // first character of `name` on a neutral tile. `size` in px. Square, zero-radius.
  let { url = "", name = "", size = 24 }: { url?: string; name?: string; size?: number } = $props();
  let failed = $state(false);
  const initial = $derived((name.trim()[0] ?? "?").toUpperCase());
</script>

{#if url && !failed}
  <img
    class="avatar"
    src={url}
    alt={name}
    style="width:{size}px;height:{size}px"
    onerror={() => (failed = true)}
  />
{:else}
  <span class="avatar mono" style="width:{size}px;height:{size}px;font-size:{Math.round(size * 0.45)}px">
    {initial}
  </span>
{/if}

<style>
  .avatar {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    object-fit: cover;
    background: var(--color-surface-2);
    border: 1px solid var(--color-border);
    flex: none;
  }
  .mono {
    font-family: var(--font-mono);
    color: var(--color-muted, #888);
  }
</style>
