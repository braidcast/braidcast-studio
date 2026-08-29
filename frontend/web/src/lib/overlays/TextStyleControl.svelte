<script lang="ts">
  // The value control for a `textstyle` field: one editor for the whole nested style
  // object a slot is styled through.
  //
  // Rows come from STYLE_PROPS, the same table the overlay runtime compiles to CSS, so
  // every property offered here reaches the stream. There are more than twenty of them,
  // which is why the control opens closed and then shows one group at a time -- a flat
  // list of every property would bury the four or five a user actually reaches for.
  //
  // A property is either SET (its key is present in the object) or absent, and absent
  // means "leave it to the widget's own fields". Clearing a row deletes its key rather
  // than writing a neutral value, so emptying every row returns the object to {} and
  // withOverride drops the whole setting.
  import {
    dependencyFor,
    dependentsOf,
    STYLE_GROUPS,
    STYLE_PROPS,
    type StyleGroup,
    type StyleOption,
    type StyleProp,
  } from "../../overlay/textStyle";
  import CssColorInput from "$lib/ui/CssColorInput.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Segmented from "$lib/ui/Segmented.svelte";
  import { clamp } from "$lib/utils/clamp";

  let {
    value,
    name,
    fontListId,
    ariaDescribedBy,
    onChange,
  }: {
    value: Record<string, unknown>;
    name: string;
    fontListId: string;
    /** Id of an element describing this control, for a form that renders help text. */
    ariaDescribedBy?: string;
    onChange: (next: Record<string, unknown>) => void;
  } = $props();

  let open = $state(false);
  let tab = $state<StyleGroup>("typography");

  const uid = $props.id();
  const bodyId = `${uid}-body`;

  function isSet(key: string): boolean {
    return Object.hasOwn(value, key);
  }

  // Always a fresh object: `value` can be the schema's own default object when the field
  // carries no override yet, and mutating that would edit the schema every widget of this
  // type reads.
  //
  // The pair below keep one invariant: a row that reads as set has an effect on the
  // stream. Some properties only mean something beside a companion -- a shadow offset with
  // no shadow color, an outline color with no outline width -- and COMPOSITES is where
  // those pairings are declared. Setting a part supplies its gate; clearing a gate takes
  // its parts with it, rather than leaving rows lit over a widget that never changed.
  // Neither side names a property here, so a pairing added there is handled by both.
  function put(key: string, v: unknown): void {
    const next = { ...value, [key]: v };
    const dep = dependencyFor(key);
    if (dep && !Object.hasOwn(next, dep.key)) {
      next[dep.key] = dep.value;
    }
    onChange(next);
  }

  function drop(key: string): void {
    const next = { ...value };
    for (const k of [key, ...dependentsOf(key)]) {
      delete next[k];
    }
    onChange(next);
  }

  const setCount = $derived(STYLE_PROPS.filter((p) => isSet(p.key)).length);
  // One string for the toggle's visible text and its accessible name, so the two cannot
  // drift apart and the count stays part of the name a speech-input user says.
  const summary = $derived(setCount > 0 ? `${setCount} set` : "Inherits widget style");
  const rows = $derived(STYLE_PROPS.filter((p) => p.group === tab));

  // The count rides the tab label so a property set under a group the user is not looking
  // at is still visible from the collapsed side of the control.
  const tabs = $derived(
    STYLE_GROUPS.map((g) => {
      const n = STYLE_PROPS.filter((p) => p.group === g.id && isSet(p.key)).length;
      return { value: g.id, label: n > 0 ? `${g.label} (${n})` : g.label };
    }),
  );

  function selectTab(v: string): void {
    const group = STYLE_GROUPS.find((g) => g.id === v);
    if (group) {
      tab = group.id;
    }
  }

  // --- per-kind accessors (the template branches on kind; these keep the narrowing out
  // of the markup, where a union member's extras are awkward to reach) ---
  function bounds(p: StyleProp): { min: number; max: number; step: number } {
    return p.kind === "length" || p.kind === "ratio" || p.kind === "percent"
      ? { min: p.min, max: p.max, step: p.step }
      : { min: 0, max: 100, step: 1 };
  }
  function unitOf(p: StyleProp): string {
    if (p.kind === "length") {
      return p.unit;
    }
    return p.kind === "percent" ? "%" : p.kind === "ratio" ? "×" : "";
  }
  function optionsOf(p: StyleProp): StyleOption[] {
    return p.kind === "enum" ? p.options : [];
  }
  function seedOf(p: StyleProp): string {
    return p.kind === "color" ? p.seed : "#ffffff";
  }

  function asText(v: unknown): string {
    return v == null ? "" : String(v);
  }

  // Number rows keep a draft of the text being typed, separate from the number stored.
  // Storage is clamped on every keystroke, so the stored value and the value the runtime
  // renders can never disagree however the row loses focus -- abandoning a half-typed
  // 9999 leaves 400 behind, not 9999. The draft is what keeps that from being felt: bind
  // the input to the store and clamping mid-entry rewrites the field under the typist --
  // a first keystroke of 1 in Size, which floors at 4, would snap to 4 and put 10 out of
  // reach. The input shows the draft until the entry is committed, then re-reads the store
  // and settles on whatever the clamp allowed.
  let draft = $state<Record<string, string>>({});

  function shownValue(p: StyleProp): string {
    return draft[p.key] ?? (isSet(p.key) ? asText(value[p.key]) : "");
  }

  function typeNumber(p: StyleProp, input: HTMLInputElement): void {
    // A partial entry ("-", "1e") leaves `.value` empty while `badInput` is set. Taking
    // that for a clear would delete the key AND write the empty string back over the
    // character just typed, so the draft is left untouched and the DOM keeps it.
    if (input.validity.badInput) {
      return;
    }
    draft[p.key] = input.value;
    if (input.value === "") {
      drop(p.key);
      return;
    }
    const n = Number(input.value);
    if (!Number.isFinite(n)) {
      return;
    }
    const b = bounds(p);
    put(p.key, clamp(n, b.min, b.max));
  }

  // Blur or Enter. Dropping the draft is the whole job: the store already holds the
  // clamped number, and the input re-renders from it.
  function commitNumber(p: StyleProp): void {
    delete draft[p.key];
  }
</script>

<div class="ts">
  <button
    type="button"
    class="ts__toggle"
    aria-expanded={open}
    aria-controls={bodyId}
    aria-describedby={ariaDescribedBy}
    aria-label="{summary} — {name} style properties"
    onclick={() => (open = !open)}
  >
    <Icon name={open ? "caret-down" : "caret-right"} size={11} />
    <span class="ts__summary">{summary}</span>
  </button>

  <!-- Rendered whether or not it is open, so aria-controls above always resolves to a
       real element rather than dangling while the control is collapsed. -->
  <div class="ts__body" id={bodyId} hidden={!open}>
    {#if open}
      <Segmented options={tabs} value={tab} onChange={selectTab} />

      <ul class="ts__rows">
        {#each rows as p (p.key)}
          {@const set = isSet(p.key)}
          {@const id = `${uid}-${p.key}`}
          {@const b = bounds(p)}
          <li class="ts__row" class:ts__row--set={set}>
            {#if p.kind === "color"}
              <span class="ts__name">{p.label}</span>
            {:else}
              <label class="ts__name" for={id}>{p.label}</label>
            {/if}

            <div class="ts__val">
              {#if p.kind === "color"}
                {#if set}
                  <CssColorInput
                    value={asText(value[p.key])}
                    ariaLabel="{name} {p.label}"
                    onChange={(v) => put(p.key, v)}
                  />
                {:else}
                  <!-- The visible text stays short for the row; the name it is addressed
                       by carries the slot, and still contains that text verbatim. -->
                  <button
                    type="button"
                    class="ts__seed"
                    aria-label="Set {p.label.toLowerCase()} for {name}"
                    onclick={() => put(p.key, seedOf(p))}>Set {p.label.toLowerCase()}</button
                  >
                {/if}
              {:else if p.kind === "enum"}
                <select
                  {id}
                  class="ts__select"
                  value={set ? asText(value[p.key]) : ""}
                  onchange={(e) => (e.currentTarget.value === "" ? drop(p.key) : put(p.key, e.currentTarget.value))}
                >
                  <option value="">Inherit</option>
                  {#each optionsOf(p) as o (o.value)}
                    <option value={o.value}>{o.label}</option>
                  {/each}
                </select>
              {:else if p.kind === "font"}
                <input
                  {id}
                  class="ts__text"
                  type="text"
                  list={fontListId}
                  placeholder="Inherit"
                  value={set ? asText(value[p.key]) : ""}
                  oninput={(e) => (e.currentTarget.value === "" ? drop(p.key) : put(p.key, e.currentTarget.value))}
                />
              {:else}
                <input
                  {id}
                  class="ts__num"
                  type="number"
                  min={b.min}
                  max={b.max}
                  step={b.step}
                  placeholder="Inherit"
                  value={shownValue(p)}
                  oninput={(e) => typeNumber(p, e.currentTarget)}
                  onchange={() => commitNumber(p)}
                  onblur={() => commitNumber(p)}
                />
                <span class="ts__unit">{unitOf(p)}</span>
              {/if}
            </div>

            <div class="ts__reset">
              {#if set}
                <button
                  type="button"
                  class="tool-btn"
                  aria-label="Clear {name} {p.label}"
                  title="Clear — inherit the widget's own setting"
                  onclick={() => drop(p.key)}
                >
                  <Icon name="x" size={11} />
                </button>
              {/if}
            </div>
          </li>
        {/each}
      </ul>
    {/if}
  </div>
</div>

<style>
  .ts {
    display: flex;
    flex-direction: column;
    gap: 8px;
    width: 100%;
    min-width: 0;
  }

  .ts__toggle {
    align-self: flex-start;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 8px;
    background: var(--color-base);
    color: var(--color-dim);
    transition: color 160ms ease;
  }
  .ts__toggle:hover {
    color: var(--color-text);
  }
  .ts__summary {
    font-family: var(--font-mono);
    font-size: 11px;
  }

  /* The body stays mounted so aria-controls resolves, which leaves `hidden` to take it out
     of the layout -- and `hidden` alone cannot, against the display: flex below. Hence the
     attribute selector, which outranks it. */
  .ts__body[hidden] {
    display: none;
  }
  .ts__body {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 8px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }

  .ts__rows {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .ts__row {
    display: flex;
    align-items: center;
    gap: 8px;
    /* Same marker the outer field rows carry, at the same width whether it is lit or
       not, so a row does not shift as it gains its value. */
    border-left: 2px solid transparent;
    padding-left: 6px;
  }
  .ts__row--set {
    border-left-color: var(--color-accent);
  }
  .ts__name {
    flex: 0 0 108px;
    min-width: 0;
    font-size: 11px;
    color: var(--color-dim);
  }
  .ts__row--set .ts__name {
    color: var(--color-text);
  }
  .ts__val {
    flex: 1;
    min-width: 0;
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .ts__reset {
    flex: 0 0 25px;
    display: flex;
    justify-content: flex-end;
  }
  .ts__text,
  .ts__select {
    width: 100%;
  }
  .ts__num {
    flex: 0 0 96px;
  }
  .ts__unit {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .ts__seed {
    padding: 0 10px;
    font-size: 11px;
    color: var(--color-dim);
  }

  @media (prefers-reduced-motion: reduce) {
    .ts__toggle {
      transition: none;
    }
  }
</style>
