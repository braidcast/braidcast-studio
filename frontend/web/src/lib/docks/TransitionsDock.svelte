<script lang="ts">
  import { obs, type TransitionType, type TransitionState } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";
  import PropertiesModal from "$lib/properties/PropertiesModal.svelte";

  // The active scene transition: a type dropdown + a duration (ms) field, wired
  // to transitionTypes.list / transitions.getCurrent / setCurrent / setDuration
  // and re-fetched on the transitions.changed push. Edits are optimistic; a
  // failed call surfaces the error and reverts via a fresh getCurrent.
  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  let types = $state<TransitionType[]>([]);
  let current = $state<TransitionState | null>(null);
  let error = $state<string | null>(null);
  // Whether the active transition exposes any configurable properties. Fade/Cut
  // have none, so the Properties affordance is gated off for them; Stinger,
  // Fade-to-Color and Luma-Wipe report props and enable it.
  let hasProps = $state(false);
  let showProps = $state(false);

  // Re-evaluate hasProps for whatever transition is now active. Called after the
  // native state settles (initial load, transitions.changed, a committed type
  // switch) so it reads the current transition's real property set, not the
  // pre-swap one.
  async function checkProps() {
    try {
      const res = await obs.call("properties.get", { kind: "transition", ref: "" });
      hasProps = res.props.length > 0;
    } catch {
      hasProps = false;
    }
  }

  async function refresh() {
    try {
      current = await obs.call("transitions.getCurrent");
      error = null;
      void checkProps();
    } catch (e) {
      error = (e as Error).message;
    }
  }

  $effect(() => {
    void (async () => {
      try {
        const [t, c] = await Promise.all([obs.call("transitionTypes.list"), obs.call("transitions.getCurrent")]);
        types = t;
        current = c;
        error = null;
        void checkProps();
      } catch (e) {
        error = (e as Error).message;
      }
    })();
    const offChanged = obs.on(EV.transitionsChanged, () => void refresh());
    return () => {
      offChanged();
    };
  });

  async function onTypeChange(e: Event) {
    if (!current) {
      return;
    }
    const id = (e.currentTarget as HTMLSelectElement).value;
    current.id = id; // optimistic
    try {
      const res = await obs.call("transitions.setCurrent", { id });
      current.id = res.id;
      current.name = res.name;
      error = null;
      void checkProps();
    } catch (err) {
      error = (err as Error).message;
      void refresh(); // revert to the authoritative value
    }
  }

  async function commitDuration(e: Event) {
    if (!current) {
      return;
    }
    const durationMs = Math.max(0, Math.round(Number((e.currentTarget as HTMLInputElement).value)));
    current.durationMs = durationMs; // optimistic
    try {
      const res = await obs.call("transitions.setDuration", { durationMs });
      current.durationMs = res.durationMs;
      error = null;
    } catch (err) {
      error = (err as Error).message;
      void refresh();
    }
  }

  function onDurationKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      (e.currentTarget as HTMLInputElement).blur();
    }
  }
</script>

<div class="dock-body">
  {#if error}
    <p class="dock-msg err">{error}</p>
  {/if}

  {#if !current}
    <p class="dock-msg">Loading…</p>
  {:else}
    <div class="form">
      <label class="field">
        <span class="lbl">Transition</span>
        <select class="control" value={current.id} onchange={onTypeChange}>
          {#each types as t (t.id)}
            <option value={t.id}>{t.name}</option>
          {/each}
        </select>
      </label>

      <label class="field">
        <span class="lbl">Duration (ms)</span>
        <input
          class="control"
          type="number"
          min="0"
          step="50"
          value={current.durationMs}
          onchange={commitDuration}
          onkeydown={onDurationKey}
        />
      </label>

      <button
        class="control btn"
        disabled={!hasProps}
        title={hasProps ? "Edit transition properties" : "This transition has no properties"}
        onclick={() => (showProps = true)}
      >
        Properties…
      </button>
    </div>
  {/if}
</div>

{#if showProps && current}
  <PropertiesModal
    kind="transition"
    ref=""
    title={"Properties — " + current.name}
    onClose={() => (showProps = false)}
  />
{/if}

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 8px;
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .lbl {
    font-size: 10px;
    color: var(--color-muted);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .control {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  .control:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .btn {
    cursor: pointer;
    text-align: center;
  }
  .btn:hover:not(:disabled) {
    border-color: var(--color-accent);
  }
  .btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
</style>
