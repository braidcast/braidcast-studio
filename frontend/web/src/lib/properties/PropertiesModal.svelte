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

<Modal
  {title}
  onClose={discardAndClose}
  {width}
  {maxHeight}
  actions={[{ label: "Restore Defaults", onclick: () => void form?.restoreDefaults() }]}
  cancel={{ label: "Cancel", onclick: discardAndClose }}
  confirm={{ label: "OK", onclick: keepAndClose }}
>
  <PropertyForm bind:this={form} {kind} {ref} />
</Modal>
