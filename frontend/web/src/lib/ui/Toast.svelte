<script lang="ts">
  import { toast } from "$lib/stores/toastStore.svelte";
</script>

{#if toast.current}
  {#key toast.current.seq}
    <div class="toast" role="status" aria-live="polite" title={toast.current.title}>
      {toast.current.message}
    </div>
  {/key}
{/if}

<style>
  .toast {
    position: fixed;
    left: 50%;
    bottom: 24px;
    transform: translateX(-50%);
    z-index: 300;
    max-width: min(560px, calc(100vw - 48px));
    padding: 9px 14px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    box-shadow: 0 4px 18px rgba(0, 0, 0, 0.4);
    animation: toast-in 140ms ease-out;
  }
  @keyframes toast-in {
    from {
      opacity: 0;
      transform: translate(-50%, 8px);
    }
    to {
      opacity: 1;
      transform: translate(-50%, 0);
    }
  }
</style>
