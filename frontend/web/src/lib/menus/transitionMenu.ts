import type { ContextMenuItem } from "$lib/menus/ContextMenu.svelte";
import { obs, type ItemTransition, type TransitionType } from "$lib/api/bridge";

// Preset durations offered in the nested "Duration ▸" submenu. A context-menu
// radio can't host a free-form numeric field, and the codebase has no existing
// numeric-prompt dialog to reuse (grepped), so presets are the lighter option
// for this pass -- matches OBS's own quick-pick durations.
const DURATION_PRESETS_MS = [300, 500, 1000, 2000];

// The registered transition types are fetched once and cached for the process
// lifetime (the set is fixed at startup -- same list transitionTypes.list backs
// the Transitions dock with). Shared here so both scene-item context menus
// (SourcesDock, CanvasDock) hit the bridge once between them, not per-open.
let typesPromise: Promise<TransitionType[]> | null = null;

export function transitionTypes(): Promise<TransitionType[]> {
  if (!typesPromise) {
    typesPromise = obs.call("transitionTypes.list").catch((e) => {
      typesPromise = null;
      throw e;
    });
  }
  return typesPromise;
}

function durationSubmenu(current: number, onPick: (ms: number) => void): ContextMenuItem {
  return {
    label: "Duration",
    children: DURATION_PRESETS_MS.map((ms) => ({
      label: `${ms} ms`,
      checked: current === ms,
      action: () => onPick(ms),
    })),
  };
}

// A "Show Transition ▸" / "Hide Transition ▸" submenu: None + one radio-checked
// entry per registered transition type, plus a nested Duration preset submenu.
// `current` is the item's showTransition/hideTransition field (null = None).
function transitionSubmenu(
  label: string,
  current: ItemTransition | null,
  types: TransitionType[],
  onPickType: (type: string | null) => void,
  onPickDuration: (ms: number) => void,
): ContextMenuItem {
  const currentType = current?.type ?? "";
  return {
    label,
    children: [
      { label: "None", checked: !currentType, action: () => onPickType(null) },
      ...types.map((t) => ({
        label: t.name,
        checked: currentType === t.id,
        action: () => onPickType(t.id),
      })),
      null,
      durationSubmenu(current?.duration ?? DURATION_PRESETS_MS[0], onPickDuration),
    ],
  };
}

export function showTransitionMenu(
  current: ItemTransition | null,
  types: TransitionType[],
  onPickType: (type: string | null) => void,
  onPickDuration: (ms: number) => void,
): ContextMenuItem {
  return transitionSubmenu("Show Transition", current, types, onPickType, onPickDuration);
}

export function hideTransitionMenu(
  current: ItemTransition | null,
  types: TransitionType[],
  onPickType: (type: string | null) => void,
  onPickDuration: (ms: number) => void,
): ContextMenuItem {
  return transitionSubmenu("Hide Transition", current, types, onPickType, onPickDuration);
}
