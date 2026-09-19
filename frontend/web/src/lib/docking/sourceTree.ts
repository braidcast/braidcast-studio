import type { SceneItem, SceneItemRef } from "$lib/api/bridge";
import { findItem, isChildOf, refKey, sameItem } from "$lib/utils/sceneItemRef";

// One rendered row of a sources tree: a scene's own item at level 1, or a group's child at
// level 2 (groups cannot nest, so there is no deeper level).
export interface TreeRow {
  item: SceneItem;
  key: string;
  level: 1 | 2;
  // The group row a child belongs to; null at top level.
  parent: SceneItem | null;
  // Top-first position within the owner's full list, the index sceneItems.reorder's `to`
  // takes. A filter does not change it.
  index: number;
  // 1-based position among the siblings actually shown, and their count (aria-posinset /
  // aria-setsize).
  posInSet: number;
  setSize: number;
  // Group rows only: whether its children are shown. null for any other row.
  expanded: boolean | null;
}

export function isGroupRow(item: SceneItem): boolean {
  return item.children != null;
}

// The rows a tree shows for `items`, top-first, each child directly under its group. With
// no query a group's children follow its saved collapsed state. With a query only matching
// rows show, whatever the collapsed state, since a search has to find a child inside a
// collapsed group; a group also shows, as the context for its matching children. A group
// matched by name alone shows without its children, and reads as not expanded.
export function visibleRows(items: readonly SceneItem[], query: string): TreeRow[] {
  const q = query.trim().toLowerCase();
  const matches = (i: SceneItem) => (i.source ?? "").toLowerCase().includes(q);
  const shown: { item: SceneItem; index: number; kids: { item: SceneItem; index: number }[] }[] = [];
  items.forEach((item, index) => {
    const all = (item.children ?? []).map((c, i) => ({ item: c, index: i }));
    if (!q) {
      shown.push({ item, index, kids: item.collapsed ? [] : all });
      return;
    }
    const kids = all.filter((k) => matches(k.item));
    if (matches(item) || kids.length > 0) {
      shown.push({ item, index, kids });
    }
  });
  const rows: TreeRow[] = [];
  shown.forEach(({ item, index, kids }, pos) => {
    const expanded = q ? kids.length > 0 : !item.collapsed;
    rows.push({
      item,
      key: refKey(item),
      level: 1,
      parent: null,
      index,
      posInSet: pos + 1,
      setSize: shown.length,
      expanded: isGroupRow(item) ? expanded : null,
    });
    kids.forEach((k, kpos) => {
      rows.push({
        item: k.item,
        key: refKey(k.item),
        level: 2,
        parent: item,
        index: k.index,
        posInSet: kpos + 1,
        setSize: kids.length,
        expanded: null,
      });
    });
  });
  return rows;
}

// Where a ref sits among its siblings in the full list: its top-first index and the size of
// the list it is ordered within (the scene's items, or its group's children). index -1 when
// the ref is not listed.
export function siblingPosition(items: readonly SceneItem[], ref: SceneItemRef): { index: number; count: number } {
  const top = items.findIndex((i) => sameItem(i, ref));
  if (top >= 0) {
    return { index: top, count: items.length };
  }
  for (const row of items) {
    const at = row.children?.findIndex((c) => sameItem(c, ref)) ?? -1;
    if (at >= 0) {
      return { index: at, count: row.children!.length };
    }
  }
  return { index: -1, count: 0 };
}

// The key of the row holding a tree's single tab stop, first of: the `active` row the user
// last moved to, while shown; its group row, when a collapse hid it; the selection's `focus`,
// while shown; the row now at `lastIndex`, the position the stop last held, so removing the
// focused row hands the stop to its neighbour (the new last row when the list got shorter).
// null when there are no rows.
export function tabStopKey(
  rows: readonly TreeRow[],
  active: SceneItemRef | null,
  focus: SceneItemRef | null,
  lastIndex: number,
): string | null {
  const shown = (ref: SceneItemRef) => rows.find((r) => sameItem(r.item, ref));
  const row =
    (active ? (shown(active) ?? rows.find((r) => isChildOf(r.item, active))) : undefined) ??
    (focus ? shown(focus) : undefined);
  if (row) {
    return row.key;
  }
  return rows[Math.min(Math.max(lastIndex, 0), rows.length - 1)]?.key ?? null;
}

// How a dock can bring a listed item's row on screen: it already is, its group has to be
// expanded first, or it cannot be shown at all because a filter hides it.
export type RowReveal = { kind: "shown" } | { kind: "expand"; group: SceneItem } | { kind: "hidden" };

export function revealRow(
  items: readonly SceneItem[],
  rows: readonly TreeRow[],
  ref: SceneItemRef,
  filtering: boolean,
): RowReveal {
  if (rows.some((r) => sameItem(r.item, ref))) {
    return { kind: "shown" };
  }
  const group = items.find((i) => isChildOf(i, ref));
  if (!filtering && group?.collapsed) {
    return { kind: "expand", group };
  }
  return { kind: "hidden" };
}

// Expand the group the preview has been drilled into, so the children it is now picking
// from are listed. `entered` is sceneItem.selected's `enteredGroup`: a ref to the group's
// own top-level row, or null when the preview is in no group. Nothing here ever collapses:
// a collapse is the user's own state, and leaving a group is not a request to undo their
// expand. Shared by every dock that hosts a sources tree so they cannot react differently.
export async function expandEnteredGroup(
  items: readonly SceneItem[],
  entered: SceneItemRef | null | undefined,
  setCollapsed: (group: SceneItem, collapsed: boolean) => Promise<boolean>,
): Promise<void> {
  if (!entered) {
    return;
  }
  const group = findItem(items, entered);
  if (group?.collapsed) {
    await setCollapsed(group, false);
  }
}

export type TreeKeyAction =
  // Focus `row` and select it: alone, or extending from the pivot when `extend`.
  | { kind: "move"; row: TreeRow; extend: boolean }
  // Add or remove `row` from the selection, leaving the rest.
  | { kind: "toggle"; row: TreeRow }
  | { kind: "setCollapsed"; row: TreeRow; collapsed: boolean }
  // A key the tree owns that has nothing to do here (an edge, a leaf's Right).
  | { kind: "none" };

interface KeyLike {
  key: string;
  shiftKey: boolean;
  ctrlKey: boolean;
  metaKey: boolean;
  altKey: boolean;
}

// What a keydown on the tree does, given the rows shown and the row holding the tab stop.
// null means the tree does not own the key, so it must be left to bubble: Ctrl+Arrow and
// Ctrl+Home/End are the app-level reorder shortcuts. While `filtering`, collapse is
// suspended, so Left/Right only move.
export function treeKeyAction(
  rows: readonly TreeRow[],
  activeKey: string | null,
  e: KeyLike,
  filtering: boolean,
): TreeKeyAction | null {
  if (e.altKey) {
    return null;
  }
  const mod = e.ctrlKey || e.metaKey;
  const at = rows.findIndex((r) => r.key === activeKey);
  const row = at >= 0 ? rows[at] : undefined;
  const none: TreeKeyAction = { kind: "none" };
  const moveTo = (target: TreeRow | undefined, extend = e.shiftKey): TreeKeyAction =>
    target ? { kind: "move", row: target, extend } : none;

  if (e.key === " " || e.key === "Spacebar") {
    if (!row) {
      return none;
    }
    return mod ? { kind: "toggle", row } : moveTo(row);
  }
  if (e.key === "Enter") {
    return mod ? null : moveTo(row);
  }
  if (mod) {
    return null;
  }
  switch (e.key) {
    case "ArrowDown":
      return moveTo(row ? rows[at + 1] : rows[0]);
    case "ArrowUp":
      return moveTo(row ? rows[at - 1] : rows.at(-1));
    case "Home":
      return moveTo(rows[0]);
    case "End":
      return moveTo(rows.at(-1));
    case "ArrowRight": {
      if (!row || row.expanded === null || e.shiftKey) {
        return none;
      }
      if (!row.expanded) {
        return filtering ? none : { kind: "setCollapsed", row, collapsed: false };
      }
      const next = rows[at + 1];
      return next?.parent === row.item ? moveTo(next, false) : none;
    }
    case "ArrowLeft": {
      if (!row || e.shiftKey) {
        return none;
      }
      if (row.expanded && !filtering) {
        return { kind: "setCollapsed", row, collapsed: true };
      }
      return row.parent ? moveTo(rows.find((r) => r.item === row.parent), false) : none;
    }
    default:
      return null;
  }
}

// Marks a sources tree's root, so the app-level key handler can tell a key aimed at the
// tree (arrow navigation) from one aimed at the preview (arrow nudge).
export const SOURCE_TREE_ATTR = "data-source-tree";

export function isInSourceTree(target: EventTarget | null): boolean {
  return target instanceof Element && target.closest(`[${SOURCE_TREE_ATTR}]`) !== null;
}
