<script lang="ts">
  import Modal from "./Modal.svelte";
  import Icon, { type IconName } from "./dock/Icon.svelte";
  import EmptyState from "./EmptyState.svelte";
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

  // Source-type icon by capability (video+audio media, video-only, audio-only, other).
  function typeIcon(t: SourceType): IconName {
    if (t.caps.video && t.caps.audio) return "film";
    if (t.caps.video) return "image";
    if (t.caps.audio) return "audio-wave";
    return "puzzle";
  }

  // Modal owns Escape. Enter creates once a valid name is entered.
  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Enter" && selectedType && nameValid) {
      void create();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<Modal title={selectedType ? `Add — ${selectedType.name}` : "Add Source"} {onClose} width={460}>
  {#if error}
    <p class="error">{error}</p>
  {/if}

  {#if !selectedType}
    {#if !loaded}
      <p class="dim">Loading source types…</p>
    {:else if types.length === 0}
      <EmptyState compact title="No source types available" />
    {:else}
      <ul class="types">
        {#each types as t (t.id)}
          <li>
            <button class="type" onclick={() => pickType(t)}>
              <span class="glyph"><Icon name={typeIcon(t)} size={16} /></span>
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
      <input id="src-name" type="text" bind:value={name} autofocus spellcheck="false" placeholder="Source name" />
      {#if nameTaken}
        <p class="warn">A source named “{trimmed}” already exists.</p>
      {/if}
      <div class="actions">
        <button onclick={backToPicker}>Back</button>
        <button class="accent" disabled={!nameValid || creating} onclick={() => void create()}>
          {creating ? "Creating…" : "Create"}
        </button>
      </div>
    </div>
  {/if}
</Modal>

<style>
  .types {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
  }
  .types li {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .types li:last-child {
    border-bottom: 0;
  }
  .type {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    height: auto;
    text-align: left;
    background: none;
    border: 0;
    padding: 9px 8px;
    color: var(--color-dim);
    font-family: var(--font-ui);
    font-size: 12px;
  }
  .type:hover {
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
    color: var(--color-accent);
    border: 0;
  }
  .glyph {
    flex: 0 0 22px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    color: var(--color-muted);
  }
  .type:hover .glyph {
    color: var(--color-accent);
  }
  .type-name {
    flex: 1;
    min-width: 0;
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
    font-size: 11px;
    color: var(--color-muted);
    text-transform: uppercase;
    letter-spacing: var(--letter-spacing);
  }
  .name-step input {
    width: 100%;
  }
  .actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    margin-top: 6px;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
    font-size: 11px;
  }
  .warn {
    color: var(--color-live);
    margin: 0;
    font-size: 12px;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 10px;
    font-size: 12px;
  }
</style>
