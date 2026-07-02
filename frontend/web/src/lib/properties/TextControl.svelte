<script lang="ts">
  import type { ControlProps } from "./controls";
  import type { TextProperty } from "../bridge";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as TextProperty);
  const str = $derived(value == null ? "" : String(value));

  function emit(raw: string) {
    onChange(prop.name, raw);
  }
</script>

{#if p.text_type === "info"}
  <p class="info" class:warning={p.info_type === "warning"} class:error={p.info_type === "error"}>
    {str || prop.label || ""}
  </p>
{:else if p.text_type === "multiline"}
  <textarea
    class:mono={p.monospace}
    rows="4"
    value={str}
    disabled={!prop.enabled}
    title={prop.long_description ?? ""}
    oninput={(e) => emit((e.currentTarget as HTMLTextAreaElement).value)}
  ></textarea>
{:else}
  <input
    type={p.text_type === "password" ? "password" : "text"}
    class:mono={p.monospace}
    value={str}
    disabled={!prop.enabled}
    title={prop.long_description ?? ""}
    oninput={(e) => emit((e.currentTarget as HTMLInputElement).value)}
  />
{/if}

<style>
  input,
  textarea {
    width: 100%;
  }
  .mono {
    font-family: var(--mono, ui-monospace, monospace);
  }
  textarea {
    resize: vertical;
  }
  .info {
    margin: 0;
    color: var(--color-muted);
    font-size: 12px;
    grid-column: 1 / -1;
  }
  .info.warning {
    color: var(--color-warn);
  }
  .info.error {
    color: var(--color-live);
  }
</style>
