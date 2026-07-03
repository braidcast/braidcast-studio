import type { ITabRenderer, PanelUpdateEvent, TabPartInitParameters } from "dockview-core";
import { requestDetach } from "./detachRegistry";

// Inline-SVG chrome for the two tab affordances. Raw markup (not the Svelte <Icon>)
// because DockTab is a vanilla dockview ITabRenderer with no Svelte context. A
// pop-out / external-link glyph reads as "tear out"; an X as "close".
const DETACH_SVG =
  '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M14 4h6v6"/><path d="M20 4l-8 8"/><path d="M18 13v5a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h5"/></svg>';
const CLOSE_SVG =
  '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M5 5l14 14M19 5L5 19"/></svg>';

// A tab-header status dot only carries meaning while a canvas is connecting / live /
// errored (StudioPage recolors it live via updateParameters). Idle is the muted
// token or empty -> the dot is hidden so an idle dock shows no grey square.
const IDLE_DOT = new Set(["", "var(--color-muted)"]);

function stopPress(e: Event): void {
  e.stopPropagation();
}

// Custom dock-header chrome per the redesign mock (dockHead): an optional leading
// status dot, a title, an optional `S/S` badge, then the tear-out and close icon
// buttons. Implemented as a vanilla dockview ITabRenderer (the tab IS the dock
// header), so it stays framework-agnostic and is driven entirely off the panel's
// init/update params + its DockviewPanelApi.
//
// - close calls api.close() -> the panel is removed; the Docks menu reopens it.
// - tear-out calls requestDetach(panelId), resolved at click time against the
//   handler the main window registered (see detachRegistry) -- so a panel rebuilt
//   from a saved layout detaches identically to a freshly-added one.
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
  private _panelId = "";
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
    this._detachBtn.innerHTML = DETACH_SVG;
    this.wireButton(this._detachBtn, this.handleDetach);

    this._closeBtn = document.createElement("button");
    this._closeBtn.className = "obs-dock-btn";
    this._closeBtn.title = "Close (reopen from Docks menu)";
    this._closeBtn.innerHTML = CLOSE_SVG;
    this.wireButton(this._closeBtn, this.handleClose);

    const btns = document.createElement("span");
    btns.className = "obs-dock-btns";
    btns.appendChild(this._detachBtn);
    btns.appendChild(this._closeBtn);

    this._element.append(this._dot, this._title, this._badge, spacer, btns);
  }

  get element(): HTMLElement {
    return this._element;
  }

  // Dockview uses the tab itself as the drag handle (cursor:move). A raw click on a
  // nested button still starts that tab-drag on the preceding mousedown/pointerdown,
  // which cancels the click -> the button feels dead. Swallow the press on the button
  // (and mark it non-draggable) so only its click fires.
  private wireButton(btn: HTMLButtonElement, onClick: (e: MouseEvent) => void): void {
    btn.draggable = false;
    btn.addEventListener("mousedown", stopPress);
    btn.addEventListener("pointerdown", stopPress);
    btn.addEventListener("click", onClick);
  }

  init(params: TabPartInitParameters): void {
    this._title.textContent = params.title;
    this._params = { ...(params.params ?? {}) };
    this.render();

    this._panelId = params.api.id;
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
    this._dot.hidden = IDLE_DOT.has(dot);
    if (!this._dot.hidden) {
      this._dot.style.background = dot;
    }

    const badge = typeof p.__badge === "string" ? p.__badge : "";
    this._badge.hidden = badge === "";
    this._badge.textContent = badge;
  }

  private readonly handleDetach = (e: MouseEvent): void => {
    e.stopPropagation();
    requestDetach(this._panelId);
  };

  private readonly handleClose = (e: MouseEvent): void => {
    e.stopPropagation();
    this._onClose?.();
  };

  dispose(): void {
    for (const [btn, onClick] of [
      [this._detachBtn, this.handleDetach],
      [this._closeBtn, this.handleClose],
    ] as const) {
      btn.removeEventListener("mousedown", stopPress);
      btn.removeEventListener("pointerdown", stopPress);
      btn.removeEventListener("click", onClick);
    }
  }
}
