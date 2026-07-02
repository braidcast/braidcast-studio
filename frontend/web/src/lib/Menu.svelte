<script lang="ts" module>
  // A single menu-bar dropdown item. A null item renders a divider. Checkable
  // items show a leading mark; disabled items are inert.
  export interface MenuItem {
    label: string;
    action?: () => void;
    checked?: boolean;
    disabled?: boolean;
  }
</script>

<script lang="ts">
  import Icon from "./dock/Icon.svelte";

  // One menu-bar dropdown. `items` is a data list; clicking the label toggles open;
  // a document click closes. One component reused by every menu-bar entry.
  let { label, items }: { label: string; items: (MenuItem | null)[] } = $props();
  let open = $state(false);

  function toggle() {
    open = !open;
  }
  function run(item: MenuItem) {
    if (item.disabled) {
      return;
    }
    item.action?.();
    open = false;
  }
  $effect(() => {
    if (!open) {
      return;
    }
    const close = () => (open = false);
    // Defer so the opening click doesn't immediately close it.
    const id = setTimeout(() => document.addEventListener("click", close, { once: true }), 0);
    return () => {
      clearTimeout(id);
      document.removeEventListener("click", close);
    };
  });
</script>

<div class="menu">
  <button
    class="menu-label"
    class:open
    onclick={(e) => {
      e.stopPropagation();
      toggle();
    }}>{label}</button
  >
  {#if open}
    <div class="dropdown" role="menu">
      {#each items as item, i (item ? item.label : "div-" + i)}
        {#if item === null}
          <div class="divider"></div>
        {:else}
          <button
            class="item"
            class:disabled={item.disabled}
            role="menuitem"
            onclick={(e) => {
              e.stopPropagation();
              run(item);
            }}
          >
            <span class="mark">{#if item.checked}<Icon name="check" size={11} />{/if}</span>{item.label}
          </button>
        {/if}
      {/each}
    </div>
  {/if}
</div>

<style>
  .menu {
    position: relative;
  }
  .menu-label {
    background: transparent;
    border: 0;
    height: auto;
    padding: 4px 8px;
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .menu-label.open,
  .menu-label:hover {
    color: var(--color-accent);
    border: 0;
  }
  .dropdown {
    position: absolute;
    top: 100%;
    left: 0;
    z-index: 100;
    min-width: 200px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    display: flex;
    flex-direction: column;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.6);
  }
  .item {
    background: transparent;
    border: 0;
    height: auto;
    padding: 6px 10px;
    text-align: left;
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    display: flex;
    gap: 6px;
    align-items: center;
  }
  .item:hover {
    background: var(--color-base);
    border: 0;
  }
  .item.disabled {
    color: var(--color-muted);
    cursor: default;
  }
  .item.disabled:hover {
    background: transparent;
  }
  .mark {
    width: 12px;
    height: 11px;
    flex: 0 0 12px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    color: var(--color-accent);
  }
  .divider {
    height: 1px;
    background: var(--color-border);
    margin: 3px 0;
  }
</style>
