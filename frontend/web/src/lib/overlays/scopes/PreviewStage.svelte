<script lang="ts">
  // The scoped editor's centre: the real widget preview (PreviewPane, test buttons and all),
  // plus Replay and a backdrop switch. Replay fires the sample event that resolves to the
  // scope being edited (replaySample.ts), and an edit replays it on its own once the save has
  // landed and the reloaded frame is listening again, so a change is seen without a click.
  import { onDestroy, untrack } from "svelte";
  import type { OverlayWidget } from "$lib/api/bridge";
  import PreviewPane from "$lib/overlays/PreviewPane.svelte";
  import Button from "$lib/ui/Button.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Segmented from "$lib/ui/Segmented.svelte";
  import { callOrToast } from "$lib/utils/callToast";
  import type { AlertScopes } from "../../../overlay/alertScopes";
  import { STAGE_BACKGROUNDS, type PreviewBackground } from "./previewBackground";
  import { sampleFor } from "./replaySample";

  let {
    widget,
    scopes,
    scope,
    scopeName,
    reloadKey,
    savedKey,
  }: {
    widget: OverlayWidget;
    scopes: AlertScopes;
    scope: string;
    scopeName: string;
    reloadKey: number;
    /** Bumped by every save that landed; a change here is what arms the auto-replay. */
    savedKey: number;
  } = $props();

  /** How long after the reloaded frame is listening an edit replays. */
  const AUTO_REPLAY_MS = 400;

  let background = $state<PreviewBackground>("checker");
  let armed = false;
  let seenSaved = untrack(() => savedKey);
  let timer: ReturnType<typeof setTimeout> | undefined;

  function replay(): void {
    const sample = sampleFor(scopes, scope);
    if (sample) {
      void callOrToast("overlays.test", { id: widget.id, ...sample }, "Replay failed");
    }
  }

  // A landed save reloads the frame (reloadKey) and bumps savedKey together. Arm on the save,
  // fire on the ready that follows it -- firing at the save would reach a frame that is
  // still loading, and nothing would play. Only a save made while this stage is up arms it:
  // the counter is page-wide, and opening the editor must not replay an earlier save.
  $effect(() => {
    if (savedKey !== seenSaved) {
      seenSaved = savedKey;
      armed = true;
    }
  });

  function onReady(): void {
    if (!armed) {
      return;
    }
    armed = false;
    clearTimeout(timer);
    timer = setTimeout(replay, AUTO_REPLAY_MS);
  }

  onDestroy(() => clearTimeout(timer));
</script>

<div class="stage">
  <div class="stage__bar">
    <Button size="sm" variant="surface" onclick={replay} aria-label="Replay {scopeName}">
      <Icon name="replay" size={12} /> Replay
    </Button>
    <span class="stage__scope" title={scopeName}>{scopeName}</span>
    <span class="stage__spacer"></span>
    <Segmented
      ariaLabel="Preview background"
      options={STAGE_BACKGROUNDS}
      value={background}
      onChange={(v) => (background = v as PreviewBackground)}
    />
  </div>
  <div class="stage__pane">
    <PreviewPane
      url={widget.url}
      widgetId={widget.id}
      widgetType={widget.type}
      naturalW={widget.naturalW}
      naturalH={widget.naturalH}
      {reloadKey}
      {background}
      {onReady}
    />
  </div>
</div>

<style>
  .stage {
    display: flex;
    flex-direction: column;
    gap: 8px;
    height: 100%;
    min-height: 0;
    min-width: 0;
  }
  .stage__bar {
    flex: 0 0 auto;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .stage__scope {
    min-width: 0;
    font-size: 11px;
    color: var(--color-dim);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .stage__spacer {
    flex: 1;
  }
  .stage__pane {
    flex: 1;
    min-height: 0;
    display: flex;
  }
  .stage__pane > :global(.preview) {
    flex: 1;
    min-width: 0;
  }
</style>
