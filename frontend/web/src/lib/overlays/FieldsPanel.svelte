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
  // withOverride and hands it to onChange, which the page debounces into overlays.update.
  import { obs, type OverlayField } from "$lib/api/bridge";
  import {
    needsFontList,
    specFor,
    suggestsFonts,
    textStyleDefaultFault,
    withOverride,
  } from "$lib/overlays/fieldTypes";
  import TextStyleControl from "$lib/overlays/TextStyleControl.svelte";
  import CssColorInput from "$lib/ui/CssColorInput.svelte";
  import FontDatalist from "$lib/ui/FontDatalist.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { isPlainObject } from "$lib/utils/plainObject";

  let {
    schema,
    settings,
    widgetId,
    onChange,
  }: {
    schema: OverlayField[];
    settings: Record<string, unknown>;
    widgetId: string;
    onChange: (next: Record<string, unknown>) => void;
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

  // The override when there is one, otherwise the default its schema field declares —
  // the same rule the host applies when it assembles the page, read off the same payload.
  // Not the host's own merge OUTPUT: that resolves an overridden key to the override, so
  // using it as the fallback would keep showing the value a moment after it was cleared,
  // and a control moved back to its default would visibly snap away from it.
  function valueOf(f: OverlayField): unknown {
    return isOverridden(f) ? settings[f.key] : f.default;
  }

  function setValue(f: OverlayField, v: unknown): void {
    onChange(withOverride(settings, f, v));
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

  async function upload(f: OverlayField, file: File, kind: "image" | "sound"): Promise<void> {
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
      const { path } = await obs.call("overlays.uploadAsset", { id: target, key: file.name, kind, base64 });
      // The panel may be looking at another widget by now; writing the path would set it
      // on that widget's settings instead.
      if (widgetId === target) {
        setValue(f, path);
      }
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
      void upload(f, file, kind);
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
    <li class="frow" class:frow--set={set} class:frow--tall={spec.control === "textstyle"}>
      <span class="cv-ci__name fname" title={f.key}>{name}</span>

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
      <div class="freset">
        {#if set}
          <button
            class="tool-btn"
            aria-label="Reset {name} to default"
            title="Changed from the default — reset it"
            onclick={() => setValue(f, f.default)}
          >
            <Icon name="x" size={12} />
          </button>
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
        <section class="fgroup">
          <h3 class="fgroup__h">{run.group}</h3>
          {@render rowList(run.items)}
        </section>
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
    color: var(--color-live);
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
  /* A control that grows downward when it opens: the label stays at the row's top edge
     instead of drifting to the vertical middle of an expanded editor. */
  .frow--tall {
    align-items: flex-start;
  }
  .frow--tall .fname {
    padding-top: 6px;
  }
  .frow--tall .fval {
    display: block;
  }
  .fname {
    flex: 0 0 180px;
    min-width: 0;
    /* Overrides .cv-ci__name's dim resting color: that class dims until its row is
       selected, and these rows have no selected state to brighten into. */
    color: var(--color-text);
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
