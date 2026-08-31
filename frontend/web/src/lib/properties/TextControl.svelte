<script lang="ts">
  import type { ControlProps } from "$lib/properties/controls";
  import type { TextProperty } from "$lib/api/bridge";
  import { sanitizeInlineHtml } from "$lib/utils/sanitizeHtml";
  import { externalLinks } from "$lib/utils/externalUrl";
  let { prop, value, onChange }: ControlProps = $props();

  const p = $derived(prop as TextProperty);
  const str = $derived(value == null ? "" : String(value));

  // Which string becomes the note, following the Qt properties view: the value, or the
  // long description when there is no value, or the property's own description when
  // there is neither. All three are markup there, so all three are markup here.
  const infoSource = $derived(str || prop.long_description || prop.label || "");
  const infoHtml = $derived(sanitizeInlineHtml(infoSource));
  // A long description is only a tooltip while the value is the one carrying the note;
  // otherwise it IS the note and repeating it on hover says nothing.
  const infoTitle = $derived(str ? (prop.long_description ?? "") : "");

  function emit(raw: string) {
    onChange(prop.name, raw);
  }
</script>

{#if p.text_type === "info"}
  <p
    class="info"
    class:warning={p.info_type === "warning"}
    class:error={p.info_type === "error"}
    class:nowrap={p.info_word_wrap === false}
    title={infoTitle}
    use:externalLinks
  >
    {@html infoHtml}
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
    font-family: var(--font-mono);
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
  .info.nowrap {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  /* :global because the note's markup is inserted with {@html}, which scoped
     selectors do not reach. Underlined, not merely colored: the warning variant's
     text is already the accent hue, so color alone would not mark the link. */
  .info :global(a) {
    color: var(--color-accent);
    text-decoration: underline;
  }
  .info :global(a:hover) {
    color: var(--color-text);
  }
  .info :global(a:focus-visible) {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: 1px;
  }
  .info :global(code) {
    font-family: var(--font-mono);
  }
</style>
