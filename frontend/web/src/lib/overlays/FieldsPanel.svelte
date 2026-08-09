<script lang="ts">
  // The Simple pane: ONE list, one row per field — the field's label and its live value
  // control, nothing else. The field list is fixed by the widget's type, because the keys
  // are a contract with the shipped template.js: it reads f.fontSize, f.maxMessages and
  // the rest by name and silently falls back to its own default for anything missing, so
  // a renamed or deleted key breaks the widget with no error to explain it. Values are
  // editable here; the schema is not editable anywhere.
  //
  // Which control a row renders is read from FIELD_TYPES, so a new field type is one entry
  // in fieldTypes.ts. Fields stay immutable: each edit builds the next array and hands it
  // to onChange, which the page debounces into overlays.update. Rows are addressed by
  // their id, never by index, so an upload that lands after the page swapped the widget
  // (an external edit, a reset) cannot write its path onto whatever field took the slot.
  import { obs, type OverlayField } from "$lib/api/bridge";
  import { FIELD_TYPES, withFieldIds } from "$lib/overlays/fieldTypes";
  import CssColorInput from "$lib/ui/CssColorInput.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";

  let {
    fields,
    widgetId,
    onChange,
  }: { fields: OverlayField[]; widgetId: string; onChange: (f: OverlayField[]) => void } = $props();

  // The page hydrates ids before the list ever reaches here, so this is a pass-through
  // that returns the very same array; it stands as the guarantee that every row below
  // has an id, not as a second source of them.
  const rows = $derived(withFieldIds(fields));

  let uploadingId = $state<string | null>(null);
  let uploadError = $state<string | null>(null);

  function indexOf(id: string): number {
    return rows.findIndex((f) => f.id === id);
  }

  function setField(id: string, patch: Partial<OverlayField>): void {
    onChange(rows.map((f) => (f.id === id ? { ...f, ...patch } : f)));
  }

  // --- value coercion helpers (field.value is unknown; inputs need concrete types) ---
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

  async function upload(id: string, file: File, kind: "image" | "sound"): Promise<void> {
    // Pinned alongside the row id, because the encode below is a real wait on a large file
    // and the page can select another overlay inside it. Reading the prop afterwards would
    // upload against whichever widget is open by then, and the host both writes the blob
    // into that widget's assets dir and lists it in its assets — a stray file on a widget
    // the user never touched, while the field that wanted it stays empty.
    const target = widgetId;
    uploadError = null;
    uploadingId = id;
    try {
      const base64 = await new Promise<string>((res, rej) => {
        const r = new FileReader();
        r.onload = () => res((r.result as string).split(",")[1] ?? "");
        r.onerror = () => rej(r.error);
        r.readAsDataURL(file);
      });
      const { path } = await obs.call("overlays.uploadAsset", { id: target, key: file.name, kind, base64 });
      // The row this upload belongs to may be gone: the page replaces the whole widget
      // when it is reset or edited elsewhere, and that re-mints every id.
      if (indexOf(id) >= 0) {
        setField(id, { value: path });
      }
    } catch (e) {
      uploadError = (e as Error).message;
    } finally {
      // A second upload may already own the indicator.
      if (uploadingId === id) {
        uploadingId = null;
      }
    }
  }

  function onFile(id: string, e: Event, kind: "image" | "sound"): void {
    const input = e.currentTarget as HTMLInputElement;
    const file = input.files?.[0];
    if (file) {
      void upload(id, file, kind);
    }
  }
</script>

<div class="fields">
  {#if uploadError}<p class="err">{uploadError}</p>{/if}

  {#if rows.length === 0}
    <p class="empty">This widget's template exposes no settings.</p>
  {/if}

  <!-- `type` arrives from a passthrough JSON document the user can hand-edit, so the
       Record's key type does not bind it at runtime; an unrecognized type reads as text
       rather than taking the whole panel down with it. -->
  <ul class="flist">
    {#each rows as f (f.id)}
      {@const spec = FIELD_TYPES[f.type] ?? FIELD_TYPES.text}
      {@const name = f.label || f.key}
      {@const placeholder = spec.control === "text" ? (spec.placeholder ?? "") : ""}
      <li class="frow">
        <span class="cv-ci__name fname" title={f.key}>{name}</span>

        <div class="fval">
          {#if spec.control === "switch"}
            <ToggleSwitch checked={asBool(f.value)} ariaLabel={name} onchange={(v) => setField(f.id, { value: v })} />
          {:else if spec.control === "color"}
            <CssColorInput
              value={asText(f.value) || "#ffffff"}
              ariaLabel={name}
              onChange={(v) => setField(f.id, { value: v })}
            />
          {:else if spec.control === "number"}
            <div class="cv-num">
              <input
                type="number"
                value={asNumber(f.value)}
                aria-label={name}
                oninput={(e) => setField(f.id, { value: Number(e.currentTarget.value) })}
              />
            </div>
          {:else if spec.control === "slider"}
            <div class="fslider">
              <input
                type="range"
                min={f.min ?? 0}
                max={f.max ?? 100}
                step={f.step ?? 1}
                value={asNumber(f.value)}
                aria-label={name}
                oninput={(e) => setField(f.id, { value: Number(e.currentTarget.value) })}
              />
              <span class="fslider__n">{asNumber(f.value)}</span>
            </div>
          {:else if spec.control === "select"}
            <select
              class="cv-select"
              value={asText(f.value)}
              aria-label={name}
              onchange={(e) => setField(f.id, { value: e.currentTarget.value })}
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
                onchange={(e) => onFile(f.id, e, spec.uploadKind)}
              />
              {#if uploadingId === f.id}
                <span class="fnote">Uploading…</span>
              {:else if asText(f.value)}
                <span class="fnote ok" title={asText(f.value)}>{asText(f.value)}</span>
              {/if}
            </div>
          {:else}
            <input
              class="ftext"
              type="text"
              {placeholder}
              value={asText(f.value)}
              aria-label={name}
              oninput={(e) => setField(f.id, { value: e.currentTarget.value })}
            />
          {/if}
        </div>
      </li>
    {/each}
  </ul>
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
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .frow {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
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
