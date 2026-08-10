<script lang="ts">
  import { obs, type OAuthProviderField } from "$lib/api/bridge";
  import GoLiveTagsInput from "$lib/dialogs/golive/GoLiveTagsInput.svelte";
  import GoLiveCategoryInput from "$lib/dialogs/golive/GoLiveCategoryInput.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import { isEmptyVal, isRequiredEnum, normOpt, resolveRequiredEnum } from "$lib/dialogs/golive/fieldValue";

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
    /** The channel being edited, for an account-scoped `category` lookup (Facebook's
     * Pages); ignored by other types. */
    accountId?: string;
    /** Inherit cue: the human-readable inherited value, shown when this control holds
     * nothing of its own. The value it names is what the host will receive, so a control
     * that dropped the cue would report no value while one is pushed. Controls that CAN
     * show it as an ordinary value do (a select lands on the matching option, a category
     * and an image render as a set one would); the free-text controls show it as a
     * placeholder, which is the only way to keep an inheriting field from reading as
     * edited. The row's own "↳ using …" tag is what marks the state in every case. */
    ghostText?: string;
    /** The RAW inherited value behind `ghostText`, for controls that display it by landing
     * on their own option for it rather than by printing the text. Resolved by the caller
     * through the same call that pushes it, so the option shown and the value sent are the
     * same one — a control reverse-mapping the cue's wording would diverge the moment two
     * options shared a label. */
    ghostValue?: unknown;
    /** Override styling (amber border / accent chips) when the field is filled. */
    accent?: boolean;
    /** Prompt for the free-text controls when this call site has no inherit cue to show
     * there. Only a caller outside the inherit layering has one to give: inside it the
     * placeholder slot is what names the value that will be sent, and a second prompt
     * competing for it would hide that. */
    placeholder?: string;
    /** Constrain width (used by advanced enums). */
    narrow?: boolean;
    /** This control edits an inherit layer: empty here means "take the layer below",
     * so a `required` field keeps its empty option instead of being resolved. */
    inheritable?: boolean;
  }
  let {
    field,
    value,
    onChange,
    providerId = "",
    accountId = "",
    ghostText = "",
    ghostValue = undefined,
    placeholder = "",
    accent = false,
    narrow = false,
    inheritable = false,
  }: Props = $props();

  const str = $derived(typeof value === "string" ? value : "");
  const arr = $derived(Array.isArray(value) ? (value as string[]) : []);
  const cat = $derived(value && typeof value === "object" ? (value as { id: string; name: string }) : null);
  const bool = $derived(value === true);
  // Whether the layer below holds a value at all, independent of whether this layer holds
  // one of its own. showGhost narrows this further to "…and nothing of my own", so it reads
  // false the moment a value is set here — a control that needs "is there something
  // underneath to fall back to" (regardless of this layer's own state) reuses hasGhost, not
  // showGhost, for that question.
  const hasGhost = $derived(ghostText !== "");
  // One gate for every control's inherit cue, over the shared emptiness rule — "empty"
  // differs by value shape (a category is an object, tags an array, the rest strings), and
  // a per-control test would have each type disagreeing about when it is inheriting.
  const showGhost = $derived(hasGhost && isEmptyVal(field.type, value));
  // The cue itself, in one place so no control invents its own wording.
  const ghostLabel = $derived("↳ " + ghostText);

  const opts = $derived((field.options ?? []).map(normOpt));

  // A `required` enum renders the value it stands for rather than the raw model value,
  // and offers no empty option. The resolution is shared with the modal, which pushes
  // the same value, so the control can't show one choice while the host gets another.
  const requiredEnum = $derived(isRequiredEnum(field, inheritable));
  const enumValue = $derived(resolveRequiredEnum(field, value, inheritable));
  // While inheriting, the select LANDS ON the inherited value's own option instead of
  // carrying a second entry that names it — two entries reading "Public" are one choice as
  // far as the user can tell. The model stays empty, so the field is still inheriting and
  // the push is unchanged; only what the closed control reads changes. Falls back to the
  // empty option when the inherited value names no option here, since a <select> whose
  // value matches nothing lands on the FIRST one and would show a choice nobody made.
  const enumDisplay = $derived(
    showGhost && typeof ghostValue === "string" && opts.some((o) => o.value === ghostValue)
      ? ghostValue
      : enumValue,
  );
  // The image on screen: this field's own, else the one it inherits. An inherited thumbnail
  // is the thumbnail that will be uploaded, so it is previewed and size-checked exactly as
  // a set one is rather than named in a dashed placeholder box.
  // Only an image field names a file. Every other type puts its typed text in `str`,
  // and an ungated path here would hand each keystroke to file.readDataUri as a
  // filesystem path -- a bridge round trip and a logged failure per character, with
  // the text in the log line.
  const imgPath = $derived(field.type === "image" ? str || (showGhost ? ghostText : "") : "");

  function basename(p: string): string {
    return p.split(/[\\/]/).pop() || p;
  }
  function toggleLabel(v: string): void {
    onChange(arr.includes(v) ? arr.filter((o) => o !== v) : [...arr, v]);
  }

  // CEF serves the app from a custom app:// scheme and refuses to load file://
  // local resources from it, so the preview can't point at the OS path directly.
  // Instead the bridge reads the picked file and returns a base64 data: URI, which
  // renders inline. Re-runs on each new path; a stale async resolve is discarded via
  // the path guard so a fast re-pick can't show the wrong image, and a read failure
  // falls back to the filename via imgError.
  // YouTube's thumbnails.set hard-rejects uploads over 2 MB, and the backend then skips an
  // oversized thumbnail silently at go-live. Validate at pick/drop time so the file is
  // refused up front instead. Mirrors kMaxThumbnailBytes in youtube_provider.cpp.
  const MAX_THUMBNAIL_BYTES = 2 * 1024 * 1024;
  let tooLarge = $state(false);
  let tooLargeMb = $state(0);

  let imgError = $state(false);
  let dataUri = $state("");
  $effect(() => {
    const path = imgPath;
    imgError = false;
    dataUri = "";
    tooLarge = false;
    if (!path) {
      return;
    }
    obs
      .call("file.readDataUri", { path })
      .then((r) => {
        if (imgPath === path) {
          dataUri = r.dataUri;
          // Flag an already-saved oversized thumbnail (e.g. remembered from a prior
          // session) so it is not silently dropped at go-live.
          if (typeof r.size === "number" && r.size > MAX_THUMBNAIL_BYTES) {
            tooLarge = true;
            tooLargeMb = r.size / (1024 * 1024);
          }
        }
      })
      .catch(() => {
        if (imgPath === path) {
          imgError = true;
        }
      });
  });

  async function pickImage(): Promise<void> {
    try {
      const r = await obs.call("dialog.openFile", { mode: "open", filter: "Image Files (*.png *.jpg *.jpeg *.bmp)" });
      if (!r.path) {
        return;
      }
      if (typeof r.size === "number" && r.size > MAX_THUMBNAIL_BYTES) {
        // Refuse up front: never store a path go-live will only skip.
        tooLarge = true;
        tooLargeMb = r.size / (1024 * 1024);
        return;
      }
      tooLarge = false;
      onChange(r.path);
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
    if (!f?.path) {
      return;
    }
    if (f.size > MAX_THUMBNAIL_BYTES) {
      tooLarge = true;
      tooLargeMb = f.size / (1024 * 1024);
      return;
    }
    tooLarge = false;
    onChange(f.path);
  }
</script>

{#if field.type === "tags"}
  <GoLiveTagsInput values={arr} ghostLabel={showGhost ? ghostLabel : ""} {accent} onChange={(v) => onChange(v)} />
{:else if field.type === "category"}
  <GoLiveCategoryInput
    {providerId}
    {accountId}
    value={cat}
    placeholder={field.placeholder ?? ""}
    inheritedName={showGhost ? ghostText : ""}
    browsable={field.browsable === true}
    onChange={(v) => onChange(v)}
  />
{:else if field.type === "textarea"}
  <textarea
    class="inp"
    class:ghost={showGhost}
    class:ovr={accent}
    class:narrow
    rows="2"
    placeholder={showGhost ? ghostLabel : placeholder}
    value={str}
    oninput={(e) => onChange(e.currentTarget.value)}
  ></textarea>
{:else if field.type === "image"}
  {#if str}
    <div class="thumb has">
      {#if imgError || !dataUri}
        <span class="fname">{basename(str)}</span>
      {:else}
        <img class="preview" src={dataUri} alt={basename(str)} onerror={() => (imgError = true)} />
      {/if}
      <button
        class="thumb-x"
        title={hasGhost ? "Reset to default" : "Remove"}
        aria-label={hasGhost ? "Reset to default" : "Remove"}
        onclick={clearImage}
      >×</button>
    </div>
    <div class="thumb-meta">{basename(str)}</div>
  {:else if showGhost}
    <!-- Presented as a set image, because it is the one that will be sent. The box itself
         takes the click: there is no × on an inherited image (this layer has nothing to
         clear), so picking IS the override. The dashed frame plus reduced opacity is the
         same "ghost" treatment .inp/.chip already use elsewhere for an inherited value, so
         this state stays visually distinct from a chosen one even though both hold a preview. -->
    <div class="thumb has ghost">
      {#if imgError || !dataUri}
        <span class="fname">{basename(imgPath)}</span>
      {:else}
        <img class="preview" src={dataUri} alt={basename(imgPath)} onerror={() => (imgError = true)} />
      {/if}
      <!-- An overlay rather than the frame itself, so the frame stays the same element the
           chosen-image case above uses: a <button> around the image does not size it the
           same way. Absolutely positioned, so it takes the click and the drop for the whole
           frame without participating in layout. The corner badge is always visible so the
           override control isn't a fully invisible click target; hover/focus expand it into
           a labelled scrim for the same reason a keyboard user needs the focus ring. -->
      <button
        class="pick-over"
        aria-label="Override inherited image"
        title="Pick to override"
        ondragover={(e) => e.preventDefault()}
        ondrop={onDrop}
        onclick={() => void pickImage()}
      >
        <span class="pick-badge" aria-hidden="true"><Icon name="edit" size={11} /></span>
        <span class="pick-label" aria-hidden="true">Click or drop to override</span>
      </button>
    </div>
    <div class="thumb-meta">{basename(imgPath)}</div>
  {:else}
    <button class="thumb" ondragover={(e) => e.preventDefault()} ondrop={onDrop} onclick={() => void pickImage()}>
      <span>drop / pick</span>
      <small>PNG/JPG, ≤2 MB</small>
    </button>
  {/if}
  {#if tooLarge}
    <div class="thumb-err" role="alert">
      Too large — {tooLargeMb.toFixed(1)} MB exceeds YouTube's 2 MB limit. Pick an image under 2 MB.
    </div>
  {/if}
{:else if field.type === "enum"}
  <select class="inp" class:narrow value={enumDisplay} onchange={(e) => onChange(e.currentTarget.value)}>
    {#if !requiredEnum}
      <!-- The way back to inheriting once this layer has diverged. It never stands in for
           the inherited value — the control lands on that value's own option instead — so
           it stays unnamed rather than reading as a duplicate of the option beside it. -->
      <option value="">—</option>
    {/if}
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
    spellcheck="false"
    placeholder={showGhost ? ghostLabel : placeholder}
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
  /* Takes the row's width like every other control, capped so a multi-column dialog
     doesn't turn the drop target into a banner. 16:9 is the frame the thumbnail is
     shown in on every platform that accepts one, so the box previews at its real crop. */
  .thumb {
    width: 100%;
    max-width: 320px;
    aspect-ratio: 16 / 9;
    /* A floor, because the ratio above is not on its own enough to keep the empty picker
       a usable drop target rather than a strip. */
    min-height: 84px;
    box-sizing: border-box;
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
  /* Once there is an image, the image carries the ratio and the frame takes its height from
     the image. Taking the frame out of the sizing path is what makes the inherited and the
     chosen preview render identically instead of one of them collapsing. */
  .thumb.has {
    position: relative;
    display: block;
    aspect-ratio: auto;
    border-style: solid;
    padding: 0;
    cursor: default;
  }
  /* Same dashed + dimmed treatment as .inp.ghost / a ghost chip elsewhere, applied to the
     frame instead of a placeholder so an inherited preview no longer reads identically to a
     chosen one. The dimming lands on the content rather than the frame because opacity on
     the frame would also fade the override affordance inside it, and a child cannot escape
     an ancestor's opacity. */
  .thumb.has.ghost {
    border-style: dashed;
  }
  .thumb.has.ghost .preview,
  .thumb.has.ghost .fname {
    opacity: 0.75;
  }
  .pick-over {
    position: absolute;
    inset: 0;
    padding: 0;
    border: 0;
    background: transparent;
    cursor: pointer;
  }
  .pick-over:focus-visible {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -2px;
  }
  .pick-badge {
    position: absolute;
    top: 0;
    right: 0;
    width: 18px;
    height: 18px;
    display: flex;
    align-items: center;
    justify-content: center;
    background: var(--color-base);
    color: var(--color-muted);
    border-left: var(--border-weight) solid var(--color-border);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .pick-over:hover .pick-badge,
  .pick-over:focus-visible .pick-badge {
    color: var(--color-accent);
  }
  .pick-label {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    text-align: center;
    padding: 4px 8px;
    font-size: 10px;
    color: #fff;
    background: rgba(0, 0, 0, 0.55);
    opacity: 0;
  }
  .pick-over:hover .pick-label,
  .pick-over:focus-visible .pick-label {
    opacity: 1;
  }
  /* Sizing the replaced element directly, rather than letting it fill the frame, is what
     holds the 16:9 crop whatever element wraps it. */
  .preview {
    width: 100%;
    height: auto;
    aspect-ratio: 16 / 9;
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
    max-width: 320px;
    word-break: break-all;
  }
  .thumb-err {
    font-size: 10px;
    color: var(--color-danger, #e5484d);
    margin-top: 4px;
    max-width: 320px;
    line-height: 1.35;
  }
</style>
