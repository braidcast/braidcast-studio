<script lang="ts">
  import Modal from "./Modal.svelte";
  import { obs, type Transform, type TransformTarget, type TransformAction } from "./bridge";
import { EV } from "./eventNames";

  interface Props {
    target: TransformTarget;
    label: string;
    onClose: () => void;
  }
  let { target, label, onClose }: Props = $props();

  // The native preview overlay (a sibling HWND above CEF) is suspended by the opener
  // (openTransform) for this dialog's whole lifetime, so it never occludes the modal.
  // Suspending in the opener rather than here keeps the gate ref-count from
  // transiently hitting zero on a context-menu -> modal handoff.

  let xf = $state<Transform | null>(null);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // The 9-position alignment bitfield, shared by item alignment + bounds alignment.
  const ALIGNMENTS: { label: string; value: number }[] = [
    { label: "Top Left", value: 5 },
    { label: "Top Center", value: 4 },
    { label: "Top Right", value: 6 },
    { label: "Center Left", value: 1 },
    { label: "Center", value: 0 },
    { label: "Center Right", value: 2 },
    { label: "Bottom Left", value: 9 },
    { label: "Bottom Center", value: 8 },
    { label: "Bottom Right", value: 10 },
  ];

  const BOUNDS_TYPES: { label: string; value: number }[] = [
    { label: "No bounds", value: 0 },
    { label: "Stretch to bounds", value: 1 },
    { label: "Scale to inner bounds", value: 2 },
    { label: "Scale to outer bounds", value: 3 },
    { label: "Scale to width of bounds", value: 4 },
    { label: "Scale to height of bounds", value: 5 },
    { label: "Maximum size only", value: 6 },
  ];

  const ACTIONS: { label: string; action: TransformAction }[] = [
    { label: "Reset", action: "reset" },
    { label: "Center", action: "center" },
    { label: "Fit to Screen", action: "fitToScreen" },
    { label: "Stretch to Screen", action: "stretchToScreen" },
    { label: "Flip Horizontal", action: "flipH" },
    { label: "Flip Vertical", action: "flipV" },
  ];

  function report(e: unknown) {
    error = (e as Error).message;
  }

  // The push event/method params address the item with the same {canvas?,scene?,id}.
  function targetParams(): Record<string, unknown> {
    const p: Record<string, unknown> = { id: target.id };
    if (target.canvas != null) {
      p.canvas = target.canvas;
    }
    if (target.scene != null) {
      p.scene = target.scene;
    }
    return p;
  }

  async function load() {
    error = null;
    try {
      xf = await obs.call("sceneItems.getTransform", targetParams());
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  // (Re)load when the target changes. The dialog is remounted per open, but the
  // target can also change in place if a different item is opened while mounted.
  $effect(() => {
    void target.id;
    void target.canvas;
    void target.scene;
    loaded = false;
    void load();
  });

  // Reload on external edits (e.g. dragging in the preview) that concern this item.
  $effect(() => {
    return obs.on(EV.sceneItemsChanged, (p) => {
      const sameCanvas = (p.canvas ?? null) === (target.canvas ?? null);
      const sameScene = !p.scene || !target.scene || p.scene === target.scene;
      if (sameCanvas && sameScene) {
        void load();
      }
    });
  });

  // Apply a partial transform: optimistic local merge, then reconcile from the
  // authoritative returned transform. On failure, re-load to discard the optimism.
  async function commit(patch: Partial<Transform>) {
    if (!xf) {
      return;
    }
    const optimistic = { ...xf, ...patch };
    xf = optimistic;
    try {
      xf = await obs.call("sceneItems.setTransform", { ...targetParams(), transform: patch });
      error = null;
    } catch (e) {
      report(e);
      void load();
    }
  }

  async function runAction(action: TransformAction) {
    try {
      xf = await obs.call("sceneItems.transformAction", { ...targetParams(), action });
      error = null;
    } catch (e) {
      report(e);
    }
  }

  // Numeric input parsing. Empty / non-finite falls back to the current value so a
  // half-typed field never commits NaN. `int` clamps to integers; `min` floors it.
  function num(value: string, fallback: number, opts: { int?: boolean; min?: number } = {}): number {
    let n = Number(value);
    if (!Number.isFinite(n)) {
      n = fallback;
    }
    if (opts.int) {
      n = Math.round(n);
    }
    if (opts.min != null && n < opts.min) {
      n = opts.min;
    }
    return n;
  }

  // Enter on a numeric input fires `change` via blur-equivalent; commit explicitly
  // so Enter applies without losing focus.
  function onEnter(e: KeyboardEvent) {
    if (e.key === "Enter") {
      (e.target as HTMLInputElement).blur();
    }
  }

  const showBounds = $derived(xf != null && xf.boundsType !== 0);
</script>

<Modal title="Edit Transform — {label}" {onClose} width={560} draggable>
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading…</p>
  {:else if !xf}
    <p class="dim">No transform available.</p>
  {:else}
    <div class="actions">
      {#each ACTIONS as a (a.action)}
        <button onclick={() => void runAction(a.action)}>{a.label}</button>
      {/each}
    </div>

    <div class="grid">
      <div class="group">
        <div class="group-head">Position</div>
        <label>
          <span>X</span>
          <input
            type="number"
            value={xf.pos.x}
            onkeydown={onEnter}
            onchange={(e) => void commit({ pos: { x: num(e.currentTarget.value, xf!.pos.x), y: xf!.pos.y } })}
          />
        </label>
        <label>
          <span>Y</span>
          <input
            type="number"
            value={xf.pos.y}
            onkeydown={onEnter}
            onchange={(e) => void commit({ pos: { x: xf!.pos.x, y: num(e.currentTarget.value, xf!.pos.y) } })}
          />
        </label>
      </div>

      <div class="group">
        <div class="group-head">Rotation</div>
        <label>
          <span>Degrees</span>
          <input
            type="number"
            value={xf.rot}
            onkeydown={onEnter}
            onchange={(e) => void commit({ rot: num(e.currentTarget.value, xf!.rot) })}
          />
        </label>
      </div>

      <div class="group">
        <div class="group-head">Size</div>
        <label>
          <span>Scale X</span>
          <input
            type="number"
            step="0.01"
            value={xf.scale.x}
            onkeydown={onEnter}
            onchange={(e) => void commit({ scale: { x: num(e.currentTarget.value, xf!.scale.x), y: xf!.scale.y } })}
          />
          <em class="px">{Math.round(xf.sourceWidth * xf.scale.x)}px</em>
        </label>
        <label>
          <span>Scale Y</span>
          <input
            type="number"
            step="0.01"
            value={xf.scale.y}
            onkeydown={onEnter}
            onchange={(e) => void commit({ scale: { x: xf!.scale.x, y: num(e.currentTarget.value, xf!.scale.y) } })}
          />
          <em class="px">{Math.round(xf.sourceHeight * xf.scale.y)}px</em>
        </label>
      </div>

      <div class="group">
        <div class="group-head">Alignment</div>
        <label>
          <span>Position</span>
          <select value={xf.alignment} onchange={(e) => void commit({ alignment: Number(e.currentTarget.value) })}>
            {#each ALIGNMENTS as a (a.value)}
              <option value={a.value}>{a.label}</option>
            {/each}
          </select>
        </label>
      </div>

      <div class="group span2">
        <div class="group-head">Crop</div>
        <div class="row4">
          <label>
            <span>Left</span>
            <input
              type="number"
              min="0"
              value={xf.crop.left}
              onkeydown={onEnter}
              onchange={(e) =>
                void commit({ crop: { ...xf!.crop, left: num(e.currentTarget.value, xf!.crop.left, { int: true, min: 0 }) } })}
            />
          </label>
          <label>
            <span>Top</span>
            <input
              type="number"
              min="0"
              value={xf.crop.top}
              onkeydown={onEnter}
              onchange={(e) =>
                void commit({ crop: { ...xf!.crop, top: num(e.currentTarget.value, xf!.crop.top, { int: true, min: 0 }) } })}
            />
          </label>
          <label>
            <span>Right</span>
            <input
              type="number"
              min="0"
              value={xf.crop.right}
              onkeydown={onEnter}
              onchange={(e) =>
                void commit({ crop: { ...xf!.crop, right: num(e.currentTarget.value, xf!.crop.right, { int: true, min: 0 }) } })}
            />
          </label>
          <label>
            <span>Bottom</span>
            <input
              type="number"
              min="0"
              value={xf.crop.bottom}
              onkeydown={onEnter}
              onchange={(e) =>
                void commit({ crop: { ...xf!.crop, bottom: num(e.currentTarget.value, xf!.crop.bottom, { int: true, min: 0 }) } })}
            />
          </label>
        </div>
      </div>

      <div class="group span2">
        <div class="group-head">Bounding Box</div>
        <label>
          <span>Type</span>
          <select value={xf.boundsType} onchange={(e) => void commit({ boundsType: Number(e.currentTarget.value) })}>
            {#each BOUNDS_TYPES as b (b.value)}
              <option value={b.value}>{b.label}</option>
            {/each}
          </select>
        </label>
        {#if showBounds}
          <div class="row4">
            <label>
              <span>Width</span>
              <input
                type="number"
                value={xf.bounds.x}
                onkeydown={onEnter}
                onchange={(e) => void commit({ bounds: { x: num(e.currentTarget.value, xf!.bounds.x), y: xf!.bounds.y } })}
              />
            </label>
            <label>
              <span>Height</span>
              <input
                type="number"
                value={xf.bounds.y}
                onkeydown={onEnter}
                onchange={(e) => void commit({ bounds: { x: xf!.bounds.x, y: num(e.currentTarget.value, xf!.bounds.y) } })}
              />
            </label>
            <label class="span2">
              <span>Alignment</span>
              <select
                value={xf.boundsAlignment}
                onchange={(e) => void commit({ boundsAlignment: Number(e.currentTarget.value) })}
              >
                {#each ALIGNMENTS as a (a.value)}
                  <option value={a.value}>{a.label}</option>
                {/each}
              </select>
            </label>
          </div>
        {/if}
      </div>
    </div>
  {/if}

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .actions {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
    margin-bottom: 12px;
  }
  .actions button {
    height: auto;
    padding: 5px 10px;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    background: transparent;
  }
  .actions button:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }

  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
  }
  .group {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .group.span2 {
    grid-column: 1 / -1;
  }
  .group-head {
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: uppercase;
    color: var(--color-accent);
  }

  label {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 11px;
    color: var(--color-text);
  }
  label > span {
    flex: 0 0 64px;
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  label.span2 {
    grid-column: 1 / -1;
  }

  .row4 {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
  }
  .row4 label > span {
    flex: 0 0 52px;
  }

  .px {
    flex: 0 0 auto;
    font-style: normal;
    font-size: 10px;
    color: var(--color-muted);
  }

  input[type="number"],
  select {
    flex: 1;
    min-width: 0;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  input[type="number"]:focus,
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }

  .dim {
    color: var(--color-muted);
    margin: 0;
    padding: 8px;
    font-size: 11px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
</style>
