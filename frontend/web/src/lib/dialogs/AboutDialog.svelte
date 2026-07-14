<script lang="ts">
  import Modal from "$lib/ui/Modal.svelte";
  import { obs } from "$lib/api/bridge";

  interface Props {
    onClose: () => void;
  }
  let { onClose }: Props = $props();

  // Modal.svelte owns the preview suspension for its whole lifetime; this dialog
  // must not self-suspend on top of it (a second, separately-released suspension
  // would race the modal's on close and leave the preview stuck hidden).

  // libobs version, loaded on open; "…" until resolved.
  let version = $state("…");

  $effect(() => {
    obs
      .call("getVersion")
      .then((v) => (version = v || "unknown"))
      .catch((e) => (version = "error: " + (e as Error).message));
  });
</script>

<Modal title="About" {onClose} width={380}>
  <div class="wordmark">Braidcast</div>
  <p class="tagline">Stream to every platform at once. Built on OBS Studio.</p>

  <dl class="facts">
    <div class="fact">
      <dt>Engine</dt>
      <dd>libobs {version}</dd>
    </div>
    <div class="fact">
      <dt>License</dt>
      <dd>Licensed under GPLv2+.</dd>
    </div>
    <div class="fact">
      <dt>Upstream</dt>
      <dd>A fork of OBS Studio (obsproject.com).</dd>
    </div>
  </dl>

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .wordmark {
    font-size: 16px;
    font-weight: 700;
    letter-spacing: var(--letter-spacing);
    color: var(--color-text);
  }
  .tagline {
    margin: 4px 0 16px;
    font-size: 11px;
    color: var(--color-muted);
  }

  .facts {
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .fact {
    display: flex;
    flex-direction: column;
    gap: 2px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 7px 8px;
  }
  .fact dt {
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
  }
  .fact dd {
    margin: 0;
    font-size: 11px;
    color: var(--color-text);
  }
</style>
