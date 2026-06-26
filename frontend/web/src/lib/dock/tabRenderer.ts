import type { ITabRenderer, PanelUpdateEvent, TabPartInitParameters } from "dockview-core";

// Custom dock-header chrome per the redesign mock (dockHead): an optional leading
// status dot, a title, an optional `S/S` badge, then the `⧉` tear-out and `✕` close
// affordances. Implemented as a vanilla dockview ITabRenderer (the tab IS the dock
// header), so it stays framework-agnostic and is driven entirely off the panel's
// init/update params + its DockviewPanelApi.
//
// - `✕` calls api.close() -> the panel is removed; the Docks menu reopens it.
// - `⧉` calls the detachDock seam threaded through params.__detach (P1: logs only;
//   real floating-window detach lands next phase per the P1 floating-deferral).
// - `__accent` tints the title (and badge) to match the accent docks in the mock.
// - `__dot` (CSS color) shows a 7px status dot; `__badge` (text) shows the mono
//   `GLOBAL/OWN S/S` badge on canvas docks. Both absent -> nothing renders, so the
//   plain docks (Scenes/Sources/...) are unchanged.
//
// Header chrome is styled in app.css against these class names so it re-skins live
// with the active theme.
export class DockTab implements ITabRenderer {
  private readonly _element: HTMLDivElement;
  private readonly _dot: HTMLSpanElement;
  private readonly _title: HTMLSpanElement;
  private readonly _badge: HTMLSpanElement;
  private readonly _detachBtn: HTMLButtonElement;
  private readonly _closeBtn: HTMLButtonElement;
  private _params: Record<string, unknown> = {};
  private _onDetach: (() => void) | undefined;
  private _onClose: (() => void) | undefined;

  constructor() {
    this._element = document.createElement("div");
    this._element.className = "obs-dock-tab";

    this._dot = document.createElement("span");
    this._dot.className = "obs-dock-dot";
    this._dot.hidden = true;

    this._title = document.createElement("span");
    this._title.className = "obs-dock-title";

    this._badge = document.createElement("span");
    this._badge.className = "obs-dock-badge";
    this._badge.hidden = true;

    const spacer = document.createElement("span");
    spacer.className = "obs-dock-spacer";

    this._detachBtn = document.createElement("button");
    this._detachBtn.className = "obs-dock-btn";
    this._detachBtn.title = "Tear out to its own window";
    this._detachBtn.textContent = "⧉"; // ⧉
    this._detachBtn.addEventListener("click", this.handleDetach);

    this._closeBtn = document.createElement("button");
    this._closeBtn.className = "obs-dock-btn";
    this._closeBtn.title = "Close (reopen from Docks menu)";
    this._closeBtn.textContent = "✕"; // ✕
    this._closeBtn.addEventListener("click", this.handleClose);

    const btns = document.createElement("span");
    btns.className = "obs-dock-btns";
    btns.appendChild(this._detachBtn);
    btns.appendChild(this._closeBtn);

    this._element.append(this._dot, this._title, this._badge, spacer, btns);
  }

  get element(): HTMLElement {
    return this._element;
  }

  init(params: TabPartInitParameters): void {
    this._title.textContent = params.title;
    this._params = { ...(params.params ?? {}) };
    this.render();

    const panelId = params.api.id;
    const detach = this._params.__detach as ((id: string) => void) | undefined;
    this._onDetach = detach ? () => detach(panelId) : undefined;
    this._onClose = () => params.api.close();
  }

  update(event: PanelUpdateEvent): void {
    this._params = { ...this._params, ...(event.params ?? {}) };
    this.render();
  }

  private render(): void {
    const p = this._params;
    this._element.classList.toggle("accent", p.__accent === true);

    const dot = typeof p.__dot === "string" ? p.__dot : "";
    this._dot.hidden = dot === "";
    if (dot) {
      this._dot.style.background = dot;
    }

    const badge = typeof p.__badge === "string" ? p.__badge : "";
    this._badge.hidden = badge === "";
    this._badge.textContent = badge;
  }

  private readonly handleDetach = (e: MouseEvent): void => {
    e.stopPropagation();
    this._onDetach?.();
  };

  private readonly handleClose = (e: MouseEvent): void => {
    e.stopPropagation();
    this._onClose?.();
  };

  dispose(): void {
    this._detachBtn.removeEventListener("click", this.handleDetach);
    this._closeBtn.removeEventListener("click", this.handleClose);
  }
}
