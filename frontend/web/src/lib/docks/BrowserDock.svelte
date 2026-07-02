<script lang="ts">
  // A Dockview panel hosting an arbitrary web URL in a full-size sandboxed iframe
  // (Task 12). The reconciler supplies {url,title} from the store by dock id, so a
  // restored layout re-resolves its url even though the saved layout carries no
  // params. The mount adapter strips internal __* keys before they reach here.
  //
  // NOTE: some sites block framing via X-Frame-Options / CSP frame-ancestors (full
  // platform dashboards); chat/widget popout URLs embed fine. That caveat is
  // surfaced in the Browser Docks settings manager, not per-panel.
  import EmptyState from "../EmptyState.svelte";

  interface Props {
    url: string;
    title: string;
  }
  let { url, title }: Props = $props();
</script>

{#if url}
  <iframe
    class="frame"
    src={url}
    {title}
    sandbox="allow-scripts allow-same-origin allow-forms allow-popups"
  ></iframe>
{:else}
  <div class="empty-wrap">
    <EmptyState compact title="No URL set for this browser dock." />
  </div>
{/if}

<style>
  .frame {
    width: 100%;
    height: 100%;
    border: 0;
    border-radius: 0;
    display: block;
    background: var(--color-base);
  }
  .empty-wrap {
    width: 100%;
    height: 100%;
    background: var(--color-base);
  }
</style>
