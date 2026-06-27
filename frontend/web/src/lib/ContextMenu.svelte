<script lang="ts" module>
  // One entry in a cursor-positioned context menu. A null item renders a divider.
  // `danger` paints the label in the live/destructive color; `disabled` is inert.
  export interface ContextMenuItem {
    label: string;
    action?: () => void;
    danger?: boolean;
    disabled?: boolean;
    // A checkable item renders a leading ✓ when `checked`. `action` toggles it.
    checked?: boolean;
    // A submenu item: hovering opens a flyout of these children. `action` is
    // ignored when `children` is present.
    children?: (ContextMenuItem | null)[];
  }
</script>

<script lang="ts">
  // A reusable right-click popup. Positioned at (x, y) in viewport coordinates,
  // clamped to stay on screen. Mirrors Menu.svelte's dropdown look (same tokens,
  // same box-shadow). Auto-closes on outside click (deferred so the opening
  // right-click doesn't dismiss it), Escape, resize, and scroll.
  import Self from "./ContextMenu.svelte";

  let {
    x,
    y,
    items,
    onClose,
  }: { x: number; y: number; items: (ContextMenuItem | null)[]; onClose: () => void } = $props();

  // Reserve the leading tick gutter only when the menu has at least one checkable
  // item, so plain flat menus render with their original left padding unchanged.
  const hasCheckable = $derived(items.some((it) => it && "checked" in it));

  let openSub = $state<number | null>(null);
  let subPos = $state<{ x: number; y: number }>({ x: 0, y: 0 });

  function openSubmenu(i: number, el: HTMLElement) {
    const r = el.getBoundingClientRect();
    subPos = { x: r.right - 2, y: r.top };
    openSub = i;
  }

  let menuEl = $state<HTMLDivElement | undefined>();
  // Set once by the measure effect below (clamped into the viewport). Hidden until
  // then so it never flashes at the origin before placement.
  let left = $state(0);
  let top = $state(0);
  let ready = $state(false);

  function run(item: ContextMenuItem) {
    if (item.disabled) {
      return;
    }
    item.action?.();
    onClose();
  }

  // Measure once after mount and clamp into the viewport. The flag guards against
  // the effect re-running (and re-clamping off already-adjusted values).
  let measured = false;
  $effect(() => {
    if (measured || !menuEl) {
      return;
    }
    measured = true;
    const margin = 4;
    const r = menuEl.getBoundingClientRect();
    let nx = x;
    let ny = y;
    if (nx + r.width > window.innerWidth - margin) {
      nx = Math.max(margin, window.innerWidth - r.width - margin);
    }
    if (ny + r.height > window.innerHeight - margin) {
      ny = Math.max(margin, window.innerHeight - r.height - margin);
    }
    left = nx;
    top = ny;
    ready = true;
  });

  // Dismissal listeners, all torn down together.
  $effect(() => {
    const close = () => onClose();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        onClose();
      }
    };
    // Defer the document click so the opening right-click doesn't close it.
    const id = setTimeout(() => document.addEventListener("click", close), 0);
    document.addEventListener("keydown", onKey);
    window.addEventListener("resize", close);
    document.addEventListener("scroll", close, true);
    return () => {
      clearTimeout(id);
      document.removeEventListener("click", close);
      document.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", close);
      document.removeEventListener("scroll", close, true);
    };
  });
</script>

<div
  class="context-menu"
  role="menu"
  bind:this={menuEl}
  style:left="{left}px"
  style:top="{top}px"
  style:visibility={ready ? "visible" : "hidden"}
>
  {#each items as item, i (item ? item.label : "div-" + i)}
    {#if item === null}
      <div class="divider"></div>
    {:else if item.children}
      <div
        class="item submenu"
        class:disabled={item.disabled}
        role="menuitem"
        tabindex="-1"
        aria-haspopup="true"
        onmouseenter={(e) => openSubmenu(i, e.currentTarget as HTMLElement)}
      >
        {#if hasCheckable}
          <span class="tick"></span>
        {/if}
        <span class="lbl">{item.label}</span>
        <span class="arrow">▸</span>
      </div>
    {:else}
      <button
        class="item"
        class:disabled={item.disabled}
        class:danger={item.danger}
        role="menuitemcheckbox"
        aria-checked={item.checked ? "true" : "false"}
        onclick={(e) => {
          e.stopPropagation();
          run(item);
        }}
      >
        {#if hasCheckable}
          <span class="tick">{item.checked ? "✓" : ""}</span>
        {/if}
        <span class="lbl">{item.label}</span>
      </button>
    {/if}
  {/each}
</div>

{#if openSub !== null && items[openSub] && items[openSub]!.children}
  <Self x={subPos.x} y={subPos.y} items={items[openSub]!.children!} onClose={onClose} />
{/if}

<style>
  .context-menu {
    position: fixed;
    z-index: 200;
    min-width: 180px;
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
    padding: 6px 12px;
    text-align: left;
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .item.submenu {
    justify-content: space-between;
    cursor: default;
  }
  .item.submenu:hover {
    background: var(--color-base);
  }
  .tick {
    flex: 0 0 12px;
    text-align: center;
    color: var(--color-accent);
  }
  .lbl {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .arrow {
    flex: 0 0 auto;
    color: var(--color-dim);
    font-size: 9px;
  }
  .item:hover {
    background: var(--color-base);
    border: 0;
  }
  .item.danger {
    color: var(--color-live);
  }
  .item.disabled {
    color: var(--color-muted);
    cursor: default;
  }
  .item.disabled:hover {
    background: transparent;
  }
  .divider {
    height: 1px;
    background: var(--color-border);
    margin: 3px 0;
  }
</style>
