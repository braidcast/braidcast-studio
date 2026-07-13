<script lang="ts">
  import Icon from "./Icon.svelte";

  // Space-saving filter: a search-icon button that toggles a one-line input inline
  // in a list toolbar. Stays open while it holds text so a live filter is never
  // hidden. `value` is bindable so the host dock filters its own list.
  interface Props {
    value: string;
    placeholder?: string;
  }
  let { value = $bindable(""), placeholder = "Filter…" }: Props = $props();

  let open = $state(false);
  const shown = $derived(open || value.trim().length > 0);

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
  }

  function toggle() {
    open = !open;
    if (!open) {
      value = "";
    }
  }

  function onKey(e: KeyboardEvent) {
    if (e.key === "Escape") {
      value = "";
      open = false;
    }
  }
</script>

<div class="filter-reveal" class:shown>
  <button
    class="tool-btn"
    title={shown ? "Hide filter" : "Filter…"}
    aria-label={shown ? "Hide filter" : "Filter…"}
    aria-pressed={shown}
    onclick={toggle}
  >
    <Icon name="search" size={13} />
  </button>
  {#if shown}
    <input class="filter-input" bind:value {placeholder} onkeydown={onKey} use:focusOnMount />
  {/if}
</div>
