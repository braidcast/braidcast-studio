<script lang="ts">
  import { obs, type TransitionType, type TransitionState } from "../bridge";
import { EV } from "../eventNames";

  // The active scene transition: a type dropdown + a duration (ms) field, wired
  // to transitionTypes.list / transitions.getCurrent / setCurrent / setDuration
  // and re-fetched on the transitions.changed push. Edits are optimistic; a
  // failed call surfaces the error and reverts via a fresh getCurrent.
  // The mount adapter strips internal __* keys; this dock declares no props.
  let {}: Record<string, unknown> = $props();

  let types = $state<TransitionType[]>([]);
  let current = $state<TransitionState | null>(null);
  let error = $state<string | null>(null);

  async function refresh() {
    try {
      current = await obs.call("transitions.getCurrent");
      error = null;
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
    </div>
  {/if}
</div>

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
</style>
