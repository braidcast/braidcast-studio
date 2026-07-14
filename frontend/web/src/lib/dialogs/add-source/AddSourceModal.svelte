<script lang="ts">
  import Modal from "$lib/ui/Modal.svelte";
  import Icon, { type IconName } from "$lib/ui/Icon.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import { obs, type SourceType, type ExistingSource } from "$lib/api/bridge";
  import { recentSources, pushRecent } from "$lib/dialogs/add-source/addSourceRecent.svelte";
  import { bucketTypes, categoryOf, CATEGORY_LABEL, type Category } from "$lib/dialogs/add-source/addSourceCategories";

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

  // Modal.svelte owns the preview suspension for its whole lifetime; this modal must
  // not self-suspend on top of it (a second, separately-released suspension would race
  // the modal's on close and leave the preview stuck hidden).

  let types = $state<SourceType[]>([]);
  let existingSources = $state<ExistingSource[]>([]);
  let existingNames = $state<Set<string>>(new Set());
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let creating = $state(false);

  // Rail selection: "recent" | a functional Category. Detail step: the picked type.
  type Rail = "recent" | Category;
  let rail = $state<Rail>("recent");
  let selectedType = $state<SourceType | null>(null);
  // In the detail step: "existing" references an existing source; "new" creates one.
  let mode = $state<"existing" | "new">("new");
  let name = $state("");
  let pickedExisting = $state<string>("");

  async function load() {
    error = null;
    try {
      const [list, existing] = await Promise.all([
        obs.call("sourceTypes.list"),
        obs.call("sources.listExisting", { canvas, scene }),
      ]);
      types = list;
      existingSources = existing;
      existingNames = new Set(existing.map((s) => s.name.toLowerCase()));
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }
  $effect(() => {
    void load();
  });

  const buckets = $derived(bucketTypes(types));
  // Resolve the MRU type ids against the live type list (skip unregistered ones).
  const recentTypes = $derived(
    recentSources.ids.map((id) => types.find((t) => t.id === id)).filter((t): t is SourceType => t !== undefined),
  );
  // Rail tabs: Recently Used ONLY when it has entries (an empty one is an
  // unselectable dead tab), then each non-empty functional category.
  const railTabs = $derived<Rail[]>([
    ...(recentTypes.length > 0 ? (["recent"] as Rail[]) : []),
    ...buckets.map((b) => b.category),
  ]);
  // Types shown in the right pane for the active rail tab.
  const paneTypes = $derived<SourceType[]>(
    rail === "recent" ? recentTypes : (buckets.find((b) => b.category === rail)?.types ?? []),
  );

  // The initial rail state is "recent"; when it's empty (and thus hidden), fall the
  // selection through to the first functional category so the pane is never blank.
  $effect(() => {
    if (loaded && rail === "recent" && recentTypes.length === 0 && buckets.length > 0) {
      rail = buckets[0].category;
    }
  });

  // Existing instances of the picked type (candidates not already in the scene).
  const existingOfType = $derived(selectedType ? existingSources.filter((s) => s.typeId === selectedType!.id) : []);

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
    error = null;
    // Spec: default to Create-new even when existing instances exist; preselect the
    // first existing so switching the radio to "Use existing" already has a choice.
    mode = "new";
    const firstExisting = existingSources.find((s) => s.typeId === t.id);
    pickedExisting = firstExisting ? firstExisting.name : "";
    name = uniqueName(t.name);
  }
  function backToPicker() {
    selectedType = null;
    error = null;
  }

  const trimmed = $derived(name.trim());
  const nameTaken = $derived(trimmed.length > 0 && existingNames.has(trimmed.toLowerCase()));
  const nameValid = $derived(trimmed.length > 0 && !nameTaken);

  // Add a reference to an already-created source into this canvas's current scene.
  async function addExisting(sourceName: string) {
    if (creating) return;
    creating = true;
    error = null;
    try {
      const created = await obs.call("sources.addExisting", { canvas, scene, name: sourceName });
      onCreated(created);
    } catch (e) {
      error = (e as Error).message;
      creating = false;
    }
  }
  async function create() {
    if (!selectedType || !nameValid || creating) return;
    creating = true;
    error = null;
    try {
      const created = await obs.call("sources.create", { type: selectedType.id, name: trimmed, canvas, scene });
      pushRecent(selectedType.id);
      onCreated(created);
    } catch (e) {
      error = (e as Error).message;
      creating = false;
    }
  }
  function confirmDetail() {
    if (mode === "existing") {
      if (pickedExisting) void addExisting(pickedExisting);
    } else {
      void create();
    }
  }

  // Source-type icon by capability (video+audio media, video-only, audio-only, other).
  function typeIcon(caps: { video: boolean; audio: boolean }): IconName {
    if (caps.video && caps.audio) return "film";
    if (caps.video) return "image";
    if (caps.audio) return "audio-wave";
    return "puzzle";
  }

  // Modal owns Escape. Enter confirms the active detail mode.
  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Enter" && selectedType) {
      if (mode === "new" && nameValid) void create();
      else if (mode === "existing" && pickedExisting) void addExisting(pickedExisting);
    }
  }

  const detailValid = $derived(mode === "existing" ? pickedExisting.length > 0 : nameValid);
</script>

<svelte:window onkeydown={onKeydown} />

<Modal title={selectedType ? `Add — ${selectedType.name}` : "Add Source"} {onClose} width={620}>
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Loading source types…</p>
  {:else if selectedType}
    <div class="detail">
      <button class="back" onclick={backToPicker}><Icon name="submenu" size={10} /> Back</button>
      {#if existingOfType.length > 0}
        <label class="radio"><input type="radio" value="existing" bind:group={mode} /> Use existing</label>
        {#if mode === "existing"}
          <ul class="existing">
            {#each existingOfType as src (src.name)}
              <li>
                <label class="exrow">
                  <input type="radio" value={src.name} bind:group={pickedExisting} />
                  <span class="glyph"><Icon name={typeIcon(src.caps)} size={15} /></span>
                  <span class="type-name">{src.name}</span>
                </label>
              </li>
            {/each}
          </ul>
        {/if}
        <label class="radio"><input type="radio" value="new" bind:group={mode} /> Create new</label>
      {/if}
      {#if mode === "new"}
        <div class="name-step">
          <label for="src-name">Name</label>
          <!-- svelte-ignore a11y_autofocus -->
          <input id="src-name" type="text" bind:value={name} autofocus spellcheck="false" placeholder="Source name" />
          {#if nameTaken}<p class="warn">A source named “{trimmed}” already exists.</p>{/if}
        </div>
      {/if}
      <div class="actions">
        <button class="accent" disabled={!detailValid || creating} onclick={confirmDetail}>
          {creating ? "Adding…" : mode === "existing" ? "Add" : "Create"}
        </button>
      </div>
    </div>
  {:else}
    <div class="picker">
      <div class="rail" role="tablist" aria-label="Source categories">
        {#each railTabs as tab (tab)}
          <button
            class="railtab"
            class:on={rail === tab}
            role="tab"
            aria-selected={rail === tab}
            onclick={() => (rail = tab)}>{CATEGORY_LABEL[tab]}</button
          >
        {/each}
      </div>
      <div class="pane">
        {#if paneTypes.length === 0}
          <EmptyState compact title={rail === "recent" ? "No recently used sources" : "No sources in this category"} />
        {:else}
          <ul class="types">
            {#each paneTypes as t (t.id)}
              <li>
                <button class="type" onclick={() => pickType(t)}>
                  <span class="glyph"><Icon name={typeIcon(t.caps)} size={16} /></span>
                  <span class="type-name">{t.name}</span>
                </button>
              </li>
            {/each}
          </ul>
        {/if}
      </div>
    </div>
  {/if}
</Modal>

<style>
  .picker {
    display: flex;
    min-height: 320px;
    max-height: 60vh;
  }
  .rail {
    flex: 0 0 150px;
    display: flex;
    flex-direction: column;
    border-right: var(--border-weight) solid var(--color-border);
    overflow: auto;
  }
  .railtab {
    text-align: left;
    background: none;
    border: 0;
    border-bottom: var(--border-weight) solid var(--color-border);
    color: var(--color-muted);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    padding: 10px 12px;
    cursor: pointer;
  }
  .railtab:hover {
    color: var(--color-text);
    background: var(--color-surface);
  }
  .railtab.on {
    color: var(--color-text);
    background: var(--color-surface-2);
    box-shadow: inset 3px 0 0 var(--color-accent);
  }
  .pane {
    flex: 1;
    min-width: 0;
    overflow: auto;
  }
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
    padding: 9px 12px;
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

  .detail {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .back {
    align-self: flex-start;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    background: none;
    border: 0;
    color: var(--color-muted);
    font-size: 11px;
    padding: 2px 0;
  }
  .back:hover {
    color: var(--color-text);
    border: 0;
  }
  .radio {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: var(--color-text);
    cursor: pointer;
  }
  .existing {
    list-style: none;
    margin: 0 0 0 22px;
    padding: 0;
    border: var(--border-weight) solid var(--color-border);
    max-height: 220px;
    overflow: auto;
  }
  .existing li {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .existing li:last-child {
    border-bottom: 0;
  }
  .exrow {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 7px 9px;
    cursor: pointer;
    font-size: 12px;
    color: var(--color-dim);
  }
  .exrow:hover {
    background: var(--color-surface);
    color: var(--color-text);
  }
  .name-step {
    display: flex;
    flex-direction: column;
    gap: 6px;
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
