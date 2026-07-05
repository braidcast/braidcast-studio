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

  // Option contract: enum/labelset `options` are {value,label} objects (value submitted,
  // label shown verbatim). Coerce a bare string to {value, label} so a mixed/legacy
  // provider never renders "[object Object]".
  type Opt = { value: string; label: string };
  function normOpt(o: unknown): Opt {
    return typeof o === "string" ? { value: o, label: o } : (o as Opt);
  }
  const opts = $derived((field.options ?? []).map(normOpt));

  function basename(p: string): string {
    return p.split(/[\\/]/).pop() || p;
  }
  function toggleLabel(v: string): void {
    onChange(arr.includes(v) ? arr.filter((o) => o !== v) : [...arr, v]);
  }

  // Render a local OS path as a CEF-loadable URL: normalize Windows backslashes to
  // forward slashes, ensure a leading slash, and encode spaces/specials. Windows
  // "C:\a b.png" -> file:///C:/a%20b.png; POSIX "/home/x.png" -> file:///home/x.png.
  function toFileUrl(p: string): string {
    const norm = p.replace(/\\/g, "/");
    return encodeURI("file://" + (norm.startsWith("/") ? norm : "/" + norm));
  }

  // Reset the load-failure flag whenever a different path is selected so a new pick
  // re-attempts the preview after a prior file failed to load.
  let imgError = $state(false);
  $effect(() => {
    void str;
    imgError = false;
  });

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

  function clearImage(e: MouseEvent): void {
    e.stopPropagation();
    // Empty string -> isEmptyVal() true in the modal, so the field is omitted from
    // the streamMeta push rather than sent as a blank thumbnail.
    onChange("");
  }

  function onDrop(e: DragEvent): void {
    e.preventDefault();
    // CEF exposes the OS path on dropped files via the non-standard File.path. If it
    // is absent (sandboxed / plain browser), drag-drop is a no-op and click-to-pick
    // remains the path; we can't synthesize a local path from a sandboxed File.
    const f = e.dataTransfer?.files?.[0] as (File & { path?: string }) | undefined;
    if (f?.path) {
      onChange(f.path);
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
  {#if str}
    <div class="thumb has">
      {#if imgError}
        <span class="fname">{basename(str)}</span>
      {:else}
        <img class="preview" src={toFileUrl(str)} alt={basename(str)} onerror={() => (imgError = true)} />
      {/if}
      <button class="thumb-x" title="Remove" aria-label="Remove image" onclick={clearImage}>×</button>
    </div>
    <div class="thumb-meta">{basename(str)}</div>
  {:else}
    <button
      class="thumb"
      ondragover={(e) => e.preventDefault()}
      ondrop={onDrop}
      onclick={() => void pickImage()}
    >
      <span>drop / pick</span>
      <small>PNG/JPG, ≤2 MB</small>
    </button>
  {/if}
{:else if field.type === "enum"}
  <select class="inp" class:narrow value={str} onchange={(e) => onChange(e.currentTarget.value)}>
    <option value="">—</option>
    {#each opts as opt (opt.value)}
      <option value={opt.value}>{opt.label}</option>
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
    {#each opts as opt (opt.value)}
      <label class="lscheck">
        <input type="checkbox" checked={arr.includes(opt.value)} onchange={() => toggleLabel(opt.value)} />
        {opt.label}
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
    width: 150px;
    height: 84px;
    border: var(--border-weight) dashed var(--color-border);
    color: var(--color-muted);
    font-size: 10px;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 2px;
    background: var(--color-base);
    cursor: pointer;
    overflow: hidden;
    text-align: center;
    padding: 4px;
    word-break: break-all;
  }
  .thumb small {
    font-size: 9px;
    color: var(--color-muted);
  }
  button.thumb:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .thumb.has {
    position: relative;
    border-style: solid;
    padding: 0;
    cursor: default;
  }
  .preview {
    width: 100%;
    height: 100%;
    object-fit: cover;
    display: block;
  }
  .fname {
    font-size: 10px;
    color: var(--color-muted);
    padding: 4px;
  }
  .thumb-x {
    position: absolute;
    top: 0;
    right: 0;
    width: 18px;
    height: 18px;
    line-height: 1;
    padding: 0;
    font-size: 14px;
    background: var(--color-base);
    color: var(--color-text);
    border: none;
    border-left: var(--border-weight) solid var(--color-border);
    border-bottom: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  .thumb-x:hover {
    color: var(--color-accent);
  }
  .thumb-meta {
    font-size: 9px;
    color: var(--color-muted);
    margin-top: 3px;
    word-break: break-all;
  }
</style>
