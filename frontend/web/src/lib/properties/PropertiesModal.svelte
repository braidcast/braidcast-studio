<script lang="ts">
  import Modal from "$lib/ui/Modal.svelte";
  import PropertyForm from "$lib/properties/PropertyForm.svelte";
  import type { PropertyKind } from "$lib/api/bridge";

  // Shared source Properties dialog shell: Modal + PropertyForm + a
  // Restore Defaults / Cancel / OK footer. Edits still live-apply as you type
  // (PropertyForm's own debounce); Cancel/Esc/X revert to the settings snapshot
  // PropertyForm captured when the dialog opened, mirroring the old Qt
  // OBSBasicProperties (Cancel restored oldSettings; Restore Defaults cleared +
  // reloaded). OK just closes — the live-applied edits are already committed.
  interface Props {
    kind: PropertyKind;
    ref: string;
    title: string;
    onClose: () => void;
    width?: number;
    maxHeight?: string;
  }
  let { kind, ref, title, onClose, width = 560, maxHeight = "80vh" }: Props = $props();

  let form: PropertyForm | undefined = $state();
  // Guards against a double revert if both the footer Cancel and Modal's onClose
  // (Esc/X) somehow fire for the same dismissal.
  let closing = false;

  async function discardAndClose() {
    if (closing) {
      return;
    }
    closing = true;
    try {
      await form?.revert();
    } finally {
      onClose();
    }
  }

  function keepAndClose() {
    onClose();
  }
</script>

<Modal {title} onClose={discardAndClose} {width} {maxHeight}>
  <PropertyForm bind:this={form} {kind} {ref} />

  {#snippet footer()}
    <button class="ghost" onclick={() => void form?.restoreDefaults()}>Restore Defaults</button>
    <span class="spacer"></span>
    <button class="ghost" onclick={discardAndClose}>Cancel</button>
    <button class="accent" onclick={keepAndClose}>OK</button>
  {/snippet}
</Modal>

<style>
  .spacer {
    flex: 1 1 auto;
  }
</style>
