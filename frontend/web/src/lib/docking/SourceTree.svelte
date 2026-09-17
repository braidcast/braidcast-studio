<script lang="ts">
  import { tick, untrack } from "svelte";
  import type { SceneItem, SceneItemRef } from "$lib/api/bridge";
  import type { SourceSelection } from "$lib/stores/sourceSelectionStore.svelte";
  import IconButton, { ICONBTN_ROW } from "$lib/ui/IconButton.svelte";
  import { selectOnMount } from "$lib/utils/focusActions";
  import { isEditable } from "$lib/utils/editableTarget";
  import { sameItem, sameParent, toRef } from "$lib/utils/sceneItemRef";
  import { SOURCE_TREE_ATTR, isGroupRow, tabStopKey, treeKeyAction, type TreeRow } from "$lib/docking/sourceTree";

  // A scene's sources as a tree: top-level items, with each group's children indented
  // under it. Shared by every dock that lists scene items, so row markup, selection,
  // keyboard navigation and drag-to-reorder behave the same in all of them; the dock keeps
  // the bridge calls and passes them in, addressed through its own canvas and scene.
  //
  // `variant` picks the row look the dock already had: "dock" is the full Sources dock
  // (the .dock-row primitives in app.css), "embed" the compact list inside a CanvasDock
  // (its .es-row rules).
  interface Props {
    rows: TreeRow[];
    selection: SourceSelection;
    variant: "dock" | "embed";
    label: string;
    // A filter is applied: reordering and collapsing are suspended, since both only make
    // sense against the full list.
    filtering: boolean;
    renaming: SceneItemRef | null;
    renameTo: string;
    // Called after the tree changed `selection`, so the dock claims its surface and pushes
    // the new set to its preview.
    onSelect: () => void;
    onRenameCommit: () => void;
    onRenameCancel: () => void;
    onToggleVisible: (item: SceneItem) => void;
    onToggleLocked: (item: SceneItem) => void;
    onOpenProperties: (item: SceneItem) => void;
    onContextMenu: (e: MouseEvent, item: SceneItem) => void;
    // Move `item` to top-first index `to` within its own parent's list.
    onReorder: (item: SceneItem, to: number) => void;
    // Resolves to whether the host took it.
    onSetCollapsed: (item: SceneItem, collapsed: boolean) => Promise<boolean>;
  }
  let {
    rows,
    selection,
    variant,
    label,
    filtering,
    renaming,
    renameTo = $bindable(),
    onSelect,
    onRenameCommit,
    onRenameCancel,
    onToggleVisible,
    onToggleLocked,
    onOpenProperties,
    onContextMenu,
    onReorder,
    onSetCollapsed,
  }: Props = $props();

  const VARIANTS = {
    dock: { row: "dock-row", selected: "sel", hidden: "dimmed", label: "dock-label", toggle: 18, lockLast: false },
    embed: { row: "es-row src", selected: "on", hidden: "hidden-src", label: "es-label", toggle: 17, lockLast: true },
  } as const;
  const v = $derived(VARIANTS[variant]);

  const rowItems = $derived(rows.map((r) => r.item));
  // Top-level rows reserve the disclosure column only once the list holds a group, so a
  // scene without groups keeps its labels where they were.
  const hasGroups = $derived(rows.some((r) => isGroupRow(r.item)));

  let rootEl = $state<HTMLUListElement | undefined>();
  const uid = $props.id();

  // The row holding the single tab stop (tabStopKey has the fallbacks). `lastIndex` is where
  // the stop sat when the rows last changed; it is plain, so remembering it does not
  // recompute the stop.
  let activeRef = $state<SceneItemRef | null>(null);
  let lastIndex = 0;
  const tabKey = $derived(tabStopKey(rows, activeRef, selection.item, lastIndex));
  $effect(() => {
    const at = rows.findIndex((r) => r.key === tabKey);
    if (at >= 0) {
      lastIndex = at;
    }
  });

  function hasFocus(): boolean {
    return rootEl?.contains(document.activeElement) ?? false;
  }

  // A selection made elsewhere (the preview) moves the tab stop to its focus, but never
  // while the user is navigating the tree, where the stop follows the keyboard instead.
  $effect(() => {
    const focus = selection.item;
    if (focus && !hasFocus()) {
      activeRef = toRef(focus);
    }
  });

  function focusRow(key: string | null): void {
    if (key === null) {
      return;
    }
    rootEl?.querySelector<HTMLElement>(`[data-key="${CSS.escape(key)}"]`)?.focus();
  }

  // A keyed reload moves row nodes, and moving or removing the focused node drops focus to
  // the body (a reorder, a collapse hiding the focused child, a removal). Put it back on the
  // tab stop only when that is what happened: the tree held focus before the rows changed and
  // nothing holds it now. Both effects depend on `rows` alone, so a later change to the tab
  // stop cannot pull focus back from wherever the user has since moved it.
  let focusBeforeRows = false;
  $effect.pre(() => {
    void rows;
    focusBeforeRows = untrack(hasFocus);
  });
  $effect(() => {
    void rows;
    const lost = focusBeforeRows && (document.activeElement === null || document.activeElement === document.body);
    focusBeforeRows = false;
    if (lost) {
      untrack(() => focusRow(tabKey));
    }
  });

  function selectRow(row: TreeRow, mods: Pick<MouseEvent, "shiftKey" | "ctrlKey" | "metaKey">): void {
    activeRef = toRef(row.item);
    if (mods.shiftKey) {
      selection.range(row.item, rowItems);
    } else if (mods.ctrlKey || mods.metaKey) {
      selection.toggle(row.item);
    } else {
      selection.selectOne(row.item);
    }
    onSelect();
  }

  // The row's own controls (disclosure, visibility, lock, rename field) handle their
  // clicks; only the rest of the row selects.
  function onControl(e: Event): boolean {
    return e.target instanceof Element && e.target.closest("button, input") !== null;
  }

  function onRowClick(e: MouseEvent, row: TreeRow): void {
    if (!onControl(e)) {
      selectRow(row, e);
    }
  }

  function onRowDblClick(e: MouseEvent, row: TreeRow): void {
    if (!onControl(e)) {
      onOpenProperties(row.item);
    }
  }

  function onRowKeydown(e: KeyboardEvent): void {
    if (isEditable(e.target)) {
      return;
    }
    // Space and Enter on a visibility or lock button press that button. They are out of the
    // tab order, but a click still focuses them.
    if ((e.key === " " || e.key === "Enter") && e.target instanceof Element && e.target.closest("button")) {
      return;
    }
    const action = treeKeyAction(rows, tabKey, e, filtering);
    if (!action) {
      return;
    }
    e.preventDefault();
    switch (action.kind) {
      case "move":
        selectRow(action.row, { shiftKey: action.extend, ctrlKey: false, metaKey: false });
        focusRow(action.row.key);
        break;
      case "toggle":
        selection.toggle(action.row.item);
        onSelect();
        break;
      case "setCollapsed":
        void setCollapsed(action.row, action.collapsed);
        break;
      case "none":
        break;
      default:
        action satisfies never;
    }
  }

  // Collapsing moves the tab stop to the group row but never selects it; selected children
  // just leave the selection, and come back if the host refuses the collapse.
  async function setCollapsed(row: TreeRow, collapsed: boolean): Promise<void> {
    let restore: ReturnType<SourceSelection["collapseGroup"]> = null;
    if (collapsed) {
      activeRef = toRef(row.item);
      restore = selection.collapseGroup(row.item);
      if (restore) {
        onSelect();
      }
    }
    if (!(await onSetCollapsed(row.item, collapsed)) && restore?.(rowItems)) {
      onSelect();
    }
  }

  function toggleCollapsed(row: TreeRow): void {
    if (!filtering && row.expanded !== null) {
      void setCollapsed(row, row.expanded);
    }
  }

  async function onRenameKey(e: KeyboardEvent, row: TreeRow): Promise<void> {
    if (e.key !== "Enter" && e.key !== "Escape") {
      return;
    }
    if (e.key === "Enter") {
      onRenameCommit();
    } else {
      onRenameCancel();
    }
    await tick();
    focusRow(row.key);
  }

  // Drag-to-reorder among one parent's rows. Dropping onto a row of another parent (into or
  // out of a group) is refused, rather than taken as a drop that silently does nothing: the
  // drag-over sets dropEffect "none", which shows the no-drop pointer and keeps the drop from
  // firing, and draws no indicator. Withholding preventDefault would not refuse it on its
  // own, since App's window-level dragover handler always calls it.
  let dragRow = $state<TreeRow | null>(null);
  let dropKey = $state<string | null>(null);

  function onDragStart(e: DragEvent, row: TreeRow): void {
    if (filtering) {
      return;
    }
    dragRow = row;
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", row.key); // Firefox requires data
    }
  }

  function onDragOver(e: DragEvent, row: TreeRow): void {
    if (!dragRow) {
      return;
    }
    if (!sameParent(dragRow.item, row.item)) {
      dropKey = null;
      if (e.dataTransfer) {
        e.dataTransfer.dropEffect = "none";
      }
      return;
    }
    e.preventDefault(); // mark this a valid drop target
    if (e.dataTransfer) {
      e.dataTransfer.dropEffect = "move";
    }
    dropKey = row.key;
  }

  function onDrop(e: DragEvent, row: TreeRow): void {
    e.preventDefault();
    const dragged = dragRow;
    onDragEnd();
    if (dragged && sameParent(dragged.item, row.item) && dragged.key !== row.key) {
      onReorder(dragged.item, row.index);
    }
  }

  function onDragEnd(): void {
    dragRow = null;
    dropKey = null;
  }

  // The pointer left the tree, not just one of its rows for another.
  function onTreeDragLeave(e: DragEvent): void {
    if (!(e.relatedTarget instanceof Node && rootEl?.contains(e.relatedTarget))) {
      dropKey = null;
    }
  }

  // A row is named by its label alone, not by its buttons' labels. The states those buttons
  // show are read as its description instead, from hidden text in the row.
  const nameOf = (item: SceneItem) => item.source ?? "(unnamed)";
  const statesOf = (item: SceneItem) => [!item.visible && "hidden", item.locked && "locked"].filter(Boolean).join(", ");
</script>

{#snippet lockButton(item: SceneItem)}
  <IconButton
    icon={item.locked ? "lock" : "lock-open"}
    size={v.toggle}
    iconSize={12}
    title={item.locked ? "Unlock" : "Lock"}
    aria-label={item.locked ? "Unlock" : "Lock"}
    tabindex={-1}
    onclick={() => onToggleLocked(item)}
  />
{/snippet}

<ul
  class="dock-list tree"
  role="tree"
  aria-label={label}
  aria-multiselectable="true"
  bind:this={rootEl}
  ondragleave={onTreeDragLeave}
  {...{ [SOURCE_TREE_ATTR]: "" }}
>
  {#each rows as row (row.key)}
    {@const item = row.item}
    {@const states = statesOf(item)}
    {@const statesId = `${uid}-states-${row.key}`}
    <li
      class={[v.row, "tree-row", selection.has(item) && v.selected, !item.visible && v.hidden]}
      class:dropTarget={dropKey === row.key && dragRow !== null && dragRow.key !== row.key}
      role="treeitem"
      aria-label={nameOf(item)}
      aria-describedby={states ? statesId : undefined}
      aria-level={row.level}
      aria-posinset={row.posInSet}
      aria-setsize={row.setSize}
      aria-selected={selection.has(item)}
      aria-expanded={row.expanded ?? undefined}
      tabindex={row.key === tabKey ? 0 : -1}
      data-key={row.key}
      style:box-shadow={item.color ? `inset 3px 0 0 ${item.color}` : null}
      draggable={!filtering}
      ondragstart={(e) => onDragStart(e, row)}
      ondragover={(e) => onDragOver(e, row)}
      ondrop={(e) => onDrop(e, row)}
      ondragend={onDragEnd}
      onclick={(e) => onRowClick(e, row)}
      ondblclick={(e) => onRowDblClick(e, row)}
      oncontextmenu={(e) => onContextMenu(e, item)}
      onkeydown={onRowKeydown}
      onfocusin={() => (activeRef = toRef(item))}
    >
      <IconButton
        icon={item.visible ? "eye" : "eye-off"}
        size={v.toggle}
        iconSize={14}
        title={item.visible ? "Hide" : "Show"}
        aria-label={item.visible ? "Hide" : "Show"}
        tabindex={-1}
        onclick={() => onToggleVisible(item)}
      />
      {#if !v.lockLast}
        {@render lockButton(item)}
      {/if}
      {#if row.level === 2}
        <span class="indent" style:width="{ICONBTN_ROW.size}px" aria-hidden="true"></span>
      {/if}
      {#if row.expanded !== null}
        <span class="chev" class:open={row.expanded}>
          <IconButton
            icon="caret-right"
            {...ICONBTN_ROW}
            title={row.expanded ? "Collapse group" : "Expand group"}
            aria-label={row.expanded ? "Collapse group" : "Expand group"}
            aria-disabled={filtering ? "true" : undefined}
            tabindex={-1}
            onclick={() => toggleCollapsed(row)}
          />
        </span>
      {:else if hasGroups}
        <span class="chev-space" style:width="{ICONBTN_ROW.size}px" aria-hidden="true"></span>
      {/if}
      {#if renaming && sameItem(renaming, item)}
        <input
          class="inline"
          class:compact={variant === "embed"}
          aria-label="Rename {nameOf(item)}"
          bind:value={renameTo}
          onkeydown={(e) => void onRenameKey(e, row)}
          onblur={onRenameCommit}
          use:selectOnMount
        />
      {:else}
        <span class={v.label}>{nameOf(item)}</span>
      {/if}
      {#if v.lockLast}
        {@render lockButton(item)}
      {/if}
      {#if states}
        <span id={statesId} class="sr-only">{states}</span>
      {/if}
    </li>
  {/each}
</ul>

<style>
  .tree {
    overflow: auto;
  }
  /* The whole row selects on a click, so it shows one cursor, labels included. */
  .tree .tree-row {
    position: relative;
    cursor: pointer;
  }
  .tree-row .dock-label,
  .tree-row .es-label {
    cursor: inherit;
  }
  /* --tree-row-pad-y is shared with the indentation guide below. The dock row is one row
     shorter than the shared .dock-row default (5px 7px). */
  .tree-row.dock-row {
    --tree-row-pad-y: 4px;
    padding: var(--tree-row-pad-y) 7px;
  }
  .tree-row.es-row.src {
    --tree-row-pad-y: 5px;
    padding: var(--tree-row-pad-y) 10px;
  }
  .tree-row.hidden-src .es-label {
    color: var(--color-muted);
    text-decoration: line-through;
  }
  .tree-row .dock-label,
  .tree-row .es-label {
    min-width: 0;
  }

  /* The row is the focusable treeitem, so the ring sits on it. Same indicator as the
     .dock-row label ring in app.css, for the reasons given there: --color-text holds
     contrast on every preset, plain and selected, and tells focus apart from selection,
     which the accent already means. :where() keeps it below the drop indicator. */
  .tree-row:where(:focus-visible) {
    outline: 2px solid var(--color-text);
    outline-offset: -2px;
  }

  /* Drag-reorder drop indicator. Outline avoids layout shift and the inline box-shadow
     the color tag already uses. */
  .tree-row.dropTarget {
    outline: var(--border-weight) solid var(--color-accent);
    outline-offset: -1px;
  }

  .chev,
  .chev-space,
  .indent {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
  }
  /* The indentation guide: a hairline under the group's disclosure column. Its negative
     margins reach through the row's vertical padding and over its bottom border, so
     consecutive children draw one unbroken line. Decorative only; aria-level carries the
     nesting. */
  .indent {
    align-self: stretch;
    margin-block: calc(-1 * var(--tree-row-pad-y)) calc(-1 * (var(--tree-row-pad-y) + var(--border-weight)));
    background: linear-gradient(
        color-mix(in srgb, var(--color-dim) 40%, transparent),
        color-mix(in srgb, var(--color-dim) 40%, transparent)
      )
      center / 1px 100% no-repeat;
  }
  .chev :global(svg) {
    transition: transform 0.15s ease;
  }
  .chev.open :global(svg) {
    transform: rotate(90deg);
  }

  .inline {
    flex: 1;
    min-width: 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 3px 5px;
  }
  .inline.compact {
    padding: 2px 5px;
  }
  .inline:focus {
    outline: none;
  }

  @media (prefers-reduced-motion: reduce) {
    .chev :global(svg) {
      transition: none;
    }
  }
</style>
