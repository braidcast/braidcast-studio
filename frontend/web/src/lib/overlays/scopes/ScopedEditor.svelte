<script lang="ts">
  // The Simple editor for every stock widget (roadmap 9.4b): the live stage beside an inspector
  // of the widget's fields in collapsible groups, so the preview never scrolls away from the
  // setting being changed. A type that declares scopes (the alert box) also gets the scope
  // rail, and the inspector shows the selected scope's fields; any other type has one scope,
  // Defaults, and no rail. Every
  // edit is a new set of layers handed to onLayers, which the page saves like any other
  // edit; this component owns only which scope is selected and the undo window of a
  // deleted variation.
  import { onDestroy, untrack } from "svelte";
  import type { OverlayField, OverlayWidget } from "$lib/api/bridge";
  import FieldsPanel from "$lib/overlays/FieldsPanel.svelte";
  import { hideToast, showToast } from "$lib/stores/toastStore.svelte";
  import {
    DEFAULT_SCOPE,
    inheritedValue,
    normalizeScopes,
    scopeLabel,
    type AlertScopes,
  } from "../../../overlay/alertScopes";
  import PreviewStage from "./PreviewStage.svelte";
  import ScopeRail from "./ScopeRail.svelte";
  import {
    fieldInScope,
    layerOf,
    newVariation,
    withLayer,
    withoutVariation,
    withVariationAt,
    withVariationMeta,
    type ScopeLayers,
  } from "./scopeEdit";
  import VariationHeader from "./VariationHeader.svelte";

  let {
    widget,
    wide,
    reloadKey,
    savedKey,
    onLayers,
    onPrune,
  }: {
    widget: OverlayWidget;
    /** At or above the editor's wide breakpoint: rail, stage and inspector side by side. */
    wide: boolean;
    reloadKey: number;
    savedKey: number;
    onLayers: (next: ScopeLayers) => void;
    /** Remove `scope`'s uploads that nothing references any more, once saved. */
    onPrune: (widgetId: string, scope: string) => void;
  } = $props();

  // The page mounts one editor per widget ({#key widget.id}), so the id is fixed for this
  // instance -- and has to be read now: a prune that runs at teardown would otherwise read
  // the prop after it already names the widget switched to.
  const widgetId = untrack(() => widget.id);

  /** How long a deleted variation can be brought back. Its uploads go when this runs out. */
  const UNDO_MS = 8000;
  const TOAST_KIND = "overlay-variation-delete";

  let selected = $state<string>(DEFAULT_SCOPE);

  const events = $derived(widget.scopes?.events ?? []);
  const layers = $derived<ScopeLayers>({
    settings: widget.settings,
    overrides: widget.overrides ?? {},
    variations: widget.variations ?? [],
  });
  const scopes = $derived<AlertScopes>(normalizeScopes({ ...layers, events }));
  /** Defaults as the resolver sees them: every schema key at its override or its default. */
  const defaults = $derived(
    Object.fromEntries(
      widget.schema.map((f) => [f.key, Object.hasOwn(widget.settings, f.key) ? widget.settings[f.key] : f.default]),
    ),
  );
  const selectedVariation = $derived(scopes.variations.find((v) => v.id === selected) ?? null);
  const selectedEvent = $derived(
    events.find((e) => e.key === (selectedVariation ? selectedVariation.event : selected)) ?? null,
  );
  /** Whether this type declares scopes: the rail, Replay and per-scope wording exist only then. */
  const hasScopes = $derived(events.length > 0);
  const scopeName = $derived(hasScopes ? scopeLabel(scopes, selected) : "Settings");

  // A scope that stopped existing (a variation deleted here or by an external edit) falls
  // back to its event, or to Defaults.
  $effect(() => {
    if (selected === DEFAULT_SCOPE || events.some((e) => e.key === selected)) {
      return;
    }
    if (!scopes.variations.some((v) => v.id === selected)) {
      selected = DEFAULT_SCOPE;
    }
  });

  function inherit(f: OverlayField): { value: unknown; fromLabel: string } {
    const { value, from } = inheritedValue(scopes, defaults, selected, f.key);
    return { value: value === undefined ? f.default : value, fromLabel: scopeLabel(scopes, from) };
  }

  function onFields(next: Record<string, unknown>): void {
    onLayers(withLayer(layers, selected, events, next));
  }

  function addVariation(eventKey: string): void {
    const event = events.find((e) => e.key === eventKey);
    const v = event ? newVariation(event, layers.variations.map((x) => x.id)) : null;
    if (v) {
      onLayers({ ...layers, variations: [...layers.variations, v] });
      selected = v.id;
    }
  }

  // Each deletion keeps its own timer, so deleting a second variation inside the first's
  // window still removes the first one's uploads when its time comes.
  const pendingPrunes = new Map<string, ReturnType<typeof setTimeout>>();

  function deleteVariation(id: string): void {
    const { layers: next, removed, index } = withoutVariation(layers, id);
    if (!removed) {
      return;
    }
    const name = scopeLabel(scopes, id);
    if (selected === id) {
      selected = removed.event;
    }
    onLayers(next);
    pendingPrunes.set(
      id,
      setTimeout(() => {
        pendingPrunes.delete(id);
        onPrune(widgetId, id);
      }, UNDO_MS),
    );
    showToast(`Deleted variation "${name}"`, name, {
      kind: TOAST_KIND,
      durationMs: UNDO_MS,
      dismissible: true,
      announce: `Deleted variation ${name}. Undo is available for a few seconds.`,
      action: {
        label: "Undo",
        onAction: () => {
          clearTimeout(pendingPrunes.get(id));
          pendingPrunes.delete(id);
          onLayers(withVariationAt(layers, removed, index));
          selected = id;
          hideToast(TOAST_KIND);
        },
      },
    });
  }

  // Leaving the editor ends every undo window early: the variations stay deleted, so their
  // uploads go now rather than never, and the toast goes with them -- its Undo would put a
  // variation back into whichever widget is open by then.
  onDestroy(() => {
    hideToast(TOAST_KIND);
    for (const [id, t] of pendingPrunes) {
      clearTimeout(t);
      onPrune(widgetId, id);
    }
    pendingPrunes.clear();
  });
</script>

<div class="se" class:se--wide={wide} class:se--norail={!hasScopes}>
  {#if hasScopes}
    <div class="se__rail">
      <ScopeRail
        {scopes}
        {selected}
        variant={wide ? "rail" : "select"}
        onSelect={(s) => (selected = s)}
        onAdd={addVariation}
        onDelete={deleteVariation}
      />
    </div>
  {/if}

  <div class="se__stage">
    <PreviewStage {widget} {scopes} scope={selected} {scopeName} {reloadKey} {savedKey} />
  </div>

  <div class="se__inspector">
    <h2 class="se__title">{scopeName}</h2>
    {#if selectedVariation && selectedEvent}
      <VariationHeader
        variation={selectedVariation}
        event={selectedEvent}
        onMeta={(patch) => onLayers(withVariationMeta(layers, selectedVariation.id, patch))}
        onDelete={() => deleteVariation(selectedVariation.id)}
      />
    {:else if selectedEvent}
      <p class="se__help">
        Changes here apply to every {selectedEvent.label.toLowerCase()} alert. Anything you leave alone follows Defaults.
      </p>
    {:else if hasScopes}
      <p class="se__help">The look every alert starts from. Events and variations change only what they set.</p>
    {/if}
    <FieldsPanel
      schema={widget.schema}
      settings={layerOf(layers, selected, events)}
      widgetId={widget.id}
      scope={selected}
      inherit={selected === DEFAULT_SCOPE ? undefined : inherit}
      visible={(f) => fieldInScope(f, selected)}
      assets={widget.assets}
      widgetUrl={widget.url}
      onChange={onFields}
      onAssetReleased={() => onPrune(widgetId, selected)}
    />
  </div>
</div>

<style>
  .se {
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    flex-direction: column;
    gap: 12px;
    overflow-y: auto;
  }
  .se__stage {
    flex: 0 0 300px;
    min-height: 0;
    display: flex;
  }
  .se__inspector {
    display: flex;
    flex-direction: column;
    gap: 10px;
    min-width: 0;
  }
  /* Wide: three columns, each scrolling on its own so the stage never scrolls away from
     the field being edited. */
  .se--wide {
    display: grid;
    grid-template-columns: 200px minmax(280px, 1fr) minmax(360px, 440px);
    overflow: hidden;
  }
  .se--wide.se--norail {
    grid-template-columns: minmax(280px, 1fr) minmax(360px, 440px);
  }
  .se--wide .se__rail,
  .se--wide .se__stage,
  .se--wide .se__inspector {
    min-height: 0;
  }
  .se--wide .se__inspector {
    overflow-y: auto;
    padding-right: 4px;
  }
  .se__title {
    margin: 0;
    font-size: 14px;
    font-weight: 600;
    color: var(--color-text);
  }
  .se__help {
    margin: 0;
    font-size: 11.5px;
    line-height: 1.45;
    color: var(--color-dim);
  }
</style>
