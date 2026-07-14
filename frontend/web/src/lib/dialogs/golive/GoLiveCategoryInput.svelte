<script lang="ts">
  import { onDestroy } from "svelte";
  import { obs, type StreamCategory } from "$lib/api/bridge";

  // Debounced category typeahead (mock `Category ▾`). Calls
  // streamMeta.searchCategories({providerId, query}) ~250ms after the last
  // keystroke and shows matches in a dropdown; selecting one reports {id,name}.
  interface Props {
    providerId: string;
    value: { id: string; name: string } | null;
    onChange: (v: { id: string; name: string } | null) => void;
  }
  let { providerId, value, onChange }: Props = $props();

  let query = $state("");
  let results = $state<StreamCategory[]>([]);
  let open = $state(false);
  let loading = $state(false);
  let rootEl = $state<HTMLDivElement | null>(null);
  let timer: ReturnType<typeof setTimeout> | null = null;
  let seq = 0;

  // Keep the visible text in sync if the value is set externally (e.g. prefill).
  $effect(() => {
    query = value?.name ?? "";
  });

  function schedule(): void {
    if (timer !== null) {
      clearTimeout(timer);
    }
    const q = query.trim();
    if (q === "") {
      results = [];
      open = false;
      return;
    }
    timer = setTimeout(() => void run(q), 250);
  }

  async function run(q: string): Promise<void> {
    const mine = ++seq;
    loading = true;
    try {
      const res = await obs.call("streamMeta.searchCategories", { providerId, query: q });
      if (mine !== seq) {
        return; // a newer keystroke superseded this request
      }
      results = res;
      open = true;
    } catch {
      if (mine === seq) {
        results = [];
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
    placeholder="Search category…"
    bind:value={query}
    oninput={schedule}
    onfocus={() => {
      if (results.length) open = true;
    }}
  />
  {#if open && results.length}
    <ul class="menu">
      {#each results as c (c.id)}
        <li>
          <button type="button" onmousedown={() => pick(c)}>{c.name}</button>
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
  .menu button:hover {
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
