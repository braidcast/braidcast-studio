<script lang="ts">
  import { tick, untrack } from "svelte";
  import {
    bridgeErrorText,
    obs,
    type SceneDuplicateParams,
    type SceneDuplicateSources,
    type SceneFreeNameParams,
  } from "$lib/api/bridge";
  import Modal from "$lib/ui/Modal.svelte";
  import { canvasStore } from "$lib/stores/canvasStore.svelte";
  import { duplicateScenePref, setDuplicateSceneSources } from "$lib/stores/duplicateScenePrefStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import { RequestGuard } from "$lib/utils/requestGuard";

  interface Props {
    /** The scene to duplicate. */
    name: string;
    /** The canvas it lives on; undefined = the Default canvas. */
    canvas?: string;
    onClose: () => void;
  }
  let { name: sceneName, canvas, onClose }: Props = $props();

  canvasStore.start();

  let mounted = true;
  $effect(() => () => {
    mounted = false;
  });

  const uid = $props.id();
  const errId = `${uid}-name-err`;

  // The bridge addresses the Default canvas by omission, so every canvas is carried
  // here as a key: "" for Default, the uuid for any other. Comparing keys rather than
  // uuids keeps "same canvas" true while the canvas list is still loading.
  const defaultUuid = $derived(canvasStore.canvases.find((c) => c.isDefault)?.uuid ?? "");
  const keyOf = (uuid: string): string => (uuid === defaultUuid ? "" : uuid);
  const sourceUuid = $derived(canvas || defaultUuid);
  const sourceKey = $derived(keyOf(sourceUuid));

  let pickedUuid = $state<string | null>(null);
  const destUuid = $derived(pickedUuid ?? sourceUuid);
  const destKey = $derived(keyOf(destUuid));

  let newName = $state("");
  // Set by the first keystroke: from then on the field is the user's, and a canvas
  // change no longer re-fetches the suggested name over it.
  let edited = $state(false);
  let prefilling = $state(true);
  let sources = $state<SceneDuplicateSources>(duplicateScenePref.sources);
  let busy = $state(false);
  /** A host failure (the suggestion or the duplicate itself), shown at the Name field. */
  let hostError = $state("");
  /** The last taken-check answer, for the name and canvas it was asked about. */
  let checked = $state<{ name: string; key: string; taken: boolean } | null>(null);

  let nameEl = $state<HTMLInputElement | undefined>();

  const trimmed = $derived(newName.trim());
  const taken = $derived(checked !== null && checked.taken && checked.name === trimmed && checked.key === destKey);

  const nameError = $derived.by(() => {
    if (hostError) {
      return hostError;
    }
    if (trimmed === "") {
      return edited || !prefilling ? "Enter a name for the new scene." : "";
    }
    if (taken) {
      return `A scene or source named “${trimmed}” already exists on that canvas.`;
    }
    return "";
  });

  const canSubmit = $derived(!busy && !(prefilling && !edited) && trimmed !== "" && !taken);

  // Modal focuses its own first focusable (the header close button); the name is what
  // this dialog is for, so it takes focus instead. The suggestion selects it on arrival.
  $effect(() => {
    nameEl?.focus();
  });

  /** The name a scene called `name` would get on the canvas `key` addresses. */
  async function freeName(name: string, key: string): Promise<string> {
    const params: SceneFreeNameParams = { name, canvas: key || undefined };
    return (await obs.call("scenes.freeName", params)).name;
  }

  const prefillGuard = new RequestGuard();
  const takenGuard = new RequestGuard();

  // The suggested name for the chosen canvas, until the user types one of their own.
  $effect(() => {
    const key = destKey;
    untrack(() => {
      if (!edited) {
        void prefill(key);
      }
    });
  });

  async function prefill(key: string): Promise<void> {
    const current = prefillGuard.claim();
    prefilling = true;
    try {
      const suggested = await freeName(sceneName, key);
      if (!current() || edited) {
        return;
      }
      newName = suggested;
      await tick();
      if (nameEl && document.activeElement === nameEl) {
        nameEl.select();
      }
    } catch (e) {
      if (current() && !edited) {
        hostError = bridgeErrorText(e) || "Couldn't suggest a name.";
      }
    } finally {
      if (current()) {
        prefilling = false;
      }
    }
  }

  // Ask the host whether the typed name is free on the chosen canvas. The answer is
  // kept with the name and canvas it answers, so one that lands after the user moved
  // on can never flag the field; the guard drops it outright.
  $effect(() => {
    const name = trimmed;
    const key = destKey;
    if (name !== "") {
      untrack(() => void checkTaken(name, key));
    }
  });

  async function checkTaken(name: string, key: string): Promise<void> {
    const current = takenGuard.claim();
    try {
      const free = await freeName(name, key);
      if (current()) {
        checked = { name, key, taken: free !== name };
      }
    } catch {
      // Unanswered: the host refuses a taken name on submit anyway.
    }
  }

  function onNameInput(): void {
    edited = true;
    hostError = "";
  }

  function onCanvasChange(uuid: string): void {
    pickedUuid = uuid;
    hostError = "";
  }

  async function submit(): Promise<void> {
    if (!canSubmit) {
      return;
    }
    const requested = trimmed;
    const key = destKey;
    const params: SceneDuplicateParams = {
      name: sceneName,
      canvas: sourceKey || undefined,
      newName: requested,
      sources,
    };
    const elsewhere = key !== sourceKey;
    if (elsewhere) {
      params.destCanvas = destUuid;
    }
    const destName = elsewhere ? canvasStore.byUuid(destUuid)?.name : undefined;
    setDuplicateSceneSources(sources);
    busy = true;
    hostError = "";
    try {
      const r = await obs.call("scenes.duplicate", params);
      const to = destName ? ` to "${destName}"` : "";
      showToast(`Duplicated "${sceneName}"${to}`, r.name);
      if (mounted) {
        onClose();
      }
    } catch (e) {
      const why = bridgeErrorText(e) || "Duplicate failed.";
      if (!mounted) {
        // Closed while the duplicate was in flight, so there is no field left to show it at.
        showToast(`Duplicate failed: ${why}`, why);
        return;
      }
      busy = false;
      hostError = why;
      // Almost every refusal here is a name taken since the last check; re-asking marks
      // the field so the same name can't simply be resubmitted.
      void checkTaken(requested, key);
    }
  }

  function onEnter(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.isComposing) {
      e.preventDefault();
      void submit();
    }
  }
</script>

<Modal
  title={`Duplicate scene "${sceneName}"`}
  {onClose}
  width={400}
  cancel={{ label: "Cancel", onclick: onClose }}
  confirm={{ label: busy ? "Duplicating…" : "Duplicate", onclick: () => void submit(), disabled: !canSubmit }}
>
  <div class="cv-field">
    <label class="cv-field__l" for={`${uid}-name`}>Name</label>
    <input
      id={`${uid}-name`}
      class="wide"
      type="text"
      bind:this={nameEl}
      bind:value={newName}
      spellcheck="false"
      autocomplete="off"
      aria-invalid={nameError ? "true" : undefined}
      aria-describedby={nameError ? errId : undefined}
      oninput={onNameInput}
      onkeydown={onEnter}
    />
    <!-- Always mounted: a live region inserted together with its text is not reliably
         announced. -->
    <div aria-live="polite">
      {#if nameError}<p class="cv-field__err" id={errId}>{nameError}</p>{/if}
    </div>
  </div>

  <div class="cv-field">
    <label class="cv-field__l" for={`${uid}-canvas`}>Canvas</label>
    <select
      id={`${uid}-canvas`}
      class="wide"
      value={destUuid}
      onchange={(e) => onCanvasChange(e.currentTarget.value)}
    >
      {#each canvasStore.canvases as c (c.uuid)}
        <option value={c.uuid}>{c.uuid === sourceUuid ? `This canvas (${c.name})` : c.name}</option>
      {/each}
    </select>
  </div>

  <fieldset class="cv-field sources">
    <legend class="cv-field__l">Sources</legend>
    <div class="opt">
      <input
        id={`${uid}-copy`}
        type="radio"
        name={`${uid}-sources`}
        value="copy"
        bind:group={sources}
        aria-describedby={`${uid}-copy-h`}
        onkeydown={onEnter}
      />
      <label for={`${uid}-copy`}>Independent copies</label>
      <p class="cv-field__h" id={`${uid}-copy-h`}>
        Edit them without touching the original. Webcams, capture cards and audio stay shared.
        {#if destKey !== sourceKey}Copies with sound start muted on another canvas, so it isn't heard twice.{/if}
      </p>
    </div>
    <div class="opt">
      <input
        id={`${uid}-share`}
        type="radio"
        name={`${uid}-sources`}
        value="share"
        bind:group={sources}
        aria-describedby={`${uid}-share-h`}
        onkeydown={onEnter}
      />
      <label for={`${uid}-share`}>Shared with the original</label>
      <p class="cv-field__h" id={`${uid}-share-h`}>Same sources: an edit shows in both scenes.</p>
    </div>
  </fieldset>
</Modal>

<style>
  .wide {
    width: 100%;
  }
  /* A fieldset for the radiogroup semantics, without the browser's frame: .cv-field
     supplies the padding and the divider. */
  .sources {
    margin: 0;
    border: 0;
    min-width: 0;
  }
  .sources legend {
    padding: 0;
  }
  .opt {
    display: grid;
    grid-template-columns: auto 1fr;
    column-gap: 8px;
    align-items: center;
  }
  .opt + .opt {
    margin-top: 10px;
  }
  .opt label {
    font-size: 12px;
    color: var(--color-text);
    cursor: pointer;
  }
  .opt .cv-field__h {
    grid-column: 2;
    margin-top: 2px;
    margin-bottom: 0;
  }
</style>
