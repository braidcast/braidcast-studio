<script lang="ts">
  // The value control for an `animation` field: the layer's presets as a grid of tiles, each
  // playing itself on hover, on focus and on tap, then the knobs the chosen preset uses.
  // Tiles play through the overlay's own engine, so a tile shows exactly the motion the
  // stream will -- the same presets, easings and reduced-motion rule.
  //
  // The stored value is { preset, ...knobs }. A knob the user never touched stays absent and
  // takes the preset's default, so choosing a preset stores only its id; switching presets
  // keeps speed and delay (they mean the same thing for every preset) and drops the rest.
  import { onDestroy } from "svelte";
  import Segmented from "$lib/ui/Segmented.svelte";
  import { isPlainObject } from "$lib/utils/plainObject";
  import { animate, applyTextFx } from "../../overlay/animation/engine";
  import { EASING_NAMES, EASINGS } from "../../overlay/animation/easing";
  import {
    KNOB_RANGES,
    normalizeParams,
    presetIdOf,
    type AnimationValue,
    type Knob,
  } from "../../overlay/animation/params";
  import { LAYER_FALLBACK, presetsFor, type Layer, type Preset } from "../../overlay/animation/presets";

  let {
    value,
    layer,
    name,
    ariaDescribedBy,
    onChange,
  }: {
    value: unknown;
    layer: Layer;
    name: string;
    ariaDescribedBy?: string;
    onChange: (next: AnimationValue) => void;
  } = $props();

  /** Knobs that survive a preset switch: they mean the same for every preset. */
  const PORTABLE_KNOBS = ["speed", "delay", "target"] as const;
  const DIRECTION_LABELS: Record<string, string> = { up: "Up", down: "Down", left: "Left", right: "Right" };
  const TARGET_OPTIONS = [
    { label: "Whole message", value: "all" },
    { label: "Names and amounts", value: "vars" },
  ];
  const SAMPLE_TEXT = "Aa";

  const presets = $derived(presetsFor(layer));
  const current = $derived<Record<string, unknown>>(isPlainObject(value) ? value : {});
  const selected = $derived<Preset>(
    presets.find((p) => p.id === presetIdOf(value)) ??
      presets.find((p) => p.id === LAYER_FALLBACK[layer]) ??
      presets[0],
  );
  const groups = $derived.by(() => {
    const out: { category: string; items: Preset[] }[] = [];
    for (const p of presets) {
      const g = out.find((x) => x.category === p.category);
      if (g) {
        g.items.push(p);
      } else {
        out.push({ category: p.category, items: [p] });
      }
    }
    return out;
  });
  // A preset that does nothing ("none") has no knobs worth showing, the text target included.
  const nothing = $derived(selected.keyframes(normalizeParams({}, selected.defaults)).length === 0);

  function choose(p: Preset): void {
    const next: AnimationValue = { preset: p.id };
    for (const k of PORTABLE_KNOBS) {
      if (current[k] !== undefined) {
        (next as unknown as Record<string, unknown>)[k] = current[k];
      }
    }
    onChange(next);
  }

  function setKnob(k: Knob | "target", v: unknown): void {
    onChange({ ...(current as unknown as AnimationValue), preset: selected.id, [k]: v });
  }

  function num(k: "speed" | "intensity" | "delay"): number {
    const v = current[k];
    if (typeof v === "number" && Number.isFinite(v)) {
      return v;
    }
    if (k === "intensity" && selected.defaults.intensity !== undefined) {
      return selected.defaults.intensity;
    }
    return KNOB_RANGES[k].fallback;
  }
  const easing = $derived(typeof current.easing === "string" ? current.easing : selected.defaults.easing);
  const direction = $derived(
    typeof current.direction === "string" ? current.direction : (selected.defaults.direction ?? "up"),
  );

  // One tile plays at a time; leaving it, or starting another, stops it. Looping presets
  // (idle, some text effects) would otherwise run forever under a pointer that moved on.
  let playing: Animation[] = [];
  function stop(): void {
    playing.forEach((a) => a.cancel());
    playing = [];
  }
  function play(demo: HTMLElement | null, p: Preset): void {
    stop();
    if (!demo) {
      return;
    }
    // The tile previews the preset with this field's own knobs, so tuning speed and then
    // hovering a neighbour compares like with like.
    const v = { ...current, preset: p.id };
    if (layer === "text") {
      playing = applyTextFx(demo, [{ text: SAMPLE_TEXT, key: null }], v);
      return;
    }
    const a = animate(demo, layer, v);
    playing = a ? [a] : [];
  }
  onDestroy(stop);

  const demos: Record<string, HTMLElement | null> = {};

  // Arrow keys move focus between tiles (which previews them) without choosing one: choosing
  // saves and replays the stage, which browsing must not do. One tile is in the tab order --
  // the chosen one -- so the grid is a single Tab stop.
  const STEP: Record<string, number> = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 };
  function onGridKey(e: KeyboardEvent): void {
    const tiles = [...(e.currentTarget as HTMLElement).querySelectorAll<HTMLButtonElement>(".ap__tile")];
    const at = tiles.indexOf(document.activeElement as HTMLButtonElement);
    let next = -1;
    if (e.key in STEP) {
      next = (at + STEP[e.key] + tiles.length) % tiles.length;
    } else if (e.key === "Home") {
      next = 0;
    } else if (e.key === "End") {
      next = tiles.length - 1;
    }
    if (next >= 0 && at >= 0) {
      e.preventDefault();
      tiles[next].focus();
    }
  }
</script>

<div class="ap">
  <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
  <div class="ap__grid" role="group" aria-label={name} aria-describedby={ariaDescribedBy} onkeydown={onGridKey}>
    {#each groups as g (g.category)}
      <span class="ap__cat">{g.category}</span>
      {#each g.items as p (p.id)}
        <button
          type="button"
          class="ap__tile"
          class:on={p.id === selected.id}
          aria-pressed={p.id === selected.id}
          tabindex={p.id === selected.id ? 0 : -1}
          onpointerenter={() => play(demos[p.id], p)}
          onpointerleave={stop}
          onfocus={() => play(demos[p.id], p)}
          onblur={stop}
          onclick={() => {
            choose(p);
            play(demos[p.id], p);
          }}
        >
          <span class="ap__stage" aria-hidden="true">
            <span class="ap__demo" class:ap__demo--text={layer === "text"} bind:this={demos[p.id]}
              >{layer === "text" ? SAMPLE_TEXT : ""}</span
            >
          </span>
          <span class="ap__label">{p.label}</span>
        </button>
      {/each}
    {/each}
  </div>

  {#if !nothing}
    <div class="ap__knobs">
      {#each selected.knobs as k (k)}
        {#if k === "speed" || k === "intensity" || k === "delay"}
          {@const r = KNOB_RANGES[k]}
          <label class="ap__knob">
            <span class="ap__kname">{k === "speed" ? "Speed" : k === "intensity" ? "Strength" : "Delay"}</span>
            <input
              type="range"
              min={r.min}
              max={r.max}
              step={r.step}
              value={num(k)}
              oninput={(e) => setKnob(k, Number(e.currentTarget.value))}
            />
            <span class="ap__kval"
              >{k === "speed" ? `${num(k).toFixed(2)}×` : k === "intensity" ? `${num(k)}%` : `${num(k).toFixed(2)} s`}</span
            >
          </label>
        {:else if k === "easing"}
          <label class="ap__knob">
            <span class="ap__kname">Easing</span>
            <select class="cv-select ap__select" value={easing} onchange={(e) => setKnob("easing", e.currentTarget.value)}>
              {#each EASING_NAMES as n (n)}
                <option value={n}>{EASINGS[n].label}</option>
              {/each}
            </select>
          </label>
        {:else if k === "direction" && selected.directions}
          <div class="ap__knob">
            <span class="ap__kname">Direction</span>
            <Segmented
              ariaLabel="{name} direction"
              options={selected.directions.map((d) => ({ label: DIRECTION_LABELS[d], value: d }))}
              value={direction}
              onChange={(v) => setKnob("direction", v)}
            />
          </div>
        {/if}
      {/each}
      {#if layer === "text"}
        <div class="ap__knob">
          <span class="ap__kname">Animate</span>
          <Segmented
            ariaLabel="{name} target"
            options={TARGET_OPTIONS}
            value={current.target === "vars" ? "vars" : "all"}
            onChange={(v) => setKnob("target", v)}
          />
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .ap {
    display: flex;
    flex-direction: column;
    gap: 8px;
    width: 100%;
    min-width: 0;
  }
  .ap__grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(72px, 1fr));
    gap: 4px;
  }
  .ap__cat {
    grid-column: 1 / -1;
    margin-top: 4px;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
  .ap__tile {
    display: flex;
    flex-direction: column;
    align-items: stretch;
    gap: 4px;
    height: auto;
    padding: 4px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    cursor: pointer;
  }
  .ap__tile:hover {
    color: var(--color-text);
    border-color: var(--color-muted);
  }
  .ap__tile:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .ap__tile.on {
    border-color: var(--color-accent);
    color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, var(--color-base));
  }
  /* Clips the demo: a slide or a drop travels a full box length, and it must not paint over
     the neighbouring tiles while it does. */
  .ap__stage {
    position: relative;
    height: 34px;
    overflow: hidden;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .ap__demo {
    width: 30px;
    height: 16px;
    background: var(--color-accent);
  }
  .ap__demo--text {
    width: auto;
    height: auto;
    background: none;
    color: var(--color-text);
    font-weight: 700;
    font-size: 15px;
  }
  .ap__demo--text :global(.fx-g) {
    display: inline-block;
    white-space: pre;
  }
  .ap__label {
    font-family: var(--font-ui);
    font-size: 10px;
    line-height: 1.2;
    text-align: center;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ap__knobs {
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding: 8px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  .ap__knob {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .ap__kname {
    flex: 0 0 64px;
    font-size: 11px;
    color: var(--color-dim);
  }
  .ap__knob input[type="range"] {
    flex: 1;
    min-width: 0;
  }
  .ap__kval {
    flex: 0 0 48px;
    text-align: right;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .ap__select {
    height: var(--control-height);
    max-width: none;
    flex: 1;
  }
</style>
