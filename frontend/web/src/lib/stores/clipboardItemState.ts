import { obs, type SceneItem } from "$lib/api/bridge";
import { clipboard, type CopiedItemState } from "$lib/stores/clipboardStore.svelte";

// Addresses one scene item across the global (canvas omitted) and additional-canvas
// (canvas set) paths — the shape every sceneItems.* setter already accepts.
export interface ItemTarget {
  canvas?: string;
  scene?: string | null;
  id: number;
}

// A paste destination: the current scene (+ canvas for an additional-canvas dock).
export interface SceneTarget {
  canvas?: string;
  scene: string | null;
}

// Copy a scene item: capture its full visual state (transform via the bridge; the
// appearance fields ride along on the row from sceneItems.list) plus its locator
// into the clipboard. Every copy entry point funnels through here so the reference
// AND the carried state stay consistent across the keyboard and all dock menus.
export async function copyItem(target: ItemTarget, item: SceneItem): Promise<void> {
  if (!item.source) {
    return;
  }
  let state: CopiedItemState | undefined;
  try {
    const transform = await obs.call("sceneItems.getTransform", target);
    state = {
      transform,
      blendMode: item.blendMode,
      blendMethod: item.blendMethod,
      scaleFilter: item.scaleFilter,
      color: item.color,
      visible: item.visible,
      showTransition: item.showTransition,
      hideTransition: item.hideTransition,
    };
  } catch {
    // getTransform failed — still copy the reference so a plain paste works.
    state = undefined;
  }
  clipboard.source = {
    ref: item.source,
    name: item.source,
    origin: { canvas: target.canvas, scene: target.scene ?? null, id: item.id },
    state,
  };
}

// Apply a captured state onto a freshly added/duplicated item. Each setter is
// guarded so a field absent from the capture is skipped, never written as
// undefined. THE single apply path both paste routes (reference + duplicate) share.
export async function applyItemState(target: ItemTarget, state: CopiedItemState | undefined): Promise<void> {
  if (!state) {
    return;
  }
  const calls: Promise<unknown>[] = [];
  if (state.transform) {
    calls.push(obs.call("sceneItems.setTransform", { ...target, transform: state.transform }));
  }
  if (state.blendMode) {
    calls.push(obs.call("sceneItems.setBlendingMode", { ...target, mode: state.blendMode }));
  }
  if (state.blendMethod) {
    calls.push(obs.call("sceneItems.setBlendingMethod", { ...target, method: state.blendMethod }));
  }
  if (state.scaleFilter) {
    calls.push(obs.call("sceneItems.setScaleFilter", { ...target, filter: state.scaleFilter }));
  }
  if (state.color !== undefined) {
    calls.push(obs.call("sceneItems.setColor", { ...target, color: state.color }));
  }
  if (state.visible !== undefined) {
    calls.push(obs.call("sceneItems.setVisible", { ...target, visible: state.visible }));
  }
  if (state.showTransition) {
    calls.push(
      obs.call("sceneItems.setShowTransition", {
        ...target,
        transition: state.showTransition.type,
        duration: state.showTransition.duration,
      }),
    );
  }
  if (state.hideTransition) {
    calls.push(
      obs.call("sceneItems.setHideTransition", {
        ...target,
        transition: state.hideTransition.type,
        duration: state.hideTransition.duration,
      }),
    );
  }
  await Promise.all(calls);
}

// Paste (Reference): add the copied source into `target` as a reference, then apply
// the carried state to the new item. No-ops (no throw) when the clipboard is empty
// or there is no scene to paste into.
export async function pasteReference(target: SceneTarget): Promise<void> {
  const src = clipboard.source;
  if (!src || !target.scene) {
    return;
  }
  const res = await obs.call("sources.addExisting", { ...target, name: src.ref });
  await applyItemState({ ...target, id: res.id }, src.state);
}

// Paste (Duplicate): duplicate the copied item's SOURCE (an independent copy) into
// the PASTE TARGET's scene/canvas via the name-based sources.duplicateInto (copies
// the source but NOT the scene-item appearance fields), then apply the carried state
// onto the new item so it matches the original. Falls back to a reference paste when
// the copy carries no locator (a bare/legacy clipboard entry).
export async function pasteDuplicate(target: SceneTarget): Promise<void> {
  const src = clipboard.source;
  if (!src || !target.scene) {
    return;
  }
  if (!src.origin) {
    await pasteReference(target);
    return;
  }
  const dup = await obs.call("sources.duplicateInto", {
    source: src.ref,
    scene: target.scene,
    canvas: target.canvas,
  });
  await applyItemState({ ...target, id: dup.id }, src.state);
}
