import { describe, expect, test } from "bun:test";
import type { SceneItem, SceneItemRef } from "$lib/api/bridge";
import { RenameWait, type RenameDock } from "$lib/docking/renameWait";
import {
  revealRow,
  siblingPosition,
  tabStopKey,
  treeKeyAction,
  visibleRows,
  type TreeRow,
} from "$lib/docking/sourceTree";
import { normalizeRefs, sameParent, withoutChildrenOf } from "$lib/utils/sceneItemRef";

const G = "group-uuid";

function item(id: number, source: string, extra: Partial<SceneItem> = {}): SceneItem {
  return {
    id,
    group: null,
    source,
    typeId: "",
    visible: true,
    locked: false,
    scaleFilter: "disable",
    blendMode: "normal",
    blendMethod: "default",
    color: "",
    showTransition: null,
    hideTransition: null,
    ...extra,
  };
}

// A top-level item 1, a group (id 2) holding children 1 and 3, and a top-level item 3: both
// child ids collide with top-level ids.
function scene(collapsed = false): SceneItem[] {
  const kids = [item(1, "Cam", { group: G }), item(3, "Mic", { group: G })];
  return [item(1, "Browser"), item(2, "Group", { typeId: "group", children: kids, collapsed }), item(3, "Text")];
}

const keys = (rows: TreeRow[]) => rows.map((r) => r.key);
const key = (k: string, mods: Partial<{ shiftKey: boolean; ctrlKey: boolean }> = {}) => ({
  key: k,
  shiftKey: false,
  ctrlKey: false,
  metaKey: false,
  altKey: false,
  ...mods,
});

describe("visibleRows", () => {
  test("children follow their group, keyed apart from colliding top-level ids", () => {
    const rows = visibleRows(scene(), "");
    expect(keys(rows)).toEqual([":1", ":2", `${G}:1`, `${G}:3`, ":3"]);
    expect(rows.map((r) => r.level)).toEqual([1, 1, 2, 2, 1]);
    expect(rows[1].expanded).toBe(true);
    expect(rows[3]).toMatchObject({ index: 1, posInSet: 2, setSize: 2 });
  });

  test("a collapsed group hides its children", () => {
    const rows = visibleRows(scene(true), "");
    expect(keys(rows)).toEqual([":1", ":2", ":3"]);
    expect(rows[1].expanded).toBe(false);
  });

  test("a filter reveals a matching child inside a collapsed group", () => {
    const rows = visibleRows(scene(true), "mic");
    expect(keys(rows)).toEqual([":2", `${G}:3`]);
    expect(rows[1]).toMatchObject({ index: 1, posInSet: 1, setSize: 1 });
    expect(rows[0]).toMatchObject({ index: 1, expanded: true });
  });

  test("a filter matching only a group's name shows the group without its children", () => {
    const rows = visibleRows(scene(), "group");
    expect(keys(rows)).toEqual([":2"]);
    expect(rows[0].expanded).toBe(false);
  });
});

describe("tabStopKey", () => {
  const rows = visibleRows(scene(), "");
  const cam = { id: 1, group: G };

  test("stays on the active row while it is shown", () => {
    expect(tabStopKey(rows, cam, { id: 3, group: null }, 0)).toBe(`${G}:1`);
  });

  test("falls to the group row when a collapse hides the active child", () => {
    expect(tabStopKey(visibleRows(scene(true), ""), cam, { id: 3, group: null }, 4)).toBe(":2");
  });

  test("falls to the selection's focus when the active row is gone", () => {
    const rest = rows.filter((r) => r.key !== ":1");
    expect(tabStopKey(rest, { id: 1, group: null }, { id: 3, group: null }, 0)).toBe(":3");
  });

  test("hands a removed row's stop to its neighbour, not the first row", () => {
    const withoutMic = scene();
    withoutMic[1].children = withoutMic[1].children!.slice(0, 1);
    expect(tabStopKey(visibleRows(withoutMic, ""), { id: 3, group: G }, null, 3)).toBe(":3");
    expect(tabStopKey(rows.slice(0, 2), { id: 3, group: null }, null, 4)).toBe(":2");
    expect(tabStopKey([], { id: 3, group: null }, null, 4)).toBeNull();
  });
});

describe("siblingPosition", () => {
  test("positions a child within its group, not the scene", () => {
    expect(siblingPosition(scene(), { id: 3, group: G })).toEqual({ index: 1, count: 2 });
    expect(siblingPosition(scene(), { id: 3, group: null })).toEqual({ index: 2, count: 3 });
    expect(siblingPosition(scene(), { id: 9, group: null })).toEqual({ index: -1, count: 0 });
  });
});

describe("revealRow", () => {
  const child = { id: 3, group: G };

  test("a shown row needs nothing, a child of a collapsed group needs its group expanded", () => {
    expect(revealRow(scene(), visibleRows(scene(), ""), child, false)).toEqual({ kind: "shown" });
    const collapsed = scene(true);
    expect(revealRow(collapsed, visibleRows(collapsed, ""), child, false)).toMatchObject({
      kind: "expand",
      group: { id: 2, group: null },
    });
  });

  test("a row a filter hides cannot be revealed", () => {
    const collapsed = scene(true);
    expect(revealRow(collapsed, visibleRows(collapsed, "cam"), child, true)).toEqual({ kind: "hidden" });
    expect(revealRow(scene(), visibleRows(scene(), "cam"), child, true)).toEqual({ kind: "hidden" });
  });
});

describe("treeKeyAction", () => {
  const rows = visibleRows(scene(), "");
  const act = (active: string | null, k: ReturnType<typeof key>, filtering = false) =>
    treeKeyAction(rows, active, k, filtering);

  test("Up/Down move, Shift extends, and edges do nothing", () => {
    expect(act(":2", key("ArrowDown"))).toMatchObject({ kind: "move", row: { key: `${G}:1` }, extend: false });
    expect(act(":2", key("ArrowUp", { shiftKey: true }))).toMatchObject({
      kind: "move",
      row: { key: ":1" },
      extend: true,
    });
    expect(act(":3", key("ArrowDown"))).toEqual({ kind: "none" });
  });

  test("Right expands a collapsed group, then enters its first child", () => {
    const collapsed = visibleRows(scene(true), "");
    expect(treeKeyAction(collapsed, ":2", key("ArrowRight"), false)).toMatchObject({
      kind: "setCollapsed",
      collapsed: false,
    });
    expect(act(":2", key("ArrowRight"))).toMatchObject({ kind: "move", row: { key: `${G}:1` } });
    expect(act(":1", key("ArrowRight"))).toEqual({ kind: "none" });
  });

  test("Left collapses an expanded group, and moves from a child to its group", () => {
    expect(act(":2", key("ArrowLeft"))).toMatchObject({ kind: "setCollapsed", collapsed: true });
    expect(act(`${G}:3`, key("ArrowLeft"))).toMatchObject({ kind: "move", row: { key: ":2" }, extend: false });
    expect(act(":2", key("ArrowLeft"), true)).toEqual({ kind: "none" });
  });

  test("plain Space selects the row alone, Shift+Space extends to it", () => {
    expect(act(`${G}:1`, key(" "))).toMatchObject({ kind: "move", row: { key: `${G}:1` }, extend: false });
    expect(act(`${G}:1`, key(" ", { shiftKey: true }))).toMatchObject({ kind: "move", extend: true });
  });

  test("Shift+Home and Shift+End extend to the first and last rows", () => {
    expect(act(`${G}:1`, key("Home", { shiftKey: true }))).toMatchObject({
      kind: "move",
      row: { key: ":1" },
      extend: true,
    });
    expect(act(`${G}:1`, key("End", { shiftKey: true }))).toMatchObject({
      kind: "move",
      row: { key: ":3" },
      extend: true,
    });
  });

  test("Home/End, Ctrl+Space toggles, and Ctrl+Arrow is left for reorder", () => {
    expect(act(`${G}:1`, key("Home"))).toMatchObject({ kind: "move", row: { key: ":1" } });
    expect(act(`${G}:1`, key("End"))).toMatchObject({ kind: "move", row: { key: ":3" } });
    expect(act(`${G}:1`, key(" ", { ctrlKey: true }))).toMatchObject({ kind: "toggle", row: { key: `${G}:1` } });
    expect(act(`${G}:1`, key("ArrowUp", { ctrlKey: true }))).toBeNull();
    expect(act(`${G}:1`, key("Home", { ctrlKey: true }))).toBeNull();
  });
});

describe("withoutChildrenOf", () => {
  const [top, group] = scene();
  const [cam, mic] = group.children!;

  test("drops the group's children and never adds the group", () => {
    expect(withoutChildrenOf([top, cam, mic], group)).toEqual([top]);
    expect(withoutChildrenOf([cam], group)).toEqual([]);
  });

  test("leaves a colliding top-level id and reports no change", () => {
    const topThree = scene()[2];
    expect(withoutChildrenOf([top, topThree], group)).toBeNull();
  });
});

describe("sameParent", () => {
  test("compares owners, folding an absent group to top level", () => {
    expect(sameParent({ id: 1, group: G }, { id: 3, group: G })).toBe(true);
    expect(sameParent({ id: 1, group: null }, { id: 1, group: G })).toBe(false);
    expect(sameParent({ id: 1 } as SceneItemRef, { id: 2, group: null })).toBe(true);
  });
});

describe("RenameWait", () => {
  // A dock whose group expands resolve only when the test says so.
  function setup(opts: { collapsed?: boolean; filter?: string } = {}) {
    let items = scene(opts.collapsed ?? true);
    let held: SceneItemRef | null = null;
    const begun: SceneItem[] = [];
    const expands: ((took: boolean) => void)[] = [];
    const dock: RenameDock = {
      items: () => items,
      rows: () => visibleRows(items, opts.filter ?? ""),
      filtering: () => (opts.filter ?? "") !== "",
      begin: (item) => begun.push(item),
      expand: () => new Promise((resolve) => expands.push(resolve)),
    };
    const wait = new RenameWait({ get: () => held, set: (ref) => (held = ref) }, dock);
    return {
      wait,
      begun,
      expands,
      held: () => held,
      expandGroup: () => (items = scene(false)),
    };
  }
  const settle = () => new Promise((resolve) => setTimeout(resolve, 0));
  const mic = { id: 3, group: G };

  test("a shown row opens at once and consumes the request", () => {
    const t = setup({ collapsed: false });
    let consumed = false;
    t.wait.serve(mic, () => (consumed = true));
    expect(consumed).toBe(true);
    expect(t.begun.map((i) => i.source)).toEqual(["Mic"]);
    expect(t.wait.waiting).toBe(false);
  });

  test("a row a filter hides is left unserved and unconsumed", () => {
    const t = setup({ filter: "cam" });
    let consumed = false;
    t.wait.serve(mic, () => (consumed = true));
    expect(consumed).toBe(false);
    expect(t.expands).toHaveLength(0);
    expect(t.wait.waiting).toBe(false);
  });

  test("a child of a collapsed group waits for the expand, then opens when its row shows", async () => {
    const t = setup();
    t.wait.serve(mic, () => true);
    expect(t.held()).toEqual(mic);
    t.wait.openWhenShown();
    expect(t.begun).toHaveLength(0);
    t.expands[0](true);
    await settle();
    expect(t.wait.waiting).toBe(true);
    t.expandGroup();
    t.wait.openWhenShown();
    expect(t.begun.map((i) => i.source)).toEqual(["Mic"]);
    expect(t.wait.waiting).toBe(false);
  });

  test("a refused expand drops the wait", async () => {
    const t = setup();
    t.wait.serve(mic, () => true);
    t.expands[0](false);
    await settle();
    expect(t.held()).toBeNull();
  });

  test("a refused expand leaves a newer request's wait alone", async () => {
    const t = setup();
    t.wait.serve(mic, () => true);
    const cam = { id: 1, group: G };
    t.wait.serve(cam, () => true);
    t.expands[0](false);
    await settle();
    expect(t.held()).toEqual(cam);
  });
});

describe("normalizeRefs", () => {
  test("dedupes by (group, id) keeping a repeat's last position", () => {
    const refs = normalizeRefs([
      { id: 1, group: null },
      { id: 1, group: G },
      { id: 1, group: null },
    ]);
    expect(refs).toEqual([
      { id: 1, group: G },
      { id: 1, group: null },
    ]);
  });
});
