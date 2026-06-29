<script lang="ts">
  import { obs, type OAuthProviderField } from "./bridge";
  import GoLiveTagsInput from "./GoLiveTagsInput.svelte";
  import GoLiveCategoryInput from "./GoLiveCategoryInput.svelte";

  // The single descriptor-driven field widget. Dispatch is by `field.type` only —
  // text / textarea / tags / category / image / enum / bool / labelset — so the
  // shared, simple, and advanced sections all render through here and a new field
  // type is added in ONE place. `ghostText` (inherit cue) and `accent` (override
  // styling) are passed by the caller; the widget owns the cue visuals per type.
  interface Props {
    field: OAuthProviderField;
    value: unknown;
    onChange: (v: unknown) => void;
    /** Required for `category` typeahead; ignored by other types. */
    providerId?: string;
    /** Inherit cue: when set and the value is empty, show "↳ <ghost>" / ghost line. */
    ghostText?: string;
    /** Override styling (amber border / accent chips) when the field is filled. */
    accent?: boolean;
    /** Constrain width (used by advanced enums). */
    narrow?: boolean;
  }
  let { field, value, onChange, providerId = "", ghostText = "", accent = false, narrow = false }: Props = $props();

  const str = $derived(typeof value === "string" ? value : "");
  const arr = $derived(Array.isArray(value) ? (value as string[]) : []);
  const cat = $derived(value && typeof value === "object" ? (value as { id: string; name: string }) : null);
  const bool = $derived(value === true);
  const showGhost = $derived(ghostText !== "" && str === "");

  function titleCase(s: string): string {
    return s.length ? s.charAt(0).toUpperCase() + s.slice(1) : s;
  }
  function basename(p: string): string {
    return p.split(/[\\/]/).pop() || p;
  }
  function toggleLabel(opt: string): void {
    onChange(arr.includes(opt) ? arr.filter((o) => o !== opt) : [...arr, opt]);
  }

  async function pickImage(): Promise<void> {
    try {
      const r = await obs.call("dialog.openFile", { mode: "open", filter: "Image Files (*.png *.jpg *.jpeg *.bmp)" });
      if (r.path) {
        onChange(r.path);
      }
    } catch {
      // Cancelled or unavailable: leave the field as-is.
    }
  }
</script>

{#if field.type === "tags"}
  <GoLiveTagsInput values={arr} {ghostText} {accent} onChange={(v) => onChange(v)} />
{:else if field.type === "category"}
  <GoLiveCategoryInput {providerId} value={cat} onChange={(v) => onChange(v)} />
{:else if field.type === "textarea"}
  <textarea
    class="inp"
    class:ghost={showGhost}
    class:ovr={accent}
    class:narrow
    rows="2"
    placeholder={ghostText ? "↳ " + ghostText : ""}
    value={str}
    oninput={(e) => onChange(e.currentTarget.value)}
  ></textarea>
{:else if field.type === "image"}
  <button class="thumb" onclick={() => void pickImage()}>
    {str ? basename(str) : "drop / pick"}
  </button>
{:else if field.type === "enum"}
  <select class="inp" class:narrow value={str} onchange={(e) => onChange(e.currentTarget.value)}>
    <option value="">—</option>
    {#each field.options ?? [] as opt (opt)}
      <option value={opt}>{titleCase(opt)}</option>
    {/each}
  </select>
{:else if field.type === "bool"}
  <button
    class="tog"
    class:on={bool}
    aria-label={field.label}
    aria-pressed={bool}
    onclick={() => onChange(!bool)}
  ><i></i></button>
{:else if field.type === "labelset"}
  <div class="labelset">
    {#each field.options ?? [] as opt (opt)}
      <label class="lscheck">
        <input type="checkbox" checked={arr.includes(opt)} onchange={() => toggleLabel(opt)} />
        {titleCase(opt)}
      </label>
    {/each}
  </div>
{:else}
  <input
    class="inp"
    class:ghost={showGhost}
    class:ovr={accent}
    class:narrow
    type="text"
    placeholder={ghostText ? "↳ " + ghostText : ""}
    value={str}
    oninput={(e) => onChange(e.currentTarget.value)}
  />
{/if}

<style>
  .inp {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    box-sizing: border-box;
    font: inherit;
    font-size: 12px;
  }
  .inp:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  textarea.inp {
    resize: vertical;
  }
  .inp.ovr {
    border-color: var(--color-accent);
  }
  .inp.ghost::placeholder {
    color: var(--color-muted);
    font-style: italic;
  }
  .inp.narrow {
    max-width: 200px;
  }
  .tog {
    width: 30px;
    height: 16px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    position: relative;
    display: inline-block;
    padding: 0;
    cursor: pointer;
    flex: 0 0 auto;
  }
  .tog.on {
    background: var(--color-accent);
  }
  .tog i {
    position: absolute;
    top: 1px;
    left: 1px;
    width: 12px;
    height: 12px;
    background: var(--color-muted);
  }
  .tog.on i {
    left: 15px;
    background: var(--color-accent-ink);
  }
  .labelset {
    display: flex;
    flex-wrap: wrap;
    gap: 8px 14px;
  }
  .lscheck {
    display: flex;
    align-items: center;
    gap: 5px;
    font-size: 12px;
    color: var(--color-text);
    cursor: pointer;
  }
  .thumb {
    width: 104px;
    height: 58px;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-muted);
    font-size: 10px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--color-base);
    cursor: pointer;
    overflow: hidden;
    text-align: center;
    padding: 4px;
    word-break: break-all;
  }
  .thumb:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
</style>
