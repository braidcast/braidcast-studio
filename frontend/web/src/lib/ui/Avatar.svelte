<script lang="ts">
  // Avatar with a monogram fallback: shows the image when a URL loads, else the
  // first character of `name` on a neutral tile. `size` in px. Rounded to match
  // how every streaming platform renders channel avatars.
  let { url = "", name = "", size = 24 }: { url?: string; name?: string; size?: number } = $props();
  // Track the URL that failed (not a boolean) so a later valid `url` clears the
  // fallback automatically — the render gate re-passes once `url !== failedUrl`.
  let failedUrl = $state("");
  const initial = $derived((name.trim()[0] ?? "?").toUpperCase());
</script>

{#if url && url !== failedUrl}
  <img
    class="avatar"
    src={url}
    alt={name}
    style="width:{size}px;height:{size}px"
    onerror={() => (failedUrl = url)}
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
    border-radius: 50%;
    flex: none;
  }
  .mono {
    font-family: var(--font-mono);
    color: var(--color-muted);
  }
</style>
