<script lang="ts" module>
  // One entry in a cursor-positioned context menu. A null item renders a divider.
  // `danger` paints the label in the live/destructive color; `disabled` is inert.
  export interface ContextMenuItem {
    label: string;
    action?: () => void;
    danger?: boolean;
    disabled?: boolean;
    // A checkable item renders a leading check mark when `checked`. `action` toggles it.
    checked?: boolean;
    // A color-tag item renders a leading filled square in this color (empty string
    // = a hollow "none" square). Purely cosmetic; combines with `checked`.
    swatch?: string;
    // A submenu item: hovering opens a flyout of these children. `action` is
    // ignored when `children` is present.
    children?: ContextMenuItems;
  }

  /** A menu's contents, dividers included. */
  export type ContextMenuItems = (ContextMenuItem | null)[];

  /** An open menu: what to render and the viewport point to render it at. Every
   * surface that opens one holds exactly this, so `menu` can be spread straight
   * into the component. */
  export interface ContextMenuState {
    x: number;
    y: number;
    items: ContextMenuItems;
  }
</script>

<script lang="ts">
  // A reusable right-click popup. Positioned at (x, y) in viewport coordinates,
  // clamped to stay on screen. Shares the shell's dropdown look (same tokens, same
  // box-shadow). Auto-closes on outside click (deferred so the opening right-click
  // doesn't dismiss it), Escape, resize, and scroll.
  import Self from "$lib/menus/ContextMenu.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import { isEditable } from "$lib/utils/editableTarget";
  import { pushEsc, popEsc, isTopEsc } from "$lib/utils/escStack";
  import { suspendPreview } from "$lib/stores/previewGate.svelte";

  let {
    x,
    y,
    items,
    onClose,
    onBack,
    focusToken = 0,
    returnFocus,
  }: ContextMenuState & {
    onClose: () => void;
    /** Passed to a flyout by the level that owns it: ArrowLeft hands focus back to the
     * parent row. Absent on a root menu, where there is nothing to go back to. */
    onBack?: () => void;
    /** Bumped by the parent on a KEYBOARD open, so the flyout takes focus; reset to 0
     * on a hover open, which must leave the mouse user's focus where it is. */
    focusToken?: number;
    /** The root's opener, handed down so every level restores focus to the same
     * element rather than to the parent row, which closes along with it. */
    returnFocus?: HTMLElement | null;
  } = $props();

  // What holds focus as this menu opens is the context a keyboard user has to land back
  // on when it closes. Read at init, before the menu itself can move focus; the body is
  // not a place to return anyone to.
  const opener = document.activeElement;
  const openerFocus = opener instanceof HTMLElement && opener !== document.body ? opener : null;
  const focusReturn = $derived(returnFocus ?? openerFocus);

  // Whether this level ever pulled focus to itself. A mouse click on a row leaves that
  // row focused too, so the focused element alone cannot tell the two apart, and only
  // the keyboard user is owed a focus hand-back.
  let tookFocus = false;

  // Reserve the leading tick gutter only when the menu has at least one checkable
  // item, so plain flat menus render with their original left padding unchanged.
  const hasCheckable = $derived(items.some((it) => it && "checked" in it));

  let openSub = $state<number | null>(null);
  let subPos = $state<{ x: number; y: number }>({ x: 0, y: 0 });
  let subFocusToken = $state(0);
  // The row arrow keys are currently on, null until focus has landed in this level.
  let focusIndex = $state<number | null>(null);

  // Rows a keyboard can land on: separators (null items) and disabled rows are skipped.
  const navIndices = $derived(items.flatMap((it, i) => (it && !it.disabled ? [i] : [])));

  // Every key an open menu claims for itself, whether or not it has somewhere to move.
  const ARROW_KEYS = new Set(["ArrowDown", "ArrowUp", "ArrowLeft", "ArrowRight"]);

  // Roving focus: exactly one row is tabbable -- the one focus is on, else the first
  // navigable row, which also re-anchors the tab stop if the items change underneath.
  const tabRow = $derived.by(() => {
    if (focusIndex !== null && navIndices.includes(focusIndex)) {
      return focusIndex;
    }
    return navIndices.length > 0 ? navIndices[0] : -1;
  });

  // Rows are looked up by index at keypress time rather than collected into a ref
  // array: the each block is keyed by index, so a shorter items array would leave a
  // detached element behind in one.
  function rowEl(i: number): HTMLElement | null {
    return menuEl?.querySelector<HTMLElement>(`[data-row="${i}"]`) ?? null;
  }

  function focusRow(i: number): void {
    tookFocus = true;
    focusIndex = i;
    // Moving along this level abandons the flyout the previous row had open, the same
    // way hovering a sibling row does.
    openSub = null;
    rowEl(i)?.focus();
  }

  function step(from: number, delta: number): void {
    if (navIndices.length === 0) {
      return;
    }
    const at = navIndices.indexOf(from);
    if (at === -1) {
      // Focus can sit on a row arrow keys never choose -- a disabled row accepts a
      // click -- so carry on in the pressed direction from where it actually is.
      const onward = delta > 0 ? navIndices.find((n) => n > from) : navIndices.filter((n) => n < from).at(-1);
      focusRow(onward ?? (delta > 0 ? navIndices[0] : navIndices[navIndices.length - 1]));
      return;
    }
    focusRow(navIndices[(at + delta + navIndices.length) % navIndices.length]);
  }

  function openSubmenu(i: number, el: HTMLElement, fromKeyboard = false) {
    const r = el.getBoundingClientRect();
    subPos = { x: r.right - 2, y: r.top };
    openSub = i;
    // Only a keyboard open changes the token, so a flyout re-opened by hover after a
    // keyboard open carries no focus request and cannot pull focus off the pointer.
    subFocusToken = fromKeyboard ? subFocusToken + 1 : 0;
  }

  function openSubmenuFromKey(i: number): void {
    const el = rowEl(i);
    if (!el) {
      return;
    }
    focusIndex = i;
    openSubmenu(i, el, true);
  }

  function onRowKey(e: KeyboardEvent, i: number): void {
    const item = items[i];
    if (!item) {
      return;
    }
    switch (e.key) {
      case "ArrowDown":
        step(i, 1);
        break;
      case "ArrowUp":
        step(i, -1);
        break;
      case "ArrowRight":
        if (item.children && !item.disabled) {
          openSubmenuFromKey(i);
        }
        break;
      case "ArrowLeft":
        onBack?.();
        break;
      case "Enter":
      case " ":
        // A flyout parent is a div with no native activation of its own. A plain row is
        // a button, so its click handler stays the single activation path -- handling
        // the key here too would run the action twice.
        if (!item.children) {
          return;
        }
        if (!item.disabled) {
          openSubmenuFromKey(i);
        }
        break;
      default:
        return;
    }
    // An arrow that reached the app-wide handler on `window` would nudge the selected
    // scene item as well, and the enter-the-menu handler below must not act on a key a
    // row has already answered.
    e.preventDefault();
    e.stopPropagation();
  }

  // A flyout opened from the keyboard takes focus. The menu is visibility:hidden until
  // the placement effect below lands and a hidden element cannot be focused, so the
  // focus waits for the frame that placement paints in.
  $effect(() => {
    if (focusToken === 0) {
      return;
    }
    const id = requestAnimationFrame(() => {
      if (navIndices.length > 0) {
        focusRow(navIndices[0]);
      }
    });
    return () => cancelAnimationFrame(id);
  });

  // A fresh menu (new items array from the caller) starts with no flyout open.
  $effect(() => {
    items;
    openSub = null;
  });

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

  // The native preview is a child HWND the OS composites ABOVE the whole CEF window,
  // so no z-index can put this menu over it -- the overlay has to be hidden instead.
  // Held HERE rather than by whoever opened the menu, because every opener having to
  // remember was not a rule anything enforced: four of the six surfaces that open a
  // menu suspended, and the audio mixer's and the scene list's menus were drawn under
  // the preview for as long as they have existed. Owning it here makes the coverage a
  // property of the component instead of a habit, so a new menu cannot reintroduce
  // the bug. Ref-counted, so a submenu level and any suspension its opener still
  // holds simply nest.
  $effect(() => suspendPreview());

  // Dismissal listeners, all torn down together. The Escape token gates so a menu
  // opened over a modal (or a submenu over its parent) only closes the topmost layer.
  $effect(() => {
    const token = pushEsc();
    const close = () => onClose();
    const onKey = (e: KeyboardEvent) => {
      if (!isTopEsc(token)) {
        return;
      }
      if (e.key === "Escape") {
        onClose();
        return;
      }
      // Opening the menu does not move focus (a right-click must not take it off what
      // was clicked), so the first Down/Up press is what moves focus onto a row. A key
      // pressed on a row is answered there and stopped before it reaches this.
      if (!ARROW_KEYS.has(e.key) || navIndices.length === 0) {
        return;
      }
      // A caret in a text field keeps its arrows -- the same carve-out the app-wide
      // handler makes, and a menu can open over a focused field (the native preview
      // raises one without touching DOM focus). A row that has focus answers its own.
      if (isEditable(e.target) || menuEl?.contains(document.activeElement)) {
        return;
      }
      e.preventDefault();
      // The app-wide handler on `window` sits past `document` in the bubble path, so
      // stopping here is what keeps arrows aimed at an open menu from also nudging the
      // selected scene item. Left/Right have nowhere to go until focus is on a row, yet
      // they are still the menu's keys to answer. Same-stage listeners -- the other
      // levels of a nested menu -- are unaffected.
      e.stopPropagation();
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        focusRow(e.key === "ArrowDown" ? navIndices[0] : navIndices[navIndices.length - 1]);
      }
    };
    // Defer the document click so the opening right-click doesn't close it.
    const id = setTimeout(() => document.addEventListener("click", close), 0);
    document.addEventListener("keydown", onKey);
    window.addEventListener("resize", close);
    document.addEventListener("scroll", close, true);
    return () => {
      popEsc(token);
      clearTimeout(id);
      document.removeEventListener("click", close);
      document.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", close);
      document.removeEventListener("scroll", close, true);
      // Hand focus back only if this level still holds it: anything that has taken
      // focus by now -- a dialog the chosen action opened, whatever an outside click
      // hit -- owns it, and a trigger removed while the menu was open is gone.
      if (tookFocus && menuEl?.contains(document.activeElement) && focusReturn?.isConnected) {
        focusReturn.focus();
      }
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
  {#each items as item, i (i)}
    {#if item === null}
      <div class="divider" role="separator" onmouseenter={() => (openSub = null)}></div>
    {:else if item.children}
      <div
        class="item submenu"
        class:disabled={item.disabled}
        role="menuitem"
        data-row={i}
        tabindex={i === tabRow ? 0 : -1}
        aria-haspopup="true"
        aria-expanded={openSub === i}
        aria-disabled={item.disabled ? "true" : undefined}
        onmouseenter={(e) => openSubmenu(i, e.currentTarget as HTMLElement)}
        onkeydown={(e) => onRowKey(e, i)}
      >
        {#if hasCheckable}
          <span class="tick"></span>
        {/if}
        <span class="lbl">{item.label}</span>
        <span class="arrow"><Icon name="submenu" size={9} /></span>
      </div>
    {:else}
      <button
        class="item"
        class:disabled={item.disabled}
        class:danger={item.danger}
        role={"checked" in item ? "menuitemcheckbox" : "menuitem"}
        aria-checked={"checked" in item ? (item.checked ? "true" : "false") : undefined}
        data-row={i}
        tabindex={i === tabRow ? 0 : -1}
        aria-disabled={item.disabled ? "true" : undefined}
        onmouseenter={() => (openSub = null)}
        onkeydown={(e) => onRowKey(e, i)}
        onclick={(e) => {
          e.stopPropagation();
          run(item);
        }}
      >
        {#if hasCheckable}
          <span class="tick">{#if item.checked}<Icon name="check" size={11} />{/if}</span>
        {/if}
        {#if "swatch" in item}
          <span class="swatch" class:none={!item.swatch} style:background={item.swatch || "transparent"}></span>
        {/if}
        <span class="lbl">{item.label}</span>
      </button>
    {/if}
  {/each}
</div>

{#if openSub !== null && items[openSub] && items[openSub]!.children}
  <Self
    x={subPos.x}
    y={subPos.y}
    items={items[openSub]!.children!}
    onClose={onClose}
    focusToken={subFocusToken}
    returnFocus={focusReturn}
    onBack={() => {
      const parentRow = openSub;
      if (parentRow !== null) {
        focusRow(parentRow);
      }
    }}
  />
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
    height: 11px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    color: var(--color-accent);
  }
  .swatch {
    flex: 0 0 10px;
    width: 10px;
    height: 10px;
    border: var(--border-weight) solid var(--color-border);
  }
  .swatch.none {
    background: transparent;
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
    display: inline-flex;
    align-items: center;
    color: var(--color-dim);
  }
  .item:hover {
    background: var(--color-base);
    border: 0;
  }
  .item:focus-visible {
    background: var(--color-base);
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -1px;
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
