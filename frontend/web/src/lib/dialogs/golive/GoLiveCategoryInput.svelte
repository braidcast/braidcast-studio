<script lang="ts" module>
  // Each instance needs a listbox id of its own: the go-live modal and the schedule
  // form both render one per destination, and a shared id would point every input's
  // aria-controls at the first list on the page.
  let nextWidgetId = 0;
</script>

<script lang="ts">
  import { onDestroy } from "svelte";
  import { obs, type StreamCategory } from "$lib/api/bridge";

  // Debounced category typeahead (mock `Category ▾`). Calls
  // streamMeta.searchCategories({providerId, query}) ~250ms after the last
  // keystroke and shows matches in a dropdown; selecting one reports {id,name}.
  interface Props {
    providerId: string;
    /** The channel this control edits. Forwarded because a provider whose lookup is
     * account-scoped (Facebook's Pages) returns a different list per account, so omitting
     * it would answer from whichever account of the platform happens to be first. */
    accountId?: string;
    value: { id: string; name: string } | null;
    onChange: (v: { id: string; name: string } | null) => void;
    /** Prompt shown in the empty field; the catalog wording when unset. */
    placeholder?: string;
    /** The category this control inherits when it holds none of its own. Shown as ordinary
     * text, not as a muted prompt: it is the category that will be sent, so it reads the
     * way a chosen one does and the row's "↳ using …" tag carries the distinction. Typing
     * over it searches as usual; leaving it alone keeps the field inheriting. */
    inheritedName?: string;
    /** The provider's choices are a short list, not a catalog: look them up on focus with
     * an empty query so they can be browsed. Off by default — a catalog search rejects an
     * empty query, and asking for one would spend a request that can only fail. */
    browsable?: boolean;
  }
  let {
    providerId,
    accountId = "",
    value,
    onChange,
    placeholder = "",
    inheritedName = "",
    browsable = false,
  }: Props = $props();

  const prompt = $derived(placeholder.trim() || "Search category…");

  let query = $state("");
  let results = $state<StreamCategory[]>([]);
  let open = $state(false);
  let loading = $state(false);
  let rootEl = $state<HTMLDivElement | null>(null);
  let timer: ReturnType<typeof setTimeout> | null = null;
  let seq = 0;

  // Highlighted result, driven by the arrow keys. -1 = nothing highlighted, which is
  // also what a fresh keystroke returns to.
  let active = $state(-1);
  const widgetId = `cat-${nextWidgetId++}`;
  const listId = `${widgetId}-list`;
  const optionId = (i: number): string => `${widgetId}-opt-${i}`;

  // Keep the visible text in sync if the value is set externally (e.g. prefill), and show
  // the inherited category the same way when this layer holds none — the field is still
  // inheriting (nothing is written back), it just reads as what it will send.
  $effect(() => {
    query = value?.name ?? inheritedName;
  });

  function schedule(): void {
    if (timer !== null) {
      clearTimeout(timer);
    }
    const q = query.trim();
    // Emptying the box drops the selection. Without this the id survives the text
    // that named it, so the field reads as holding nothing while a category is
    // still pushed -- and an id is the only part a provider reads.
    if (q === "" && value !== null) {
      onChange(null);
    }
    if (q === "" && !browsable) {
      results = [];
      active = -1;
      open = false;
      return;
    }
    timer = setTimeout(() => void run(q), 250);
  }

  async function run(q: string): Promise<void> {
    const mine = ++seq;
    loading = true;
    try {
      const res = await obs.call("streamMeta.searchCategories", { providerId, accountId, query: q });
      if (mine !== seq) {
        return; // a newer keystroke superseded this request
      }
      results = res;
      active = res.length > 0 ? 0 : -1;
      open = true;
    } catch {
      if (mine === seq) {
        results = [];
        active = -1;
        open = false;
      }
    } finally {
      if (mine === seq) {
        loading = false;
      }
    }
  }

  function pick(c: StreamCategory): void {
    onChange({ id: c.id, name: c.name });
    query = c.name;
    open = false;
    results = [];
    active = -1;
  }

  // The list was reachable by mouse only. Arrow keys move the highlight, Enter takes
  // it and Escape abandons it, so a category can be chosen without a pointer.
  function onInputKeydown(e: KeyboardEvent): void {
    if (e.key === "ArrowDown" && !open && results.length > 0) {
      open = true;
      active = 0;
      e.preventDefault();
      return;
    }
    if (!open || results.length === 0) {
      return;
    }
    if (e.key === "ArrowDown") {
      active = (active + 1) % results.length;
    } else if (e.key === "ArrowUp") {
      active = (active - 1 + results.length) % results.length;
    } else if (e.key === "Enter" && active >= 0) {
      pick(results[active]);
    } else if (e.key === "Escape") {
      // Stops here rather than bubbling to the modal: the first Escape closes the
      // list, and only a second one closes the dialog behind it.
      open = false;
      active = -1;
      e.stopPropagation();
    } else {
      return;
    }
    e.preventDefault();
  }

  // Close on a click outside the widget rather than on input blur: an input-blur
  // close fires the moment the pointer leaves the field to grab the dropdown's
  // scrollbar, collapsing the list mid-scroll. A click-outside check keeps the
  // menu open while the user interacts with it (scrollbar drag, wheel, item click).
  $effect(() => {
    if (!open) {
      return;
    }
    const onDoc = (e: MouseEvent): void => {
      if (rootEl && !rootEl.contains(e.target as Node)) {
        open = false;
      }
    };
    document.addEventListener("mousedown", onDoc);
    return () => document.removeEventListener("mousedown", onDoc);
  });

  onDestroy(() => {
    if (timer !== null) {
      clearTimeout(timer);
    }
  });
</script>

<div class="cat" bind:this={rootEl}>
  <input
    class="inp"
    type="text"
    role="combobox"
    aria-autocomplete="list"
    aria-expanded={open && results.length > 0}
    aria-controls={listId}
    aria-activedescendant={open && active >= 0 ? optionId(active) : undefined}
    placeholder={prompt}
    bind:value={query}
    oninput={schedule}
    onkeydown={onInputKeydown}
    onfocus={() => {
      if (results.length) {
        open = true;
      } else if (browsable) {
        schedule();
      }
    }}
  />
  {#if open && results.length}
    <ul class="menu" id={listId} role="listbox">
      {#each results as c, i (c.id)}
        <li>
          <button
            type="button"
            id={optionId(i)}
            role="option"
            aria-selected={i === active}
            class:active={i === active}
            onmousedown={() => pick(c)}>{c.name}</button
          >
        </li>
      {/each}
    </ul>
  {/if}
  {#if loading}<span class="spin">…</span>{/if}
</div>

<style>
  .cat {
    position: relative;
  }
  .inp {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    box-sizing: border-box;
    font: inherit;
    font-size: 12px;
  }
  .inp:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .menu {
    list-style: none;
    margin: 0;
    padding: 0;
    position: absolute;
    top: 100%;
    left: 0;
    right: 0;
    z-index: 5;
    max-height: 180px;
    overflow: auto;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
  }
  .menu button {
    display: block;
    width: 100%;
    text-align: left;
    background: none;
    border: none;
    color: var(--color-text);
    font: inherit;
    font-size: 12px;
    padding: 6px 10px;
    cursor: pointer;
  }
  .menu button:hover,
  .menu button.active {
    background: color-mix(in srgb, var(--color-accent) 16%, transparent);
    color: var(--color-accent);
  }
  .spin {
    position: absolute;
    right: 8px;
    top: 7px;
    color: var(--color-muted);
    font-size: 12px;
  }
</style>
