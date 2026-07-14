<script lang="ts">
  import { obs } from "$lib/api/bridge";
  import type { ControlProps } from "$lib/properties/controls";
  import type { PathProperty } from "$lib/api/bridge";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as PathProperty);
  const str = $derived(value == null ? "" : String(value));

  async function browse() {
    const mode = p.path_type === "directory" ? "directory" : p.path_type === "file_save" ? "save" : "open";
    const { path } = await obs.call("dialog.openFile", {
      mode,
      filter: p.filter ?? undefined,
      defaultPath: str || (p.default_path ?? undefined),
    });
    if (path != null) onChange(prop.name, path);
  }
</script>

<div class="path" title={prop.long_description ?? ""}>
  <input
    type="text"
    value={str}
    disabled={!prop.enabled}
    placeholder={p.path_type === "directory" ? "directory path" : "file path"}
    oninput={(e) => onChange(prop.name, (e.currentTarget as HTMLInputElement).value)}
  />
  <button type="button" class="browse" disabled={!prop.enabled} onclick={() => void browse()}>
    Browse…
  </button>
</div>

<style>
  .path {
    display: flex;
    gap: 6px;
  }
  input {
    flex: 1;
    min-width: 0;
  }
  .browse {
    white-space: nowrap;
  }
</style>
