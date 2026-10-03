<script lang="ts">
  // The Simple pane: one row per schema field — the field's label, its live value control,
  // and a way back to the default. A field naming a `group` files itself under a heading
  // with the fields adjacent to it that name the same one, and one naming `help` gets a
  // sentence under its control; a schema that names neither renders as the flat list this
  // has always been. The field list is fixed by the widget's schema,
  // because the keys are a contract with the template that reads them: it looks up
  // f.fontSize, f.maxMessages and the rest by name and silently falls back to its own
  // default for anything missing, so a renamed or deleted key breaks the widget with no
  // error to explain it. Values are editable here; the schema is not editable anywhere.
  //
  // Structure AND defaults come from `schema`; values from `settings`, which holds
  // OVERRIDES ONLY. A row whose key is absent from settings shows the default its schema
  // field declares and keeps following it as it changes across builds; that distinction is
  // what the marker and the per-row reset make visible.
  //
  // Which control a row renders is read from FIELD_TYPES, so a new field type is one entry
  // in fieldTypes.ts. Settings stay immutable: each edit builds the next map through
  // withScopedOverride and hands it to onChange, which the page debounces into
  // overlays.update.
  //
  // Scoped use (the alert box's event and variation scopes): `settings` is then that scope's
  // own map and `inherit` says what each field would be without it -- the value and the
  // scope it comes from. An unset row shows that inherited value, greyed, with where it comes
  // from; editing it creates the scope's override; Reset writes the inherited value back,
  // which removes it. Without `inherit` the schema default plays that part, as it always has.
  import { obs, type OverlayAsset, type OverlayField } from "$lib/api/bridge";
  import {
    needsFontList,
    specFor,
    suggestsFonts,
    textStyleDefaultFault,
    withScopedOverride,
  } from "$lib/overlays/fieldTypes";
  import AnimationPicker from "$lib/overlays/AnimationPicker.svelte";
  import MediaPicker from "$lib/overlays/MediaPicker.svelte";
  import SoundPicker from "$lib/overlays/SoundPicker.svelte";
  import {
    assetFileOf,
    assetKindOf,
    scopedAssetKey,
    trackUpload,
    uploadProblem,
    type AssetLimits,
  } from "$lib/overlays/scopes/scopedAssets";
  import TextStyleControl from "$lib/overlays/TextStyleControl.svelte";
  import Button from "$lib/ui/Button.svelte";
  import CssColorInput from "$lib/ui/CssColorInput.svelte";
  import FontDatalist from "$lib/ui/FontDatalist.svelte";
  import IconButton, { ICONBTN_TOOLBAR } from "$lib/ui/IconButton.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { isPlainObject } from "$lib/utils/plainObject";

  let {
    schema,
    settings,
    widgetId,
    onChange,
    scope = "default",
    inherit,
    visible,
    assets = [],
    widgetUrl = "",
    onAssetReleased,
  }: {
    schema: OverlayField[];
    settings: Record<string, unknown>;
    widgetId: string;
    onChange: (next: Record<string, unknown>) => void;
    /** The scope `settings` belongs to; it names the files a sound or media field uploads. */
    scope?: string;
    /** What a field resolves to when this scope leaves it out, and the label of the scope
     * that supplies it. Absent in Defaults, where the schema default is that value. */
    inherit?: (f: OverlayField) => { value: unknown; fromLabel: string };
    /** Which fields this scope shows; all of them when absent. */
    visible?: (f: OverlayField) => boolean;
    assets?: OverlayAsset[];
    widgetUrl?: string;
    /** A sound or media field stopped naming an uploaded file. The page prunes the scope's
     * unreferenced uploads once the edit is saved. */
    onAssetReleased?: () => void;
  } = $props();

  let uploadingKey = $state<string | null>(null);
  let uploadError = $state<string | null>(null);

  // ONE font list per panel, shared by every font row in it: a datalist answers to any
  // number of inputs, and a schema is free to declare several font fields. Hoisting is
  // free here because this component already holds the whole flat schema; the properties
  // form mounts one per control instead, for the reason FontControl states.
  // Per-instance because the editor can render a panel in more than one place at once.
  const fontListId = $props.id();
  // Mounted only where a field asks for it, so opening a widget with no font field never
  // makes the host enumerate the system font collection.
  const wantsFonts = $derived(schema.some((f) => needsFontList(specFor(f))));

  /** The schema split into the sections the panel draws. A run of CONSECUTIVE fields
   * naming the same group becomes one section, so the schema's order stays the layout and
   * a file that names no group at all is one ungrouped run — exactly the flat list the
   * panel drew before groups existed. `i` is carried because it is the row's key. */
  interface FieldRun {
    group: string | null;
    items: { f: OverlayField; i: number }[];
  }

  const runs = $derived.by<FieldRun[]>(() => {
    const out: FieldRun[] = [];
    schema.forEach((f, i) => {
      if (visible && !visible(f)) {
        return;
      }
      const group = f.group || null;
      const last = out[out.length - 1];
      if (last && last.group === group) {
        last.items.push({ f, i });
      } else {
        out.push({ group, items: [{ f, i }] });
      }
    });
    return out;
  });

  function isOverridden(f: OverlayField): boolean {
    return Object.hasOwn(settings, f.key);
  }

  /** What the field is when this scope leaves it out: the inherited value in a scope, the
   * schema default in Defaults. */
  function baseOf(f: OverlayField): unknown {
    return inherit ? inherit(f).value : f.default;
  }

  // The override when there is one, otherwise the value the row inherits -- the same rule
  // the host applies when it assembles the page, read off the same payload. Not the host's
  // own merge OUTPUT: that resolves an overridden key to the override, so using it as the
  // fallback would keep showing the value a moment after it was cleared, and a control moved
  // back to its default would visibly snap away from it.
  function valueOf(f: OverlayField): unknown {
    return isOverridden(f) ? settings[f.key] : baseOf(f);
  }

  function setValue(f: OverlayField, v: unknown): void {
    const before = settings[f.key];
    onChange(withScopedOverride(settings, f.key, v, baseOf(f)));
    if (assetFileOf(before) && before !== v) {
      onAssetReleased?.();
    }
  }

  // --- value coercion helpers (a setting is unknown; inputs need concrete types) ---
  function asText(v: unknown): string {
    return v == null ? "" : String(v);
  }
  function asNumber(v: unknown): number {
    const n = typeof v === "number" ? v : Number(v);
    return Number.isFinite(n) ? n : 0;
  }
  function asBool(v: unknown): boolean {
    return v === true || v === "true";
  }

  // The host's per-kind caps, fetched once per panel and only once a scoped upload starts,
  // so an oversize file is refused before any of it is read.
  let limits: Promise<AssetLimits> | null = null;
  function assetLimits(): Promise<AssetLimits> {
    limits ??= obs.call("overlays.assetLimits").catch((e: unknown) => {
      limits = null;
      throw e;
    });
    return limits;
  }

  /** A sound or media upload: named for its scope and field, checked against the cap first. */
  async function uploadScoped(f: OverlayField, file: File): Promise<void> {
    uploadError = null;
    const kind = assetKindOf(file);
    if (!kind) {
      uploadError = `${file.name} is not a file this field takes.`;
      return;
    }
    try {
      const problem = uploadProblem(file, kind, await assetLimits());
      if (problem) {
        uploadError = problem;
        return;
      }
    } catch (e) {
      uploadError = (e as Error).message;
      return;
    }
    await upload(f, file, kind, scopedAssetKey(scope, f.key, file.name), scope);
  }

  async function upload(
    f: OverlayField,
    file: File,
    kind: "image" | "sound" | "video",
    key: string,
    assetScope: string | undefined,
  ): Promise<void> {
    // Pinned alongside the key, because the encode below is a real wait on a large file
    // and the page can select another overlay inside it. Reading the prop afterwards would
    // upload against whichever widget is open by then, and the host both writes the blob
    // into that widget's assets dir and lists it in its assets — a stray file on a widget
    // the user never touched, while the field that wanted it stays empty.
    const target = widgetId;
    uploadError = null;
    uploadingKey = f.key;
    try {
      const base64 = await new Promise<string>((res, rej) => {
        const r = new FileReader();
        r.onload = () => res((r.result as string).split(",")[1] ?? "");
        r.onerror = () => rej(r.error);
        r.readAsDataURL(file);
      });
      await trackUpload(async () => {
        const { path } = await obs.call("overlays.uploadAsset", { id: target, key, kind, base64, scope: assetScope });
        // The panel may be looking at another widget by now; writing the path would set it
        // on that widget's settings instead.
        if (widgetId === target) {
          setValue(f, path);
        }
      });
    } catch (e) {
      uploadError = (e as Error).message;
    } finally {
      // A second upload may already own the indicator.
      if (uploadingKey === f.key) {
        uploadingKey = null;
      }
    }
  }

  function onFile(f: OverlayField, e: Event, kind: "image" | "sound"): void {
    const input = e.currentTarget as HTMLInputElement;
    const file = input.files?.[0];
    if (file) {
      void upload(f, file, kind, file.name, undefined);
    }
  }
</script>

<div class="fields">
  {#if uploadError}<p class="err">{uploadError}</p>{/if}

  {#if schema.length === 0}
    <p class="empty">This widget's template exposes no settings.</p>
  {/if}

  {#snippet row(f: OverlayField, i: number)}
    {@const spec = specFor(f)}
    {@const name = f.label || f.key}
    <!-- Only minted where the schema actually carries help: aria-describedby pointing at
         an element that was never rendered is worse than no description at all. -->
    {@const helpId = f.help ? `${widgetId}:help:${i}` : undefined}
    {@const placeholder = spec.control === "text" ? (spec.placeholder ?? "") : ""}
    {@const set = isOverridden(f)}
    {@const value = valueOf(f)}
    {@const inherited = inherit && !set ? inherit(f) : null}
    {@const tall = spec.control === "textstyle" || spec.control === "animation" || spec.control === "sound"}
    <li
      class="frow"
      class:frow--set={set && !inherit}
      class:frow--scoped={set && !!inherit}
      class:frow--inherited={!!inherited}
      class:frow--tall={tall}
    >
      <span class="fname">
        <span class="cv-ci__name fname__label" title={f.key}>{name}</span>
        {#if inherited}
          <span class="fname__from">from {inherited.fromLabel}</span>
        {/if}
      </span>

      <div class="fval">
        {#if spec.control === "switch"}
          <ToggleSwitch
            checked={asBool(value)}
            ariaLabel={name}
            ariaDescribedBy={helpId}
            onchange={(v) => setValue(f, v)}
          />
        {:else if spec.control === "color"}
          <CssColorInput
            value={asText(value) || "#ffffff"}
            ariaLabel={name}
            ariaDescribedBy={helpId}
            onChange={(v) => setValue(f, v)}
          />
        {:else if spec.control === "number"}
          <div class="cv-num">
            <input
              type="number"
              value={asNumber(value)}
              aria-label={name}
              aria-describedby={helpId}
              oninput={(e) => setValue(f, Number(e.currentTarget.value))}
            />
          </div>
        {:else if spec.control === "slider"}
          <div class="fslider">
            <input
              type="range"
              min={f.min ?? 0}
              max={f.max ?? 100}
              step={f.step ?? 1}
              value={asNumber(value)}
              aria-label={name}
              aria-describedby={helpId}
              oninput={(e) => setValue(f, Number(e.currentTarget.value))}
            />
            <span class="fslider__n">{asNumber(value)}</span>
          </div>
        {:else if spec.control === "select"}
          <select
            class="cv-select"
            value={asText(value)}
            aria-label={name}
            aria-describedby={helpId}
            onchange={(e) => setValue(f, e.currentTarget.value)}
          >
            <!-- Keyed by index: the options are fixed by the template, and two of them
                 may legitimately carry the same value. -->
            {#each f.options ?? [] as o, oi (oi)}
              <option value={o.value}>{o.label}</option>
            {/each}
          </select>
        {:else if spec.control === "upload"}
          <div class="fupload">
            <input
              type="file"
              accept={spec.accept}
              aria-label={name}
              aria-describedby={helpId}
              onchange={(e) => onFile(f, e, spec.uploadKind)}
            />
            {#if uploadingKey === f.key}
              <span class="fnote">Uploading…</span>
            {:else if asText(value)}
              <span class="fnote ok" title={asText(value)}>{asText(value)}</span>
            {/if}
          </div>
        {:else if spec.control === "animation"}
          <AnimationPicker
            {value}
            layer={f.layer ?? "in"}
            {name}
            ariaDescribedBy={helpId}
            onChange={(next) => setValue(f, next)}
          />
        {:else if spec.control === "sound"}
          <SoundPicker
            value={asText(value)}
            {name}
            {assets}
            {widgetUrl}
            uploading={uploadingKey === f.key}
            ariaDescribedBy={helpId}
            onUpload={(file) => void uploadScoped(f, file)}
            onChange={(next) => setValue(f, next)}
          />
        {:else if spec.control === "media"}
          <MediaPicker
            value={asText(value)}
            {name}
            {assets}
            {widgetUrl}
            uploading={uploadingKey === f.key}
            ariaDescribedBy={helpId}
            onUpload={(file) => void uploadScoped(f, file)}
            onChange={(next) => setValue(f, next)}
          />
        {:else if spec.control === "textstyle"}
          {@const fault = textStyleDefaultFault(f)}
          {#if fault}
            <p class="err">{fault}</p>
          {:else}
            <!-- A hand-edited document can put anything under the key; anything that is
                 not an object configures nothing, and the first edit replaces it. -->
            <TextStyleControl
              value={isPlainObject(value) ? value : {}}
              {name}
              {fontListId}
              ariaDescribedBy={helpId}
              onChange={(next) => setValue(f, next)}
            />
          {/if}
        {:else}
          <input
            class="ftext"
            type="text"
            {placeholder}
            list={suggestsFonts(spec) ? fontListId : undefined}
            value={asText(value)}
            aria-label={name}
            aria-describedby={helpId}
            oninput={(e) => setValue(f, e.currentTarget.value)}
          />
        {/if}

        {#if f.help}
          <p class="fhelp" id={helpId}>{f.help}</p>
        {/if}
      </div>

      <!-- The slot is always laid out, so a row does not shift as it gains or loses its
           override. Writing the default back is what clears the key (see withOverride). -->
      <div class="freset" class:freset--wide={!!inherit}>
        {#if set && inherit}
          <Button size="xs" variant="bare" tone="warn" aria-label="Reset {name}" onclick={() => setValue(f, baseOf(f))}>
            Reset
          </Button>
        {:else if set}
          <IconButton
            icon="x"
            {...ICONBTN_TOOLBAR}
            iconSize={12}
            aria-label="Reset {name} to default"
            title="Changed from the default — reset it"
            onclick={() => setValue(f, f.default)}
          />
        {/if}
      </div>
    </li>
  {/snippet}

  <!-- Rows are keyed by widget + schema position rather than by field key: a hand-edited
       document can carry the same key twice, which a key-keyed block reads as a collision
       and throws on, and folding the widget id in tears the rows down when the selection
       changes so no uncontrolled file input carries its filename across. -->
  {#snippet rowList(items: { f: OverlayField; i: number }[])}
    <ul class="frows">
      {#each items as it (widgetId + ":" + it.i)}
        {@render row(it.f, it.i)}
      {/each}
    </ul>
  {/snippet}

  <div class="flist">
    {#each runs as run, ri (widgetId + ":run:" + ri)}
      {#if run.group}
        <!-- Native disclosure: keyboard, focus and the expanded state come with it, and the
             open state survives a scope switch because the section is keyed by position. -->
        <details class="fgroup" open>
          <summary class="fgroup__h">{run.group}</summary>
          {@render rowList(run.items)}
        </details>
      {:else}
        {@render rowList(run.items)}
      {/if}
    {/each}
  </div>

  {#if wantsFonts}
    <FontDatalist id={fontListId} />
  {/if}
</div>

<style>
  /* Rows are read left-to-right, so they stop well short of a wide editor pane: past
     roughly this width the label and its control drift apart. */
  .fields {
    display: flex;
    flex-direction: column;
    gap: 12px;
    max-width: 800px;
  }
  .err {
    margin: 0;
    color: var(--color-live-text);
    font-size: 12px;
  }
  .empty {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }

  .flist {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .frows {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .fgroup {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .fgroup__h {
    margin: 0;
    cursor: pointer;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 400;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
  .frow {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    border: var(--border-weight) solid var(--color-border);
    /* Carried at the marker's own width even when it is off, so switching a row between
       default and overridden recolors the edge instead of moving the row. */
    border-left: 2px solid var(--color-border);
    background: var(--color-surface);
  }
  .frow--set {
    border-left-color: var(--color-accent);
  }
  /* An override set in an event or variation scope: amber, so it reads as "this scope
     differs" rather than as the Defaults marker. */
  .frow--scoped {
    border-left-color: var(--color-warn);
  }
  /* Inherited: the control shows the value it inherits, greyed until it is pointed at or
     focused -- never while focused, so the focus edge is drawn at full strength. */
  .frow--inherited .fval {
    opacity: 0.55;
    transition: opacity 150ms ease;
  }
  .frow--inherited .fval:hover,
  .frow--inherited .fval:focus-within {
    opacity: 1;
  }
  /* A control that grows downward when it opens: the label stays at the row's top edge
     instead of drifting to the vertical middle of an expanded editor. */
  .frow--tall {
    align-items: flex-start;
  }
  .frow--tall .fname {
    padding-top: 6px;
  }
  @media (prefers-reduced-motion: reduce) {
    .frow--inherited .fval {
      transition: none;
    }
  }
  .frow--tall .fval {
    display: block;
  }
  .fname {
    flex: 0 0 140px;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .fname__label {
    /* Overrides .cv-ci__name's dim resting color: that class dims until its row is
       selected, and these rows have no selected state to brighten into. */
    color: var(--color-text);
  }
  .fname__from {
    font-family: var(--font-mono);
    font-size: 9.5px;
    color: var(--color-muted);
  }
  .fval {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: center;
    /* So a help line can sit below the control on its own row without every other row
       needing a wrapper element it would otherwise be the only child of. */
    flex-wrap: wrap;
  }
  .fhelp {
    flex: 0 0 100%;
    margin: 4px 0 0;
    font-size: 11px;
    line-height: 1.45;
    color: var(--color-muted);
  }
  .freset {
    flex: 0 0 25px;
    display: flex;
    justify-content: flex-end;
  }
  .freset--wide {
    flex-basis: 52px;
  }
  .ftext {
    width: 100%;
  }
  .fslider {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
  }
  .fslider input[type="range"] {
    flex: 1;
    min-width: 0;
  }
  .fslider__n {
    flex: 0 0 auto;
    min-width: 36px;
    text-align: right;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .fupload {
    display: flex;
    align-items: center;
    gap: 10px;
    min-width: 0;
    flex-wrap: wrap;
  }
  /* File inputs sit outside the global control baseline, which lists the typed text
     inputs only. */
  .fupload input[type="file"] {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
  }
  .fnote {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    max-width: 220px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .fnote.ok {
    color: var(--meter-green);
  }
</style>
