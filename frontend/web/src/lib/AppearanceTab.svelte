<script lang="ts">
  import { themeStore } from "./theme/themeStore.svelte";
  import { ACCENT_VALUES } from "./theme/presets";
  import type { AccentName, ThemeMode, ThemeTokens } from "./theme/tokens";
  import Segmented, { type SegmentedOption } from "./Segmented.svelte";
  import Icon from "./dock/Icon.svelte";

  // Variant C — Control-Panel Grid. A dense telemetry strip on top (accent /
  // mode / density / element-style toggles), then three working columns:
  // palette editor, typography + presets, and a live preview. Everything is a
  // live tweak on the global theme store (applies + persists at once).

  // --- quick axes ------------------------------------------------------------

  // Accent swatches in the mock's order; the color is the source-of-truth ACCENT_VALUES.
  const ACCENTS: AccentName[] = ["amber", "blue", "violet", "emerald", "rose"];

  const MODE_OPTS: SegmentedOption[] = [
    { label: "Dark", value: "dark" },
    { label: "Light", value: "light" },
  ];
  const DENSITY_OPTS: SegmentedOption[] = [
    { label: "Comfortable", value: "comfortable" },
    { label: "Compact", value: "compact" },
  ];

  // --- data-driven axis tables (DRY) -----------------------------------------

  // One entry per color token; the short label is the source-of-truth name.
  const PALETTE: { key: keyof ThemeTokens; label: string }[] = [
    { key: "colorBase", label: "base" },
    { key: "colorRail", label: "rail" },
    { key: "colorSurface", label: "surface" },
    { key: "colorSurface2", label: "surf-2" },
    { key: "colorBorder", label: "border" },
    { key: "colorBorder2", label: "bord-2" },
    { key: "colorText", label: "text" },
    { key: "colorDim", label: "dim" },
    { key: "colorMuted", label: "muted" },
    { key: "colorAccent", label: "accent" },
    { key: "colorAccentContrast", label: "accent-c" },
    { key: "colorLive", label: "live" },
    { key: "meterGreen", label: "mtr-grn" },
    { key: "meterYellow", label: "mtr-yel" },
    { key: "meterRed", label: "mtr-red" },
  ];

  // Palette tokens grouped by role (Variant C group micro-labels). Keys resolve
  // their display label + live value from PALETTE, keeping it the single source.
  const PALETTE_LABEL = new Map(PALETTE.map((p) => [p.key, p.label]));
  const PALETTE_GROUPS: { group: string; keys: (keyof ThemeTokens)[] }[] = [
    { group: "Surfaces", keys: ["colorBase", "colorRail", "colorSurface", "colorSurface2"] },
    { group: "Lines", keys: ["colorBorder", "colorBorder2"] },
    { group: "Text", keys: ["colorText", "colorDim", "colorMuted"] },
    { group: "Accent & Status", keys: ["colorAccent", "colorAccentContrast", "colorLive"] },
    { group: "Meters", keys: ["meterGreen", "meterYellow", "meterRed"] },
  ];

  // Font stacks the presets use plus a few common extras. If the live value isn't
  // listed (e.g. an imported custom), it's prepended as a synthetic option so the
  // <select> still reflects it.
  const FONT_OPTIONS: { value: string; label: string }[] = [
    { value: "'Geist', 'Segoe UI', system-ui, sans-serif", label: "Geist" },
    { value: "'Geist Mono', ui-monospace, monospace", label: "Geist Mono" },
    { value: "'Consolas', 'SF Mono', ui-monospace, monospace", label: "Consolas / Mono" },
    { value: "'JetBrains Mono', ui-monospace, monospace", label: "JetBrains Mono" },
    { value: "'Segoe UI', system-ui, sans-serif", label: "Segoe UI" },
    { value: "'Inter', system-ui, sans-serif", label: "Inter" },
    { value: "system-ui, sans-serif", label: "System sans" },
    { value: "ui-monospace, monospace", label: "Monospace" },
  ];

  function fontOptionsFor(current: string): { value: string; label: string }[] {
    if (FONT_OPTIONS.some((o) => o.value === current)) {
      return FONT_OPTIONS;
    }
    return [{ value: current, label: "Custom (" + current.split(",")[0].replace(/['"]/g, "") + ")" }, ...FONT_OPTIONS];
  }

  const uiFontOptions = $derived(fontOptionsFor(themeStore.tokens.fontUi));
  const monoFontOptions = $derived(fontOptionsFor(themeStore.tokens.fontMono));

  const LABEL_CASE_OPTS: SegmentedOption[] = [
    { label: "Aa", value: "none" },
    { label: "AA", value: "uppercase" },
  ];
  const LETTER_SPACING_OPTS: SegmentedOption[] = [
    { label: "0", value: "0" },
    { label: ".04", value: "0.04em" },
    { label: ".08", value: "0.08em" },
    { label: ".1", value: "0.1em" },
  ];
  const METER_OPTS: SegmentedOption[] = [
    { label: "Gradient", value: "gradient" },
    { label: "Segmented", value: "segmented" },
  ];
  const SELECTION_OPTS: SegmentedOption[] = [
    { label: "Left bar", value: "left-bar" },
    { label: "Fill", value: "fill" },
  ];
  const BORDER_OPTS: SegmentedOption[] = [
    { label: "1px", value: "1px" },
    { label: "2px", value: "2px" },
  ];

  // --- preset save (inline-rename pattern, mirrors ScenesDock) ----------------

  let saving = $state(false);
  let saveName = $state("");

  function focusOnMount(node: HTMLInputElement) {
    node.focus();
    node.select();
  }
  function beginSave() {
    saving = true;
    saveName = "";
  }
  function commitSave() {
    const name = saveName.trim();
    saving = false;
    if (name) {
      themeStore.saveCustom(name);
    }
  }
  function onSaveKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      commitSave();
    } else if (e.key === "Escape") {
      saveName = "";
      saving = false;
    }
  }

  function onColorInput(key: keyof ThemeTokens, e: Event) {
    themeStore.setToken(key, (e.currentTarget as HTMLInputElement).value);
  }
</script>

<div class="appearance">
  <!-- crumb header -->
  <div class="crumb">
    <span class="dot"></span>
    <span class="c1">Settings</span>
    <span class="sep">/</span>
    <h2 class="c2">Appearance</h2>
    <span class="spacer"></span>
    <span class="live-note">Edits apply live · saved to theme</span>
  </div>

  <!-- telemetry strip -->
  <div class="strip">
    <div class="unit">
      <div class="flabel">Accent</div>
      <div class="accents">
        {#each ACCENTS as a (a)}
          <button
            class="acc"
            class:on={themeStore.tokens.accent === a}
            title={a}
            aria-label={a}
            aria-pressed={themeStore.tokens.accent === a}
            style:background={ACCENT_VALUES[a].accent}
            onclick={() => themeStore.setToken("accent", a)}
          ></button>
        {/each}
      </div>
    </div>
    <div class="unit">
      <div class="flabel">Mode</div>
      <Segmented
        options={MODE_OPTS}
        value={themeStore.tokens.mode}
        onChange={(v) => themeStore.setToken("mode", v as ThemeMode)}
      />
    </div>
    <div class="unit">
      <div class="flabel">Density</div>
      <Segmented
        options={DENSITY_OPTS}
        value={themeStore.tokens.density}
        onChange={(v) => themeStore.setToken("density", v as ThemeTokens["density"])}
      />
    </div>
    <div class="unit elem">
      <div class="flabel">Element Styles</div>
      <div class="mini">
        <Segmented
          options={METER_OPTS}
          value={themeStore.tokens.meterStyle}
          onChange={(v) => themeStore.setToken("meterStyle", v as ThemeTokens["meterStyle"])}
        />
        <Segmented
          options={SELECTION_OPTS}
          value={themeStore.tokens.selectionStyle}
          onChange={(v) => themeStore.setToken("selectionStyle", v as ThemeTokens["selectionStyle"])}
        />
        <Segmented
          options={BORDER_OPTS}
          value={themeStore.tokens.borderWeight}
          onChange={(v) => themeStore.setToken("borderWeight", v)}
        />
      </div>
    </div>
  </div>

  <!-- three working columns -->
  <div class="cols">
    <!-- col 1: palette -->
    <div class="col">
      <section class="panel">
        <div class="panel__hd">
          <h3>Palette</h3>
          <span class="count">{PALETTE.length} tokens</span>
        </div>
        <div class="panel__bd">
          {#each PALETTE_GROUPS as grp (grp.group)}
            <div class="tokgroup">
              <div class="tokgroup__l">{grp.group}</div>
              <div class="tokgrid">
                {#each grp.keys as key (key)}
                  <label class="tok" style:background={themeStore.tokens[key]} title={PALETTE_LABEL.get(key)}>
                    <input
                      type="color"
                      value={themeStore.tokens[key]}
                      oninput={(e) => onColorInput(key, e)}
                      aria-label={PALETTE_LABEL.get(key)}
                    />
                    <span class="tok__cap">
                      <span class="tok__name">{PALETTE_LABEL.get(key)}</span>
                      <span class="tok__hex">{String(themeStore.tokens[key]).toUpperCase()}</span>
                    </span>
                  </label>
                {/each}
              </div>
            </div>
          {/each}
        </div>
      </section>
    </div>

    <!-- col 2: typography + presets -->
    <div class="col">
      <section class="panel">
        <div class="panel__hd"><h3>Typography</h3></div>
        <div class="panel__bd">
          <div class="crow stack">
            <span class="crow__l">UI Font</span>
            <select
              class="pick"
              value={themeStore.tokens.fontUi}
              onchange={(e) => themeStore.setToken("fontUi", (e.currentTarget as HTMLSelectElement).value)}
            >
              {#each uiFontOptions as o (o.value)}
                <option value={o.value}>{o.label}</option>
              {/each}
            </select>
          </div>
          <div class="crow stack">
            <span class="crow__l">Mono Font</span>
            <select
              class="pick"
              value={themeStore.tokens.fontMono}
              onchange={(e) => themeStore.setToken("fontMono", (e.currentTarget as HTMLSelectElement).value)}
            >
              {#each monoFontOptions as o (o.value)}
                <option value={o.value}>{o.label}</option>
              {/each}
            </select>
          </div>
          <div class="crow">
            <span class="crow__l">Label Case</span>
            <Segmented
              options={LABEL_CASE_OPTS}
              value={themeStore.tokens.labelCase}
              onChange={(v) => themeStore.setToken("labelCase", v as ThemeTokens["labelCase"])}
            />
          </div>
          <div class="crow">
            <span class="crow__l">Spacing</span>
            <Segmented
              options={LETTER_SPACING_OPTS}
              value={themeStore.tokens.letterSpacing}
              onChange={(v) => themeStore.setToken("letterSpacing", v)}
            />
          </div>
        </div>
      </section>

      <section class="panel">
        <div class="panel__hd">
          <h3>Presets</h3>
          <span class="count">{themeStore.allThemes.length} saved</span>
        </div>
        <div class="panel__bd">
          <div class="presets">
            {#each themeStore.allThemes as t (t.id)}
              <button
                class="preset"
                class:on={themeStore.activeId === t.id}
                onclick={() => themeStore.selectPreset(t.id)}
              >
                <span class="swz" style:background={t.tokens.colorAccent}></span>
                <span class="preset-name">{t.name}</span>
                {#if t.id.startsWith("custom-")}
                  <span
                    class="preset-del"
                    role="button"
                    tabindex="0"
                    title="Delete theme"
                    aria-label="Delete theme"
                    onclick={(e) => {
                      e.stopPropagation();
                      themeStore.deleteCustom(t.id);
                    }}
                    onkeydown={(e) => {
                      if (e.key === "Enter" || e.key === " ") {
                        e.stopPropagation();
                        themeStore.deleteCustom(t.id);
                      }
                    }}
                  >
                    <Icon name="x" size={11} />
                  </span>
                {/if}
              </button>
            {/each}
            {#if saving}
              <input
                class="save-input"
                placeholder="Theme name"
                bind:value={saveName}
                onkeydown={onSaveKey}
                onblur={commitSave}
                use:focusOnMount
              />
            {:else}
              <button class="preset save" onclick={beginSave}>
                <Icon name="plus" size={11} />
                Save…
              </button>
            {/if}
          </div>
        </div>
      </section>
    </div>

    <!-- col 3: live preview (pure token-var DOM; repaints on every setToken) -->
    <div class="col">
      <section class="panel">
        <div class="panel__hd"><h3>Live Preview</h3></div>
        <div class="panel__bd">
          <div class="prev">
            <div class="prev-hd">
              <span>Scenes</span>
              <span class="win"><Icon name="window-restore" size={11} /><Icon name="x" size={11} /></span>
            </div>
            <div class="prev-bd">
              <div class="prev-it">Intro</div>
              <div class="prev-it sel">Live · Main</div>
              <div class="prev-it">BRB</div>
              <div class="prev-btn"><i></i>Go Live</div>
              <div class="prev-mt"><div class="prev-mt-unlit"></div></div>
            </div>
          </div>
          <p class="cap">Editing any token repaints this instantly.</p>
        </div>
      </section>
    </div>
  </div>
</div>

<style>
  .appearance {
    max-width: 1180px;
  }

  /* --- crumb header ---------------------------------------------------------- */
  .crumb {
    display: flex;
    align-items: center;
    gap: 9px;
    margin-bottom: 16px;
  }
  .crumb .dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--color-accent);
  }
  .crumb .c1 {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--color-dim);
  }
  .crumb .sep {
    color: var(--color-muted);
  }
  .crumb .c2 {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 400;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .crumb .spacer {
    flex: 1;
  }
  .crumb .live-note {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--color-muted);
    border: var(--border-weight) solid var(--color-border-2);
    padding: 3px 8px;
    white-space: nowrap;
  }

  /* --- telemetry strip ------------------------------------------------------- */
  .strip {
    display: flex;
    flex-wrap: wrap;
    gap: 14px 22px;
    align-items: flex-start;
    padding: 12px 16px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
    margin-bottom: 14px;
  }
  .unit {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-width: 0;
  }
  .unit.elem {
    margin-left: auto;
  }
  .mini {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
  }
  .flabel {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.09em;
    text-transform: uppercase;
    color: var(--color-dim);
  }
  .accents {
    display: flex;
    gap: 9px;
  }
  .acc {
    width: 30px;
    height: 30px;
    flex: 0 0 auto;
    padding: 0;
    cursor: pointer;
    border: 0;
    position: relative;
    box-shadow: 0 0 0 1px var(--color-border);
  }
  .acc.on {
    box-shadow: 0 0 0 2px var(--color-text);
  }
  .acc.on::after {
    content: "";
    position: absolute;
    inset: 0;
    border: 2px solid var(--color-base);
  }

  /* --- three-column body ----------------------------------------------------- */
  .cols {
    display: grid;
    grid-template-columns: minmax(0, 1.15fr) minmax(0, 0.9fr) minmax(0, 300px);
    gap: 14px;
    align-items: start;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 14px;
    min-width: 0;
  }

  /* --- panel primitive ------------------------------------------------------- */
  .panel {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
  }
  .panel__hd {
    display: flex;
    align-items: center;
    gap: 9px;
    padding: 9px 13px;
    background: var(--color-surface-2);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .panel__hd::before {
    content: "";
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--color-accent);
  }
  .panel__hd h3 {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.14em;
    text-transform: uppercase;
    color: var(--color-dim);
    font-weight: 600;
  }
  .panel__hd .count {
    margin-left: auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.06em;
    color: var(--color-muted);
  }
  .panel__bd {
    padding: 13px;
  }

  /* --- palette: fully-filled token rectangles -------------------------------- */
  .tokgroup {
    margin-bottom: 14px;
  }
  .tokgroup:last-child {
    margin-bottom: 0;
  }
  .tokgroup__l {
    display: flex;
    align-items: center;
    gap: 8px;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin-bottom: 8px;
  }
  .tokgroup__l::after {
    content: "";
    flex: 1;
    height: 1px;
    background: var(--color-border-2);
  }
  /* 1px grid gap on a border-colored bed => hairline seams between fills. */
  .tokgrid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 1px;
    background: var(--color-border);
    border: var(--border-weight) solid var(--color-border);
  }
  /* The rectangle IS the swatch: its background is the token color; a caption
     strip on --color-surface carries the name + hex so text stays legible on
     ANY fill (dark bases through light text tokens) without computing contrast. */
  .tok {
    position: relative;
    display: flex;
    flex-direction: column;
    justify-content: flex-end;
    min-height: 46px;
    cursor: pointer;
    overflow: hidden;
  }
  .tok input[type="color"] {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    padding: 0;
    margin: 0;
    border: 0;
    background: none;
    opacity: 0;
    cursor: pointer;
    z-index: 1;
  }
  .tok__cap {
    position: relative;
    z-index: 0;
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 6px;
    padding: 4px 7px;
    background: var(--color-surface);
    border-top: var(--border-weight) solid var(--color-border);
  }
  .tok__name {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    color: var(--color-dim);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .tok__hex {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.04em;
    color: var(--color-muted);
    white-space: nowrap;
  }
  .tok:hover .tok__hex {
    color: var(--color-accent);
  }

  /* --- typography controls --------------------------------------------------- */
  .crow {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 8px 0;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .crow:first-child {
    padding-top: 0;
  }
  .crow:last-child {
    padding-bottom: 0;
    border-bottom: none;
  }
  .crow.stack {
    flex-direction: column;
    align-items: stretch;
    gap: 7px;
  }
  .crow__l {
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.07em;
    text-transform: uppercase;
    color: var(--color-muted);
    flex: 0 0 auto;
  }
  .pick {
    width: 100%;
    height: 32px;
    padding: 0 30px 0 11px;
    background: var(--color-surface-2);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 12px;
    appearance: none;
    -webkit-appearance: none;
    cursor: pointer;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='0 0 24 24' fill='none' stroke='%23888890' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M5 9l7 7 7-7'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: right 10px center;
    background-size: 10px;
  }
  .pick:hover {
    border-color: var(--color-muted);
  }
  .pick:focus {
    outline: none;
    border-color: var(--color-accent);
  }

  /* --- presets --------------------------------------------------------------- */
  .presets {
    display: flex;
    gap: 7px;
    flex-wrap: wrap;
    align-items: center;
  }
  .preset {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    height: 30px;
    padding: 0 12px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-dim);
    font-family: var(--font-mono);
    font-size: 10px;
    letter-spacing: 0.06em;
    text-transform: uppercase;
  }
  .preset:hover {
    border-color: var(--color-accent);
    color: var(--color-text);
  }
  .preset.on {
    border-color: var(--color-accent);
    color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  .swz {
    width: 9px;
    height: 9px;
    flex: 0 0 auto;
    box-shadow: 0 0 0 1px var(--color-border);
  }
  .preset-name {
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .preset-del {
    display: inline-flex;
    align-items: center;
    color: var(--color-muted);
  }
  .preset-del:hover {
    color: var(--color-live);
  }
  .preset.save {
    border-style: dashed;
    color: var(--color-muted);
  }
  .preset.save:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .save-input {
    height: 30px;
    padding: 0 10px;
    background: var(--color-surface-2);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    width: 130px;
  }
  .save-input:focus {
    outline: none;
  }

  /* --- live preview: pure token-var DOM, repaints on every setToken ---------- */
  .prev {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    font-family: var(--font-mono);
  }
  .prev-hd {
    display: flex;
    justify-content: space-between;
    align-items: center;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
    padding: 7px 10px;
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-accent);
  }
  .prev-hd .win {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    color: var(--color-muted);
  }
  .prev-bd {
    padding: 10px;
  }
  .prev-it {
    font-size: 11px;
    color: var(--color-text);
    padding: 6px 8px;
    border-bottom: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  :root[data-selection-style="left-bar"] .prev-it.sel {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 14%, transparent);
    color: var(--color-accent);
  }
  :root[data-selection-style="fill"] .prev-it.sel {
    background: color-mix(in srgb, var(--color-accent) 24%, transparent);
    color: var(--color-accent);
  }
  .prev-btn {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 7px;
    background: var(--color-accent);
    color: var(--color-accent-contrast);
    font-size: 11px;
    padding: 8px;
    text-align: center;
    margin-top: 9px;
    font-weight: 700;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
  }
  .prev-btn i {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
    background: var(--color-accent-contrast);
  }
  /* Meter idiom from AudioMixerDock: zone gradient track + segmented mask. */
  .prev-mt {
    position: relative;
    height: 9px;
    margin-top: 9px;
    overflow: hidden;
    background-color: var(--color-base);
    background-image: linear-gradient(
      90deg,
      var(--meter-green) 0%,
      var(--meter-green) 58%,
      var(--meter-yellow) 78%,
      var(--meter-red) 100%
    );
  }
  :root[data-meter-style="segmented"] .prev-mt {
    -webkit-mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
    mask-image: repeating-linear-gradient(90deg, #000 0 4px, transparent 4px 5px);
  }
  .prev-mt-unlit {
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    width: 32%;
    background: var(--color-base);
  }
  .cap {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--color-muted);
    margin: 9px 0 0;
    line-height: 1.6;
  }

  /* Variant C is dense; collapse the three columns before the app body would
     scroll horizontally on a narrow settings pane. */
  @media (max-width: 900px) {
    .cols {
      grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    }
    .col:last-child {
      grid-column: 1 / -1;
    }
  }
  @media (max-width: 620px) {
    .cols {
      grid-template-columns: minmax(0, 1fr);
    }
    .col:last-child {
      grid-column: auto;
    }
    .unit.elem {
      margin-left: 0;
    }
  }
</style>
