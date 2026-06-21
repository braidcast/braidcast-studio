<script lang="ts">
  import { obs, type SourceType } from "./bridge";
  import { suspendPreview } from "./previewGate.svelte";

  interface Props {
    /** Focused canvas uuid; the source is added into this canvas's current scene. */
    canvas: string | null;
    /** Target scene name (the canvas's current scene). */
    scene: string | null;
    /** Called after a successful create with the new sceneitem id + source name. */
    onCreated: (created: { id: number; source: string }) => void;
    /** Close without creating. */
    onClose: () => void;
  }
  let { canvas, scene, onCreated, onClose }: Props = $props();

  // Hide the native preview overlay while this modal is open.
  $effect(() => suspendPreview());

  let types = $state<SourceType[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Two-step flow: pick a type, then name it.
  let selectedType = $state<SourceType | null>(null);
  let name = $state("");
  let creating = $state(false);

  // Names already taken, so the name step can flag collisions without a round-trip.
  let existingNames = $state<Set<string>>(new Set());

  async function load() {
    error = null;
    try {
      const [list, existing] = await Promise.all([
        obs.call("sourceTypes.list"),
        obs.call("sources.listExisting", { canvas, scene }),
      ]);
      types = list;
      existingNames = new Set(existing.map((n) => n.toLowerCase()));
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
  });

  // Suffix " N" until the name is free (matches OBS de-dup).
  function uniqueName(base: string): string {
    if (!existingNames.has(base.toLowerCase())) return base;
    for (let n = 2; ; n++) {
      const candidate = `${base} ${n}`;
      if (!existingNames.has(candidate.toLowerCase())) return candidate;
    }
  }

  function pickType(t: SourceType) {
    selectedType = t;
    name = uniqueName(t.name);
  }

  function backToPicker() {
    selectedType = null;
    error = null;
  }

  const trimmed = $derived(name.trim());
  const nameTaken = $derived(trimmed.length > 0 && existingNames.has(trimmed.toLowerCase()));
  const nameValid = $derived(trimmed.length > 0 && !nameTaken);

  async function create() {
    if (!selectedType || !nameValid || creating) return;
    creating = true;
    error = null;
    try {
      const created = await obs.call("sources.create", {
        type: selectedType.id,
        name: trimmed,
        canvas,
        scene,
      });
      onCreated(created);
    } catch (e) {
      error = (e as Error).message;
      creating = false;
    }
  }

  function typeGlyph(t: SourceType): string {
    if (t.caps.video && t.caps.audio) return "🎬";
    if (t.caps.video) return "🖼";
    if (t.caps.audio) return "🔊";
    return "🧩";
  }

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      onClose();
    } else if (e.key === "Enter" && selectedType && nameValid) {
      void create();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Add source">
    <header class="modal-head">
      <h3>{selectedType ? `Add — ${selectedType.name}` : "Add Source"}</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="modal-body">
      {#if error}
        <p class="error">{error}</p>
      {/if}

      {#if !selectedType}
        {#if !loaded}
          <p class="dim">Loading source types…</p>
        {:else if types.length === 0}
          <p class="dim">No source types available</p>
        {:else}
          <ul class="types">
            {#each types as t (t.id)}
              <li>
                <button class="type" onclick={() => pickType(t)}>
                  <span class="glyph">{typeGlyph(t)}</span>
                  <span class="type-name">{t.name}</span>
                </button>
              </li>
            {/each}
          </ul>
        {/if}
      {:else}
        <div class="name-step">
          <label for="src-name">Name</label>
          <!-- svelte-ignore a11y_autofocus -->
          <input
            id="src-name"
            type="text"
            bind:value={name}
            autofocus
            spellcheck="false"
            placeholder="Source name"
          />
          {#if nameTaken}
            <p class="warn">A source named “{trimmed}” already exists.</p>
          {/if}
          <div class="actions">
            <button class="btn ghost" onclick={backToPicker}>Back</button>
            <button class="btn primary" disabled={!nameValid || creating} onclick={() => void create()}>
              {creating ? "Creating…" : "Create"}
            </button>
          </div>
        </div>
      {/if}
    </div>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--bg-raised);
    border: 1px solid var(--border);
    border-radius: 12px;
    width: min(460px, 100%);
    max-height: 80vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 18px;
    border-bottom: 1px solid var(--border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 14px;
    font-weight: 600;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .modal-body {
    padding: 14px 18px 18px;
    overflow: auto;
  }
  .icon {
    background: none;
    border: none;
    color: var(--text-dim);
    cursor: pointer;
    padding: 2px 4px;
    border-radius: 4px;
    font-size: 14px;
    line-height: 1;
  }
  .icon:hover {
    color: var(--text);
    background: var(--bg-sunken);
  }
  .types {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .type {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    text-align: left;
    background: none;
    border: 1px solid transparent;
    border-radius: 8px;
    padding: 8px 10px;
    cursor: pointer;
    color: var(--text-soft);
    font: inherit;
  }
  .type:hover {
    background: var(--bg-sunken);
    color: var(--text);
    border-color: var(--border);
  }
  .glyph {
    font-size: 16px;
    width: 22px;
    text-align: center;
  }
  .type-name {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .name-step {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .name-step label {
    font-size: 12px;
    color: var(--text-dim);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }
  input {
    background: var(--bg-sunken);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    color: var(--text);
    font: inherit;
  }
  input:focus {
    outline: none;
    border-color: var(--accent);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 6px;
  }
  .btn {
    border-radius: 6px;
    padding: 7px 14px;
    font: inherit;
    cursor: pointer;
    border: 1px solid var(--border);
    background: var(--bg-sunken);
    color: var(--text-soft);
  }
  .btn:hover:not(:disabled) {
    color: var(--text);
  }
  .btn.primary {
    background: var(--accent);
    border-color: var(--accent);
    color: #fff;
  }
  .btn.primary:disabled {
    opacity: 0.45;
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .dim {
    color: var(--text-dim);
    margin: 0;
  }
  .warn {
    color: var(--off, #d65a5a);
    margin: 0;
    font-size: 12px;
  }
  .error {
    color: var(--off, #d65a5a);
    margin: 0 0 10px;
    font-size: 12px;
  }
</style>
