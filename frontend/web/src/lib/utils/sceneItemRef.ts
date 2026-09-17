import type { SceneItem, SceneItemRef, TransformTarget } from "$lib/api/bridge";

// Whether two refs name the same scene item. An id is unique only within its owner, so
// the owning group is part of the identity; null means top level. An absent group reads as
// null too: a payload from the host may omit it for a top-level item.
export function sameItem(a: SceneItemRef, b: SceneItemRef): boolean {
  return a.id === b.id && (a.group ?? null) === (b.group ?? null);
}

// Whether two refs are ordered within the same list: the same group's children, or both at
// top level.
export function sameParent(a: SceneItemRef, b: SceneItemRef): boolean {
  return (a.group ?? null) === (b.group ?? null);
}

// A ref reduced to exactly its two identity fields, an absent group folded to null, so a
// SceneItem's other fields never ride along on a ref sent over the bridge.
export function toRef(item: SceneItemRef): SceneItemRef {
  return { id: item.id, group: item.group ?? null };
}

// The row a ref names in a dock's list, looking inside group rows for a child.
export function findItem(list: readonly SceneItem[], ref: SceneItemRef): SceneItem | undefined {
  for (const row of list) {
    if (sameItem(row, ref)) {
      return row;
    }
    const child = row.children?.find((c) => sameItem(c, ref));
    if (child) {
      return child;
    }
  }
  return undefined;
}

// Whether `ref` names one of `group`'s children.
export function isChildOf(group: SceneItem, ref: SceneItemRef): boolean {
  return group.children?.some((c) => sameItem(c, ref)) ?? false;
}

// `items` without any of `group`'s children, or null when none of them is there.
export function withoutChildrenOf(items: readonly SceneItem[], group: SceneItem): SceneItem[] | null {
  const rest = items.filter((i) => !isChildOf(group, i));
  return rest.length === items.length ? null : rest;
}

// A string identity for a ref, for keyed each blocks and Map keys. The separator cannot
// occur in a uuid.
export function refKey(ref: SceneItemRef): string {
  return `${ref.group ?? ""}:${ref.id}`;
}

// Refs as the host normalizes a preview.select list: deduped by (group, id), a repeat
// keeping its LAST position, since the last member is the focus the host echoes back.
export function normalizeRefs(refs: readonly SceneItemRef[]): SceneItemRef[] {
  const out: SceneItemRef[] = [];
  for (const r of refs) {
    const at = out.findIndex((o) => sameItem(o, r));
    if (at >= 0) {
      out.splice(at, 1);
    }
    out.push(toRef(r));
  }
  return out;
}

export function sameRefList(a: readonly SceneItemRef[], b: readonly SceneItemRef[]): boolean {
  return a.length === b.length && a.every((r, i) => sameItem(r, b[i]));
}

// A scene item's bridge address with the unset locator fields left out rather than sent
// as null. The group always rides along: rebuilding params from the id alone would
// address the top-level item that shares a child's id.
export function targetParams(t: TransformTarget): TransformTarget {
  const p: TransformTarget = toRef(t);
  if (t.canvas != null) {
    p.canvas = t.canvas;
  }
  if (t.scene != null) {
    p.scene = t.scene;
  }
  return p;
}

// One item's address within a dock's scope (its canvas, when not the Default, and scene).
export function itemTarget(scope: { canvas?: string; scene?: string | null }, item: SceneItemRef): TransformTarget {
  return { ...scope, ...toRef(item) };
}
