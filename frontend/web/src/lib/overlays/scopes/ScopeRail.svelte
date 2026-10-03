<script lang="ts">
  // The scoped editor's list of scopes: Defaults, then each event, each event's variations
  // nested under it with their condition as the label, and an "Add variation" entry under
  // every event that has something to vary on. One listbox, so it is one Tab stop: arrow keys
  // move, Enter picks, Delete removes a variation (the editor offers the undo).
  //
  // Below the editor's wide breakpoint it renders as a select instead, with the add and
  // delete actions beside it.
  import Button from "$lib/ui/Button.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import IconButton, { ICONBTN_ROW } from "$lib/ui/IconButton.svelte";
  import {
    conditionProblem,
    DEFAULT_SCOPE,
    scopeLabel,
    type AlertScopes,
  } from "../../../overlay/alertScopes";

  let {
    scopes,
    selected,
    variant = "rail",
    onSelect,
    onAdd,
    onDelete,
  }: {
    scopes: AlertScopes;
    selected: string;
    variant?: "rail" | "select";
    onSelect: (scope: string) => void;
    onAdd: (eventKey: string) => void;
    onDelete: (variationId: string) => void;
  } = $props();

  type Item =
    | { kind: "scope"; id: string; label: string; depth: 0 | 1; dot: boolean; problem: string | null }
    | { kind: "add"; id: string; event: string; label: string; depth: 1 };

  const items = $derived.by<Item[]>(() => {
    const out: Item[] = [{ kind: "scope", id: DEFAULT_SCOPE, label: "Defaults", depth: 0, dot: false, problem: null }];
    for (const e of scopes.events) {
      const own = scopes.overrides[e.key];
      out.push({ kind: "scope", id: e.key, label: e.label, depth: 0, dot: !!own && Object.keys(own).length > 0, problem: null });
      for (const v of scopes.variations) {
        if (v.event === e.key) {
          out.push({
            kind: "scope",
            id: v.id,
            label: scopeLabel(scopes, v.id),
            depth: 1,
            dot: false,
            problem: conditionProblem(v.when, e),
          });
        }
      }
      if (e.conditions.length > 0) {
        out.push({ kind: "add", id: `add:${e.key}`, event: e.key, label: "Add variation", depth: 1 });
      }
    }
    return out;
  });

  const isVariation = (id: string): boolean => scopes.variations.some((v) => v.id === id);

  const uid = $props.id();
  let active = $state(0);
  // The cursor follows a selection made from outside (the narrow select, an add), so arrow
  // keys continue from what is shown as selected rather than from a stale position.
  $effect(() => {
    const at = items.findIndex((it) => it.id === selected);
    if (at >= 0) {
      active = at;
    }
  });

  function activate(it: Item): void {
    if (it.kind === "add") {
      onAdd(it.event);
    } else {
      onSelect(it.id);
    }
  }

  function onKey(e: KeyboardEvent): void {
    const last = items.length - 1;
    const moves: Record<string, number> = { ArrowDown: active + 1, ArrowUp: active - 1, Home: 0, End: last };
    if (e.key in moves) {
      e.preventDefault();
      active = Math.min(Math.max(moves[e.key], 0), last);
      document.getElementById(`${uid}-opt-${active}`)?.scrollIntoView({ block: "nearest" });
      return;
    }
    const it = items[active];
    if (!it) {
      return;
    }
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      activate(it);
    } else if ((e.key === "Delete" || e.key === "Backspace") && it.kind === "scope" && isVariation(it.id)) {
      e.preventDefault();
      onDelete(it.id);
    }
  }

  const selectedEvent = $derived(
    scopes.variations.find((v) => v.id === selected)?.event ?? scopes.events.find((e) => e.key === selected)?.key ?? null,
  );
  const canAdd = $derived(!!selectedEvent && (scopes.events.find((e) => e.key === selectedEvent)?.conditions.length ?? 0) > 0);
</script>

{#if variant === "rail"}
  <ul
    class="rail"
    role="listbox"
    tabindex="0"
    aria-label="Alert scopes"
    aria-activedescendant="{uid}-opt-{active}"
    onkeydown={onKey}
  >
    {#each items as it, i (it.id)}
      <!-- Following SavedItemPicker: the <li> is presentational so each option reads as a
           child of the listbox, and the delete button sits beside its option rather than in
           it, where an option's presentational content would hide it. -->
      <li class="rail__row" role="presentation">
        <button
          type="button"
          id="{uid}-opt-{i}"
          class="rail__item"
          class:rail__item--sub={it.depth === 1}
          class:rail__item--on={it.id === selected}
          class:rail__item--cursor={i === active}
          class:rail__add={it.kind === "add"}
          role="option"
          tabindex="-1"
          aria-selected={it.id === selected}
          onclick={() => {
            active = i;
            activate(it);
          }}
        >
          {#if it.kind === "add"}
            <Icon name="plus" size={10} />
          {/if}
          <span class="rail__label">{it.label}</span>
          {#if it.kind === "scope" && it.dot}
            <span class="rail__dot" aria-hidden="true"></span><span class="sr-only">, customized</span>
          {/if}
          {#if it.kind === "scope" && it.problem}
            <span class="rail__warn" title={it.problem}><Icon name="warn" size={12} /></span>
            <span class="sr-only">, never plays: {it.problem}</span>
          {/if}
        </button>
        {#if it.kind === "scope" && isVariation(it.id)}
          <IconButton
            {...ICONBTN_ROW}
            icon="trash"
            danger
            aria-label="Delete variation {it.label}"
            title="Delete (Del)"
            onclick={() => onDelete(it.id)}
          />
        {/if}
      </li>
    {/each}
  </ul>
{:else}
  <div class="pick">
    <label class="pick__label" for="{uid}-select">Editing</label>
    <select id="{uid}-select" class="cv-select pick__select" value={selected} onchange={(e) => onSelect(e.currentTarget.value)}>
      {#each items as it (it.id)}
        {#if it.kind === "scope"}
          <option value={it.id}>{it.depth === 1 ? " " : ""}{it.label}{it.dot ? " •" : ""}{it.problem ? " (never plays)" : ""}</option>
        {/if}
      {/each}
    </select>
    {#if canAdd && selectedEvent}
      <Button size="sm" variant="dashed" onclick={() => onAdd(selectedEvent)}>Add variation</Button>
    {/if}
    {#if isVariation(selected)}
      <Button size="sm" tone="live" onclick={() => onDelete(selected)}>Delete variation</Button>
    {/if}
  </div>
{/if}

<style>
  .rail {
    list-style: none;
    margin: 0;
    padding: 4px 0;
    height: 100%;
    overflow-y: auto;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .rail:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .rail__row {
    display: flex;
    align-items: center;
    padding-right: 4px;
  }
  .rail__item {
    flex: 1;
    min-width: 0;
    height: auto;
    background: transparent;
    border: 0;
    text-align: left;
    display: flex;
    align-items: center;
    gap: 6px;
    min-height: 28px;
    padding: 4px 10px;
    border-left: 2px solid transparent;
    color: var(--color-dim);
    font-size: 12px;
    cursor: pointer;
  }
  .rail__row:has(.rail__item--on) {
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  .rail__item:hover {
    color: var(--color-text);
  }
  .rail__item--sub {
    padding-left: 24px;
    font-size: 11px;
  }
  .rail__item--on {
    border-left-color: var(--color-accent);
    color: var(--color-text);
  }
  /* The keyboard cursor, drawn only while the list has focus so a mouse user never sees two
     highlights. */
  .rail:focus-visible .rail__item--cursor {
    outline: 1px dashed var(--color-accent);
    outline-offset: -3px;
  }
  .rail__label {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .rail__dot {
    flex: 0 0 auto;
    width: 6px;
    height: 6px;
    background: var(--color-accent);
  }
  .rail__warn {
    flex: 0 0 auto;
    display: inline-flex;
    color: var(--color-warn);
  }
  .rail__add {
    color: var(--color-muted);
  }
  .pick {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }
  .pick__label {
    font-size: 11px;
    color: var(--color-dim);
  }
  .pick__select {
    flex: 1;
    min-width: 160px;
    height: var(--control-height);
  }
</style>
