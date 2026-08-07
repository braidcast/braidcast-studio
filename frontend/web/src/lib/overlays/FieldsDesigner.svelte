<script lang="ts">
  // The Fields system editor: ONE list, one row per field. A collapsed row is the drag
  // handle, the field's label, its live value control inline, and a gear; the gear opens
  // that row's schema — key, label, type, the type-specific extras, delete. Tweaking a
  // value while watching the preview therefore never involves expanding anything, and a
  // field's definition and its value are never two scroll positions apart.
  //
  // Every type-dependent decision (which control, which extras, what a type change
  // seeds) is read from FIELD_TYPES, so a new field type is one entry in fieldTypes.ts.
  // Fields stay immutable: each mutation builds the next array and hands it to onChange,
  // which the page debounces into overlays.update. Rows are addressed by their id, never
  // by index, so a reorder or a delete during an upload cannot land the file on the
  // field that slid into the slot.
  import { tick } from "svelte";
  import { obs, type LabeledOption, type OverlayField } from "$lib/api/bridge";
  import {
    FIELD_TYPES,
    FIELD_TYPE_ORDER,
    newField,
    newFieldId,
    typeChangePatch,
    withFieldIds,
    type DesignerField,
    type FieldType,
  } from "$lib/overlays/fieldTypes";
  import CssColorInput from "$lib/ui/CssColorInput.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { createReorder } from "$lib/utils/listReorder.svelte";

  let {
    fields,
    widgetId,
    onChange,
  }: { fields: OverlayField[]; widgetId: string; onChange: (f: OverlayField[]) => void } = $props();

  // The page hydrates ids before the list ever reaches here, so this is a pass-through
  // that returns the very same array; it stands as the guarantee that every row below
  // has an id, not as a second source of them.
  const rows = $derived(withFieldIds(fields));

  // One expansion at a time, keyed by id so it follows its field through a reorder.
  let expandedId = $state<string | null>(null);
  let uploadingId = $state<string | null>(null);
  let uploadError = $state<string | null>(null);

  // Two fields sharing a key silently collide in fieldData (last-wins). Flag the
  // offending rows so the user notices; no hard block (they may be mid-rename).
  const dupKeys = $derived.by<Set<string>>(() => {
    const counts = new Map<string, number>();
    for (const f of rows) {
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

  function emit(next: DesignerField[]): void {
    onChange(next);
  }

  function indexOf(id: string): number {
    return rows.findIndex((f) => f.id === id);
  }

  function setField(id: string, patch: Partial<OverlayField>): void {
    emit(rows.map((f) => (f.id === id ? { ...f, ...patch } : f)));
  }

  function addField(): void {
    const f: DesignerField = { ...newField("field" + (rows.length + 1)), id: newFieldId() };
    emit([...rows, f]);
    expandedId = f.id;
  }

  function removeField(id: string): void {
    if (expandedId === id) {
      expandedId = null;
    }
    emit(rows.filter((f) => f.id !== id));
  }

  function moveTo(id: string, to: number): void {
    const from = indexOf(id);
    if (from < 0 || to < 0 || to >= rows.length || to === from) {
      return;
    }
    const next = [...rows];
    const [moved] = next.splice(from, 1);
    next.splice(to, 0, moved);
    emit(next);
  }

  function nudge(id: string, d: -1 | 1): void {
    moveTo(id, indexOf(id) + d);
  }

  // Drag lives on the handle, not the row: the row carries text boxes, ranges and a
  // colour picker, and a pointer capture that starts anywhere inside would swallow them.
  const reorder = createReorder({
    getIds: () => rows.map((f) => f.id),
    commit: (order) => {
      const by = new Map(rows.map((f) => [f.id, f]));
      emit(order.map((id) => by.get(id)).filter((f): f is DesignerField => !!f));
    },
  });
  const dragRow = reorder.row;

  // The drop index is computed from the registered nodes' midpoints, and the registered
  // node is the handle — which sits at the top of an expanded row, not its centre, so an
  // open panel would skew nearly all of its own height into "insert after". Collapsing
  // for the duration of the drag makes every row header-height and the midpoints uniform
  // again. The panel is a sibling of the handle, so removing it leaves the pointer
  // capture on the handle intact.
  $effect(() => {
    if (reorder.dragging) {
      expandedId = null;
    }
  });

  // Alt+Arrow on the focused handle is the keyboard equal of the pointer drag. Whether a
  // move re-inserts this row's node or the one it passes is the reconciler's choice, and
  // re-insertion drops focus to the document, so the handle is focused again once the
  // move has rendered — a no-op on the moves where focus survived. The keyed block moves
  // nodes rather than rebuilding them, so the reference captured here stays valid.
  function onHandleKey(e: KeyboardEvent, id: string): void {
    if (!e.altKey) {
      return;
    }
    const d: -1 | 0 | 1 = e.key === "ArrowUp" ? -1 : e.key === "ArrowDown" ? 1 : 0;
    if (d === 0) {
      return;
    }
    e.preventDefault();
    const handle = e.currentTarget as HTMLElement;
    nudge(id, d);
    void tick().then(() => handle.focus());
  }

  // --- dropdown option editing (immutable over field.options) ---
  function optionsOf(id: string): LabeledOption[] {
    return rows[indexOf(id)]?.options ?? [];
  }
  function addOption(id: string): void {
    const opts = optionsOf(id);
    setField(id, { options: [...opts, { value: "opt" + (opts.length + 1), label: "Option" }] });
  }
  function setOption(id: string, oi: number, patch: Partial<LabeledOption>): void {
    setField(id, { options: optionsOf(id).map((o, k) => (k === oi ? { ...o, ...patch } : o)) });
  }
  function removeOption(id: string, oi: number): void {
    setField(id, { options: optionsOf(id).filter((_, k) => k !== oi) });
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
    uploadError = null;
    uploadingId = id;
    try {
      const base64 = await new Promise<string>((res, rej) => {
        const r = new FileReader();
        r.onload = () => res((r.result as string).split(",")[1] ?? "");
        r.onerror = () => rej(r.error);
        r.readAsDataURL(file);
      });
      const { path } = await obs.call("overlays.uploadAsset", { id: widgetId, key: file.name, kind, base64 });
      // The field this upload belongs to may have been deleted while it was in flight.
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
    <p class="empty">No fields yet. Add one to expose a setting to this widget's template.</p>
  {/if}

  <!-- `type` arrives from a passthrough JSON document the user can hand-edit, so the
       Record's key type does not bind it at runtime; an unrecognized type reads as text
       rather than taking the whole editor down with it. -->
  <ul class="flist">
    {#each rows as f, i (f.id)}
      {@const spec = FIELD_TYPES[f.type] ?? FIELD_TYPES.text}
      {@const name = f.label || f.key}
      {@const dup = dupKeys.has(f.key)}
      {@const isOpen = expandedId === f.id}
      {@const placeholder = spec.control === "text" ? (spec.placeholder ?? "") : ""}
      <li
        class="frow"
        class:open={isOpen}
        class:lifting={reorder.dragIndex === i}
        class:dropBefore={reorder.dragging && reorder.dropIndex === i}
        class:dropAfter={reorder.dragging && reorder.dropIndex === rows.length && i === rows.length - 1}
      >
        <div class="fmain">
          <button
            class="tool-btn fhandle"
            use:dragRow={i}
            onkeydown={(e) => onHandleKey(e, f.id)}
            aria-label={`Reorder ${name}`}
            aria-keyshortcuts="Alt+ArrowUp Alt+ArrowDown"
            title="Drag to reorder · Alt+Up/Down to move"
          >
            <Icon name="grip" size={13} />
          </button>

          <span class="cv-ci__name fname" title={f.key}>{name}</span>
          {#if dup}<span class="cv-ci__badge fdup" title="Another field uses this key">DUP</span>{/if}

          <div class="fval">
            {#if spec.control === "switch"}
              <ToggleSwitch
                checked={asBool(f.value)}
                ariaLabel={name}
                onchange={(v) => setField(f.id, { value: v })}
              />
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
                <!-- Keyed by index, like the options editor: an option's value is a text
                     box in that editor, so two options transiently share one mid-rename. -->
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

          <button
            class="tool-btn"
            class:active={isOpen}
            aria-expanded={isOpen}
            aria-controls={isOpen ? `fp-${f.id}` : undefined}
            aria-label={`Settings for ${name}`}
            title="Field settings"
            onclick={() => (expandedId = isOpen ? null : f.id)}
          >
            <Icon name="gear" size={13} />
          </button>
        </div>

        {#if isOpen}
          <div class="fpanel" id={`fp-${f.id}`}>
            <div class="cv-field pgrid">
              <div>
                <div class="cv-field__l">
                  Key{#if dup}<span class="cv-field__sub warn">duplicate — only the last one reaches the template</span
                    >{/if}
                </div>
                <input
                  class="pfull"
                  class:dup
                  value={f.key}
                  aria-label="Field key"
                  oninput={(e) => setField(f.id, { key: e.currentTarget.value })}
                />
              </div>
              <div>
                <div class="cv-field__l">Label</div>
                <input
                  class="pfull"
                  value={f.label}
                  aria-label="Field label"
                  oninput={(e) => setField(f.id, { label: e.currentTarget.value })}
                />
              </div>
              <div>
                <div class="cv-field__l">Type</div>
                <select
                  class="cv-select"
                  value={f.type}
                  aria-label="Field type"
                  onchange={(e) => setField(f.id, typeChangePatch(e.currentTarget.value as FieldType))}
                >
                  {#each FIELD_TYPE_ORDER as t (t)}
                    <option value={t}>{FIELD_TYPES[t].label}</option>
                  {/each}
                </select>
              </div>
            </div>

            {#if spec.extras === "range"}
              <div class="cv-field">
                <div class="cv-field__l">Range</div>
                <div class="cv-numrow">
                  <div class="cv-num">
                    <input
                      type="number"
                      value={f.min ?? 0}
                      aria-label="Minimum"
                      oninput={(e) => setField(f.id, { min: Number(e.currentTarget.value) })}
                    />
                    <span class="cv-num__u">min</span>
                  </div>
                  <div class="cv-num">
                    <input
                      type="number"
                      value={f.max ?? 100}
                      aria-label="Maximum"
                      oninput={(e) => setField(f.id, { max: Number(e.currentTarget.value) })}
                    />
                    <span class="cv-num__u">max</span>
                  </div>
                  <div class="cv-num">
                    <input
                      type="number"
                      value={f.step ?? 1}
                      aria-label="Step"
                      oninput={(e) => setField(f.id, { step: Number(e.currentTarget.value) })}
                    />
                    <span class="cv-num__u">step</span>
                  </div>
                </div>
              </div>
            {:else if spec.extras === "options"}
              <div class="cv-field">
                <div class="cv-field__l">Options <span class="cv-field__sub">value · label</span></div>
                {#each f.options ?? [] as o, oi (oi)}
                  <div class="popt">
                    <input
                      placeholder="value"
                      value={o.value}
                      aria-label={`Option ${oi + 1} value`}
                      oninput={(e) => setOption(f.id, oi, { value: e.currentTarget.value })}
                    />
                    <input
                      placeholder="label"
                      value={o.label}
                      aria-label={`Option ${oi + 1} label`}
                      oninput={(e) => setOption(f.id, oi, { label: e.currentTarget.value })}
                    />
                    <button
                      class="cfhead__act cfhead__del"
                      title="Remove option"
                      aria-label={`Remove option ${oi + 1}`}
                      onclick={() => removeOption(f.id, oi)}
                    >
                      <Icon name="x" size={12} />
                    </button>
                  </div>
                {/each}
                <button class="cv-newcanvas paddopt" onclick={() => addOption(f.id)}>
                  <Icon name="plus" size={11} /><span>Add option</span>
                </button>
              </div>
            {/if}

            <div class="cv-field pfoot">
              <button
                class="cfhead__act"
                title="Move up"
                aria-label={`Move ${name} up`}
                disabled={i === 0}
                onclick={() => nudge(f.id, -1)}
              >
                <Icon name="up" size={13} />
              </button>
              <button
                class="cfhead__act"
                title="Move down"
                aria-label={`Move ${name} down`}
                disabled={i === rows.length - 1}
                onclick={() => nudge(f.id, 1)}
              >
                <Icon name="down" size={13} />
              </button>
              <span class="pspacer"></span>
              <button class="cfhead__act cfhead__del pdel" onclick={() => removeField(f.id)}>
                <Icon name="trash" size={13} /><span>Delete field</span>
              </button>
            </div>
          </div>
        {/if}
      </li>
    {/each}
  </ul>

  <button class="cv-newcanvas" onclick={addField}><Icon name="plus" size={13} /><span>Add field</span></button>
</div>

<style>
  /* Rows are read left-to-right, so they stop well short of a wide editor pane: past
     roughly this width the label and its control drift apart and the gear ends up at
     the far screen edge. */
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
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .frow.open {
    border-color: var(--color-accent);
  }
  /* Insertion cue drawn on the row itself: an inset edge needs no extra node between
     rows and so cannot shift the list under the pointer mid-drag. */
  .frow.dropBefore {
    box-shadow: inset 0 2px 0 var(--color-accent);
  }
  .frow.dropAfter {
    box-shadow: inset 0 -2px 0 var(--color-accent);
  }

  .fmain {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
  }
  .fhandle {
    cursor: grab;
    /* The pointer drag must not be stolen by touch/pen scrolling. */
    touch-action: none;
  }
  .fname {
    flex: 0 0 180px;
    min-width: 0;
    /* Overrides .cv-ci__name's dim resting color: that class dims until its row is
       selected, and these rows have no selected state to brighten into. */
    color: var(--color-text);
  }
  .fdup {
    color: var(--color-live);
    border-color: var(--color-live);
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

  .fpanel {
    padding: 0 12px 8px;
    border-top: var(--border-weight) solid var(--color-border-2);
    background: var(--color-base);
  }
  .pgrid {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) 150px;
    gap: 12px;
  }
  .pfull {
    width: 100%;
  }
  .pfull.dup {
    border-color: var(--color-live);
  }
  .warn {
    color: var(--color-live);
  }
  .popt {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .popt + .popt {
    margin-top: 6px;
  }
  .popt input {
    flex: 1;
    min-width: 0;
  }
  .paddopt {
    height: 28px;
    margin-top: 8px;
  }
  .pfoot {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .pspacer {
    flex: 1;
  }
  /* Same box as the icon-only header action, widened to carry its label. */
  .pdel {
    width: auto;
    gap: 7px;
    padding: 0 12px;
  }
</style>
