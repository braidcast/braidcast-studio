import { Orientation } from "dockview-core";
import type { DockviewApi, SerializedGridObject } from "dockview-core";
import { dockById } from "$lib/docking/dockRegistry";
import { clamp } from "$lib/utils/clamp";
import { log } from "$lib/utils/log";
import { Cat } from "$lib/utils/logCategories";

// Width policy for a dock column: a share of the host, floored and capped. This
// is the single width knob -- programmatic adds (restore chip, redock, the
// canvas/browser reconcilers) and drop-created columns all read it.
//
// Dockview has no say in this by design: every path that creates a group without
// an explicit size (`orthogonalize`, which is what a window-edge drop runs, and
// `addGroup({direction})`, which drops `initialWidth` on the floor) hands the
// splitview `Sizing.Distribute`, and distribute gives the new view an equal
// share of its parent. On a 4K host that is half the window for a chat column.
const COLUMN_FRACTION = 0.2;
const COLUMN_MIN_WIDTH = 300;
const COLUMN_MAX_WIDTH = 420;

// Below this share of the host the main region stops being usable, so a set of
// remembered side-column widths that would breach it is abandoned rather than
// applied.
const MAIN_MIN_FRACTION = 0.4;

// The dock whose column is the flexible main region: every size delta lands
// there so the side columns keep the width the user gave them.
const MAIN_DOCK_ID = "preview";

// The serialized grid's leaf payload. Dockview does not export
// `GroupPanelViewState`, and only the group id is needed here.
interface GroupLeafState {
  id: string;
}
type GridNode = SerializedGridObject<GroupLeafState>;

interface DockColumn {
  // Every group under this root child, so the column carrying the main dock can
  // be recognised whatever its internal shape.
  groupIds: string[];
  // The group whose `setSize({width})` resizes THIS column. A width change is
  // bubbled only as far as the nearest enclosing horizontal splitview, so the
  // group has to be a direct leaf child of the root's child; a column built
  // purely from nested branches has none and is left alone.
  sizingGroupId: string | undefined;
}

interface ColumnTarget {
  id: string;
  width: number;
}

export function dockColumnWidth(hostWidth: number): number {
  return Math.round(clamp(hostWidth * COLUMN_FRACTION, COLUMN_MIN_WIDTH, COLUMN_MAX_WIDTH));
}

// Run `apply` against a measured host. Dockview reports 0x0 until its container
// has been laid out, and a size asserted against that is silently dropped.
export function whenMeasured(dv: DockviewApi, apply: () => void): void {
  const run = (): void => {
    if (dv.width > 0 && dv.height > 0) {
      apply();
    }
  };
  run();
  if (dv.width <= 0 || dv.height <= 0) {
    requestAnimationFrame(run);
  }
}

function collectLeafIds(node: GridNode, out: string[]): void {
  if (node.type === "leaf") {
    out.push((node.data as GroupLeafState).id);
    return;
  }
  for (const child of node.data as GridNode[]) {
    collectLeafIds(child, out);
  }
}

function toColumn(node: GridNode): DockColumn {
  const groupIds: string[] = [];
  collectLeafIds(node, groupIds);
  const directLeaf =
    node.type === "leaf" ? node : (node.data as GridNode[]).find((child) => child.type === "leaf");
  return {
    groupIds,
    sizingGroupId: directLeaf ? (directLeaf.data as GroupLeafState).id : undefined,
  };
}

// The root splitview's children, when the root runs horizontally. A vertically
// split root has rows rather than columns and yields none.
function readColumns(dv: DockviewApi): DockColumn[] {
  const { grid } = dv.toJSON();
  const root = grid.root as GridNode;
  if (grid.orientation !== Orientation.HORIZONTAL || root.type !== "branch") {
    return [];
  }
  return (root.data as GridNode[]).map(toColumn);
}

// Every column except the one holding the main dock, which is the region that
// absorbs the slack. With the main dock gone (closed or torn out) the column
// carrying the most docks takes that role.
function sideColumns(dv: DockviewApi): DockColumn[] {
  const columns = readColumns(dv);
  if (columns.length < 2) {
    return [];
  }
  const mainGroupId = dv.getPanel(MAIN_DOCK_ID)?.group.id;
  const main =
    (mainGroupId === undefined
      ? undefined
      : columns.find((column) => column.groupIds.includes(mainGroupId))) ??
    columns.reduce((widest, column) =>
      column.groupIds.length > widest.groupIds.length ? column : widest,
    );
  return columns.filter((column) => column !== main && column.sizingGroupId !== undefined);
}

// The content floor a group must respect: the widest minimum among its docks.
// Panels carry the same number as their own `minimumWidth` (registry ->
// panelOptions -> serialized with the layout), which dockview reads off the
// active panel; recomputing it here is what a temporary pin is released back to.
function groupFloor(dv: DockviewApi, groupId: string): number {
  const group = dv.getGroup(groupId);
  if (!group) {
    return 0;
  }
  let floor = 0;
  for (const panel of group.panels) {
    const min = dockById(panel.id)?.minWidth ?? 0;
    if (min > floor) {
      floor = min;
    }
  }
  return floor;
}

// Own the width of every dock column for the lifetime of `dv`.
//
// Dockview treats a move as a resize: adding a group (`Sizing.Distribute`) and
// removing one (likewise) both re-split the affected splitview evenly, so a drop
// anywhere silently equalizes columns the user had sized by hand. There is no
// hook to size a drop-created group at birth -- `onDidDrop` fires only for drag
// data the component could not handle, i.e. never for an internal move -- so the
// widths are re-asserted once the layout settles instead.
//
// Layout changes that leave the column set alone (a sash drag, a host resize)
// are the user's intent and are adopted as the new truth; only a change to the
// set of columns re-asserts, giving a brand-new column the policy width.
export function installDockSizing(dv: DockviewApi): { dispose(): void } {
  const intended = new Map<string, number>();
  let adopted = false;
  let asserting = false;

  const adopt = (columns: DockColumn[]): void => {
    intended.clear();
    for (const column of columns) {
      const id = column.sizingGroupId;
      const group = id === undefined ? undefined : dv.getGroup(id);
      if (id !== undefined && group) {
        intended.set(id, group.api.width);
      }
    }
  };

  const assert = (targets: ColumnTarget[]): void => {
    asserting = true;
    try {
      for (const target of targets) {
        const group = dv.getGroup(target.id);
        if (!group) {
          continue;
        }
        // Pin before resizing: dockview spreads each resize's remainder across
        // every other view, so a column already restored this pass would be
        // pushed back off its width by the next one.
        group.api.setConstraints({ minimumWidth: target.width, maximumWidth: target.width });
        group.api.setSize({ width: target.width });
      }
    } finally {
      for (const target of targets) {
        dv.getGroup(target.id)?.api.setConstraints({
          minimumWidth: groupFloor(dv, target.id),
          maximumWidth: Number.MAX_SAFE_INTEGER,
        });
      }
      asserting = false;
    }
  };

  const settle = (): void => {
    if (asserting || dv.width <= 0) {
      return;
    }
    const columns = sideColumns(dv);
    // The first settled layout after a build or a restore is the truth, not
    // something to correct.
    if (!adopted) {
      adopt(columns);
      adopted = true;
      return;
    }
    const structural =
      columns.length !== intended.size ||
      columns.some((column) => !intended.has(column.sizingGroupId as string));
    if (!structural) {
      adopt(columns);
      return;
    }
    const targets = columns.map<ColumnTarget>((column) => {
      const id = column.sizingGroupId as string;
      return {
        id,
        width: Math.max(intended.get(id) ?? dockColumnWidth(dv.width), groupFloor(dv, id)),
      };
    });
    const total = targets.reduce((sum, target) => sum + target.width, 0);
    if (total > dv.width * (1 - MAIN_MIN_FRACTION)) {
      log.dbg(Cat.lifecycle, "dock sizing: columns would crowd the main region, leaving the split", {
        total,
        host: dv.width,
      });
      adopt(columns);
      return;
    }
    assert(targets);
    intended.clear();
    for (const target of targets) {
      intended.set(target.id, target.width);
    }
  };

  return dv.onDidLayoutChange(() => {
    // Dockview fires its listeners in a bare loop, so a throw here would take
    // out the layout-change subscribers registered after this one.
    try {
      settle();
    } catch (e) {
      log.warn(Cat.lifecycle, "dock sizing: settle failed:", (e as Error).message);
    }
  });
}
