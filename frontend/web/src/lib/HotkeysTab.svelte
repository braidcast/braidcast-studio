<script lang="ts">
  import { obs, type Hotkey, type HotkeyCombo, type HotkeyRegisterer } from "./bridge";

  let hotkeys = $state<Hotkey[]>([]);
  let loaded = $state(false);
  let error = $state<string | null>(null);
  let filter = $state("");
  // The hotkey id currently capturing a keypress, or null when idle.
  let capturing = $state<string | null>(null);

  // DOM KeyboardEvent.code values that are lone modifiers; a capture waits past
  // these for a real key so "Ctrl+A" doesn't resolve to bare "Control".
  const MODIFIER_CODES = new Set([
    "ShiftLeft",
    "ShiftRight",
    "ControlLeft",
    "ControlRight",
    "AltLeft",
    "AltRight",
    "MetaLeft",
    "MetaRight",
  ]);

  async function loadHotkeys() {
    error = null;
    try {
      const res = await obs.call("hotkeys.list");
      hotkeys = res.hotkeys;
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void loadHotkeys();
    const off = obs.on("hotkeys.changed", () => void loadHotkeys());
    return off;
  });

  // --- grouping -------------------------------------------------------------
  // Group order: Frontend first, then Source (sub-grouped by owner), then
  // Output / Encoder / Service / Unknown. Each group renders a header + its rows.
  const GROUP_ORDER: HotkeyRegisterer[] = [
    "frontend",
    "source",
    "output",
    "encoder",
    "service",
    "unknown",
  ];
  const GROUP_LABEL: Record<HotkeyRegisterer, string> = {
    frontend: "Frontend",
    source: "Source",
    output: "Output",
    encoder: "Encoder",
    service: "Service",
    unknown: "Other",
  };

  interface HotkeyGroup {
    key: string;
    label: string;
    rows: Hotkey[];
  }

  const filtered = $derived.by(() => {
    const q = filter.trim().toLowerCase();
    if (!q) return hotkeys;
    return hotkeys.filter((h) => {
      const desc = (h.description || h.name).toLowerCase();
      const owner = (h.owner ?? "").toLowerCase();
      return desc.includes(q) || owner.includes(q);
    });
  });

  // Build display groups. Sources additionally split by owner so each source's
  // hotkeys sit under their own subheader.
  const groups = $derived.by<HotkeyGroup[]>(() => {
    const out: HotkeyGroup[] = [];
    for (const reg of GROUP_ORDER) {
      const inReg = filtered.filter((h) => h.registerer === reg);
      if (inReg.length === 0) continue;
      if (reg === "source") {
        // One subgroup per owner, owners sorted alphabetically.
        const byOwner = new Map<string, Hotkey[]>();
        for (const h of inReg) {
          const owner = h.owner ?? "(unnamed)";
          const list = byOwner.get(owner);
          if (list) list.push(h);
          else byOwner.set(owner, [h]);
        }
        for (const owner of [...byOwner.keys()].sort((a, b) => a.localeCompare(b))) {
          out.push({ key: "source:" + owner, label: GROUP_LABEL.source + " · " + owner, rows: byOwner.get(owner)! });
        }
      } else {
        out.push({ key: reg, label: GROUP_LABEL[reg], rows: inReg });
      }
    }
    return out;
  });

  function rowLabel(h: Hotkey): string {
    return h.description || h.name;
  }

  // Focus the capture button the moment it enters the active branch so its
  // keydown handler receives the next press without a second click.
  function autofocus(node: HTMLElement) {
    node.focus();
  }

  // --- capture --------------------------------------------------------------
  function startCapture(id: string) {
    capturing = id;
  }
  function cancelCapture() {
    capturing = null;
  }

  async function onCaptureKeydown(id: string, e: KeyboardEvent) {
    // Always swallow the key while capturing so app shortcuts don't also fire.
    e.preventDefault();
    e.stopPropagation();
    if (e.code === "Escape" || e.key === "Escape") {
      capturing = null;
      return;
    }
    // Wait for a non-modifier key before committing the combo.
    if (MODIFIER_CODES.has(e.code)) return;
    const combo: HotkeyCombo = {
      code: e.code,
      ctrl: e.ctrlKey,
      shift: e.shiftKey,
      alt: e.altKey,
      meta: e.metaKey,
    };
    capturing = null;
    await applyBinding(id, combo);
  }

  async function applyBinding(id: string, combo: HotkeyCombo) {
    error = null;
    try {
      const res = await obs.call("hotkeys.set", { id, bindings: [combo] });
      // Reflect the formatted display locally so the row updates without a refetch.
      hotkeys = hotkeys.map((h) => (h.id === id ? { ...h, bindings: res.bindings } : h));
    } catch (e) {
      error = (e as Error).message;
    }
  }

  async function clearBinding(id: string) {
    error = null;
    try {
      const res = await obs.call("hotkeys.clear", { id });
      hotkeys = hotkeys.map((h) => (h.id === id ? { ...h, bindings: res.bindings } : h));
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<div class="hotkeys">
  {#if error}<p class="error">{error}</p>{/if}

  <div class="searchbar">
    <input
      type="text"
      placeholder="Filter by action or source…"
      bind:value={filter}
      aria-label="Filter hotkeys"
    />
  </div>

  {#if !loaded}
    <p class="dim">Loading hotkeys…</p>
  {:else if hotkeys.length === 0}
    <p class="dim">No hotkeys registered.</p>
  {:else if groups.length === 0}
    <p class="dim">No hotkeys match “{filter}”.</p>
  {:else}
    <div class="scroll">
      {#each groups as g (g.key)}
        <section class="group">
          <h4 class="grouphead">{g.label}</h4>
          <ul class="list">
            {#each g.rows as h (h.id)}
              <li class="row">
                <span class="name" title={rowLabel(h)}>{rowLabel(h)}</span>
                <div class="rowactions">
                  {#if capturing === h.id}
                    <button
                      class="capture active"
                      use:autofocus
                      onkeydown={(e) => void onCaptureKeydown(h.id, e)}
                      onblur={cancelCapture}
                      aria-label="Press a key combination"
                    >
                      Press a key…
                    </button>
                  {:else}
                    <button
                      class="capture"
                      class:empty={h.bindings.length === 0}
                      onclick={() => startCapture(h.id)}
                      title="Click to set a key binding"
                    >
                      {#if h.bindings.length === 0}
                        Click to set…
                      {:else}
                        {#each h.bindings as b, i (i)}
                          {#if i > 0}<span class="sep">,</span>{/if}<span class="combo">{b.display}</span>
                        {/each}
                      {/if}
                    </button>
                  {/if}
                  <button
                    class="mini"
                    title="Clear binding"
                    disabled={h.bindings.length === 0}
                    onclick={() => void clearBinding(h.id)}>✕</button
                  >
                </div>
              </li>
            {/each}
          </ul>
        </section>
      {/each}
    </div>
  {/if}
</div>

<style>
  .hotkeys {
    padding: 8px 0 4px;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }
  .searchbar {
    margin-bottom: 10px;
  }
  .searchbar input {
    background: var(--color-base);
    border: 1px solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
    width: 100%;
  }
  .searchbar input:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .scroll {
    overflow: auto;
    max-height: 52vh;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .group {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .grouphead {
    margin: 0 0 2px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-muted);
    position: sticky;
    top: 0;
    background: var(--color-surface);
    padding: 4px 0;
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    padding: 6px 10px;
    border: 1px solid var(--color-border);
    background: var(--color-base);
  }
  .name {
    color: var(--color-text);
    font-size: 13px;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .rowactions {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-shrink: 0;
  }
  .capture {
    background: var(--color-surface);
    border: 1px solid var(--color-border);
    color: var(--color-text);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 4px 9px;
    min-width: 110px;
    text-align: center;
    line-height: 1.2;
  }
  .capture:hover {
    border-color: var(--color-accent);
  }
  .capture.empty {
    color: var(--color-muted);
    font-style: italic;
  }
  .capture.active {
    border-color: var(--color-accent);
    color: var(--color-accent);
    outline: none;
  }
  .combo {
    color: var(--color-text);
  }
  .sep {
    color: var(--color-muted);
    margin: 0 3px;
  }
  .mini {
    background: none;
    border: 1px solid var(--color-border);
    color: var(--color-text);
    cursor: pointer;
    font: inherit;
    font-size: 12px;
    padding: 4px 8px;
    line-height: 1;
  }
  .mini:hover:not(:disabled) {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .mini:disabled {
    opacity: 0.35;
    cursor: default;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 12px;
  }
</style>
