<script lang="ts">
  // The Fields system editor: a DESIGNER (define the fields — key/label/type + the
  // type-specific extras: dropdown options, slider min/max/step) and a VALUES panel
  // (edit each field's current value with a type-appropriate input; image/sound fields
  // upload via FileReader base64 -> overlays.uploadAsset, storing the returned served
  // path as the field value). Fields are treated immutably: every mutation builds the
  // next array and hands it to onChange, which the page debounces into overlays.update.
  import { obs, type OverlayField } from "$lib/api/bridge";
  import Icon from "$lib/ui/Icon.svelte";

  let {
    fields,
    widgetId,
    onChange,
  }: { fields: OverlayField[]; widgetId: string; onChange: (f: OverlayField[]) => void } = $props();

  const TYPES: OverlayField["type"][] = [
    "text",
    "number",
    "color",
    "dropdown",
    "checkbox",
    "slider",
    "image-upload",
    "sound-upload",
    "font",
  ];
  const TYPE_LABEL: Record<OverlayField["type"], string> = {
    text: "Text",
    number: "Number",
    color: "Color",
    dropdown: "Dropdown",
    checkbox: "Checkbox",
    slider: "Slider",
    "image-upload": "Image",
    "sound-upload": "Sound",
    font: "Font",
  };

  let uploading = $state<number | null>(null);
  let uploadError = $state<string | null>(null);

  // Two fields sharing a key silently collide in fieldData (last-wins). Flag the
  // offending rows so the user notices; no hard block (they may be mid-rename).
  const dupKeys = $derived.by<Set<string>>(() => {
    const counts = new Map<string, number>();
    for (const f of fields) {
      counts.set(f.key, (counts.get(f.key) ?? 0) + 1);
    }
    const dups = new Set<string>();
    for (const [k, n] of counts) {
      if (n > 1 && k) {
        dups.add(k);
      }
    }
    return dups;
  });

  function commit(next: OverlayField[]): void {
    onChange(next);
  }

  function addField(): void {
    commit([...fields, { key: "field" + (fields.length + 1), type: "text", label: "Field", default: "", value: "" }]);
  }
  function removeField(i: number): void {
    commit(fields.filter((_, j) => j !== i));
  }
  function move(i: number, d: -1 | 1): void {
    const j = i + d;
    if (j < 0 || j >= fields.length) {
      return;
    }
    const next = [...fields];
    [next[i], next[j]] = [next[j], next[i]];
    commit(next);
  }
  function setField(i: number, patch: Partial<OverlayField>): void {
    commit(fields.map((f, j) => (j === i ? { ...f, ...patch } : f)));
  }

  // When the type changes, reset value/default + seed/strip the type-specific extras
  // so the values-panel input and the C++ fieldData stay coherent.
  function changeType(i: number, type: OverlayField["type"]): void {
    const base: Partial<OverlayField> = { type, options: undefined, min: undefined, max: undefined, step: undefined };
    if (type === "checkbox") {
      base.default = false;
      base.value = false;
    } else if (type === "number" || type === "slider") {
      base.default = 0;
      base.value = 0;
      if (type === "slider") {
        base.min = 0;
        base.max = 100;
        base.step = 1;
      }
    } else if (type === "dropdown") {
      base.options = [{ value: "a", label: "Option A" }];
      base.default = "a";
      base.value = "a";
    } else if (type === "color") {
      base.default = "#ffffff";
      base.value = "#ffffff";
    } else {
      base.default = "";
      base.value = "";
    }
    setField(i, base);
  }

  // --- dropdown option editing (immutable over field.options) ---
  function addOption(i: number): void {
    const opts = fields[i].options ?? [];
    setField(i, { options: [...opts, { value: "opt" + (opts.length + 1), label: "Option" }] });
  }
  function setOption(i: number, oi: number, patch: Partial<{ value: string; label: string }>): void {
    const opts = (fields[i].options ?? []).map((o, k) => (k === oi ? { ...o, ...patch } : o));
    setField(i, { options: opts });
  }
  function removeOption(i: number, oi: number): void {
    setField(i, { options: (fields[i].options ?? []).filter((_, k) => k !== oi) });
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

  async function upload(i: number, file: File, kind: "image" | "sound"): Promise<void> {
    uploadError = null;
    uploading = i;
    try {
      const base64 = await new Promise<string>((res, rej) => {
        const r = new FileReader();
        r.onload = () => res((r.result as string).split(",")[1] ?? "");
        r.onerror = () => rej(r.error);
        r.readAsDataURL(file);
      });
      const { path } = await obs.call("overlays.uploadAsset", { id: widgetId, key: file.name, kind, base64 });
      setField(i, { value: path });
    } catch (e) {
      uploadError = (e as Error).message;
    } finally {
      uploading = null;
    }
  }

  function onFile(i: number, e: Event, kind: "image" | "sound"): void {
    const input = e.currentTarget as HTMLInputElement;
    const file = input.files?.[0];
    if (file) {
      void upload(i, file, kind);
    }
  }
</script>

<div class="fields">
  {#if uploadError}<p class="err">{uploadError}</p>{/if}

  <section class="section">
    <div class="sec-bar">
      <h3 class="sec-head">Designer</h3>
      <span class="sec-spacer"></span>
      <button class="ghost" onclick={addField}><Icon name="plus" size={11} /> Add field</button>
    </div>

    {#if fields.length === 0}
      <p class="empty">No fields yet. Add one to expose a setting to this widget's template.</p>
    {/if}

    <div class="rows">
      {#each fields as f, i (i)}
        <div class="design-row">
          <div class="drow-top">
            <div class="col">
              <span class="mini">Key{#if dupKeys.has(f.key)}<span class="dup-hint"> · duplicate</span>{/if}</span>
              <input
                class="in"
                class:dup={dupKeys.has(f.key)}
                value={f.key}
                oninput={(e) => setField(i, { key: e.currentTarget.value })}
              />
            </div>
            <div class="col">
              <span class="mini">Label</span>
              <input class="in" value={f.label} oninput={(e) => setField(i, { label: e.currentTarget.value })} />
            </div>
            <div class="col type-col">
              <span class="mini">Type</span>
              <select
                class="in"
                value={f.type}
                onchange={(e) => changeType(i, e.currentTarget.value as OverlayField["type"])}
              >
                {#each TYPES as t (t)}
                  <option value={t}>{TYPE_LABEL[t]}</option>
                {/each}
              </select>
            </div>
            <div class="reorder">
              <button class="icon-btn" title="Move up" aria-label="Move up" disabled={i === 0} onclick={() => move(i, -1)}>
                <Icon name="up" size={13} />
              </button>
              <button
                class="icon-btn"
                title="Move down"
                aria-label="Move down"
                disabled={i === fields.length - 1}
                onclick={() => move(i, 1)}
              >
                <Icon name="down" size={13} />
              </button>
              <button class="icon-btn danger" title="Remove field" aria-label="Remove field" onclick={() => removeField(i)}>
                <Icon name="x" size={13} />
              </button>
            </div>
          </div>

          {#if f.type === "dropdown"}
            <div class="extras">
              <div class="extras-head">
                <span class="mini">Options</span>
                <button class="ghost sm" onclick={() => addOption(i)}><Icon name="plus" size={10} /> Option</button>
              </div>
              {#each f.options ?? [] as o, oi (oi)}
                <div class="opt-row">
                  <input
                    class="in"
                    placeholder="value"
                    value={o.value}
                    oninput={(e) => setOption(i, oi, { value: e.currentTarget.value })}
                  />
                  <input
                    class="in"
                    placeholder="label"
                    value={o.label}
                    oninput={(e) => setOption(i, oi, { label: e.currentTarget.value })}
                  />
                  <button
                    class="icon-btn danger"
                    title="Remove option"
                    aria-label="Remove option"
                    onclick={() => removeOption(i, oi)}
                  >
                    <Icon name="x" size={12} />
                  </button>
                </div>
              {/each}
            </div>
          {:else if f.type === "slider"}
            <div class="extras slider-extras">
              <label class="col">
                <span class="mini">Min</span>
                <input
                  class="in"
                  type="number"
                  value={f.min ?? 0}
                  oninput={(e) => setField(i, { min: Number(e.currentTarget.value) })}
                />
              </label>
              <label class="col">
                <span class="mini">Max</span>
                <input
                  class="in"
                  type="number"
                  value={f.max ?? 100}
                  oninput={(e) => setField(i, { max: Number(e.currentTarget.value) })}
                />
              </label>
              <label class="col">
                <span class="mini">Step</span>
                <input
                  class="in"
                  type="number"
                  value={f.step ?? 1}
                  oninput={(e) => setField(i, { step: Number(e.currentTarget.value) })}
                />
              </label>
            </div>
          {/if}
        </div>
      {/each}
    </div>
  </section>

  {#if fields.length > 0}
    <section class="section">
      <h3 class="sec-head">Values</h3>
      <div class="rows">
        {#each fields as f, i (i)}
          <div class="value-row">
            <span class="val-label" title={f.key}>{f.label || f.key}</span>
            <div class="val-input">
              {#if f.type === "checkbox"}
                <label class="switch">
                  <input
                    type="checkbox"
                    checked={asBool(f.value)}
                    onchange={(e) => setField(i, { value: e.currentTarget.checked })}
                  />
                  <span class="track"><span class="thumb"></span></span>
                </label>
              {:else if f.type === "color"}
                <input
                  class="in color"
                  type="color"
                  value={asText(f.value) || "#ffffff"}
                  oninput={(e) => setField(i, { value: e.currentTarget.value })}
                />
              {:else if f.type === "number"}
                <input
                  class="in"
                  type="number"
                  value={asNumber(f.value)}
                  oninput={(e) => setField(i, { value: Number(e.currentTarget.value) })}
                />
              {:else if f.type === "slider"}
                <div class="slider-val">
                  <input
                    type="range"
                    min={f.min ?? 0}
                    max={f.max ?? 100}
                    step={f.step ?? 1}
                    value={asNumber(f.value)}
                    oninput={(e) => setField(i, { value: Number(e.currentTarget.value) })}
                  />
                  <span class="num">{asNumber(f.value)}</span>
                </div>
              {:else if f.type === "dropdown"}
                <select
                  class="in"
                  value={asText(f.value)}
                  onchange={(e) => setField(i, { value: e.currentTarget.value })}
                >
                  {#each f.options ?? [] as o (o.value)}
                    <option value={o.value}>{o.label}</option>
                  {/each}
                </select>
              {:else if f.type === "image-upload" || f.type === "sound-upload"}
                <div class="upload">
                  <input
                    type="file"
                    accept={f.type === "image-upload" ? "image/*" : "audio/*"}
                    onchange={(e) => onFile(i, e, f.type === "image-upload" ? "image" : "sound")}
                  />
                  {#if uploading === i}
                    <span class="up-note">Uploading…</span>
                  {:else if asText(f.value)}
                    <span class="up-note ok" title={asText(f.value)}>{asText(f.value)}</span>
                  {/if}
                </div>
              {:else}
                <!-- text / font -->
                <input
                  class="in"
                  type="text"
                  placeholder={f.type === "font" ? "CSS font-family" : ""}
                  value={asText(f.value)}
                  oninput={(e) => setField(i, { value: e.currentTarget.value })}
                />
              {/if}
            </div>
          </div>
        {/each}
      </div>
    </section>
  {/if}
</div>

<style>
  .fields {
    display: flex;
    flex-direction: column;
    gap: 22px;
  }
  .section {
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .sec-head {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--color-muted);
  }
  .sec-bar {
    display: flex;
    align-items: center;
    gap: 12px;
  }
  .sec-spacer {
    flex: 1;
  }
  .empty {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }

  .design-row {
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 12px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .drow-top {
    display: flex;
    align-items: flex-end;
    gap: 10px;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 4px;
    flex: 1;
    min-width: 0;
  }
  .type-col {
    flex: 0 0 130px;
  }
  .mini {
    font-family: var(--font-mono);
    font-size: 8px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .in {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 12px;
    padding: 6px 8px;
    width: 100%;
  }
  .in:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .in.dup {
    border-color: var(--color-live);
  }
  .dup-hint {
    color: var(--color-live);
  }
  .in.color {
    padding: 2px;
    height: 30px;
    width: 46px;
    cursor: pointer;
  }
  select.in {
    cursor: pointer;
  }
  .reorder {
    display: flex;
    gap: 4px;
    flex: 0 0 auto;
  }
  .icon-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 26px;
    height: 30px;
    padding: 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    cursor: pointer;
    font-size: 12px;
  }
  .icon-btn:hover:not(:disabled) {
    color: var(--color-text);
    border-color: var(--color-accent);
  }
  .icon-btn.danger:hover:not(:disabled) {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .icon-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }

  .extras {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 10px 12px;
    border: var(--border-weight) dashed var(--color-border);
    background: var(--color-base);
  }
  .extras-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .opt-row {
    display: flex;
    gap: 8px;
    align-items: center;
  }
  .slider-extras {
    flex-direction: row;
    gap: 12px;
  }

  .value-row {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 10px 14px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .val-label {
    flex: 0 0 150px;
    font-family: var(--font-ui);
    font-size: 12px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .val-input {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: center;
  }
  .slider-val {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
  }
  .slider-val input[type="range"] {
    flex: 1;
    accent-color: var(--color-accent);
  }
  .slider-val .num {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
    min-width: 36px;
    text-align: right;
  }
  .upload {
    display: flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
  }
  .upload input[type="file"] {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
  }
  .up-note {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    max-width: 240px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .up-note.ok {
    color: var(--meter-green);
  }

  .ghost {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 6px 12px;
    background: none;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
    font-family: var(--font-ui);
    font-size: 11px;
  }
  .ghost:hover {
    color: var(--color-accent);
    border-color: var(--color-accent);
  }
  .ghost.sm {
    padding: 4px 8px;
    font-size: 10px;
  }
  .err {
    margin: 0;
    color: var(--color-live);
    font-size: 12px;
  }

  /* square-thumb switch (mirrors CanvasesPage). */
  .switch {
    display: inline-flex;
    align-items: center;
    cursor: pointer;
  }
  .switch input {
    position: absolute;
    opacity: 0;
    width: 0;
    height: 0;
  }
  .track {
    display: block;
    width: 36px;
    height: 20px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    position: relative;
    transition: background 0.12s ease;
  }
  .thumb {
    position: absolute;
    top: 50%;
    left: 3px;
    width: 12px;
    height: 12px;
    transform: translateY(-50%);
    background: var(--color-muted);
    transition:
      left 0.12s ease,
      background 0.12s ease;
  }
  .switch input:checked + .track {
    background: color-mix(in srgb, var(--color-accent) 30%, transparent);
    border-color: var(--color-accent);
  }
  .switch input:checked + .track .thumb {
    left: calc(100% - 12px - 3px);
    background: var(--color-accent);
  }
  .switch input:focus-visible + .track {
    outline: 1px solid var(--color-accent);
    outline-offset: 1px;
  }
</style>
