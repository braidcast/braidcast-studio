<script lang="ts">
  import { PLATFORM_COLORS, PLATFORM_LABELS } from "$lib/theme/platformColors";
  import type { ChatPlatform } from "$lib/api/bridge";

  // A row of platform selector chips shared by EventsDock (feed filter) and
  // MultichatDock (send destination). The two panels feed it DIFFERENT platform
  // lists on purpose -- Events lists platforms you have an account for, Multichat
  // lists enabled channels whose chat transport has ever reported (disabledOf marks
  // the ones that aren't currently sendable) -- so the source stays in each dock;
  // only the identical markup/CSS lives here.
  //
  // `value` is the selected chip: "all" or a platform. The "All" chip renders only
  // when showAll is true (meaningless with <2 platforms). onSelect reports clicks.
  //
  // dotColorOf/titleOf/disabledOf (G1) let a consumer overlay per-platform transport
  // health onto the dot + tooltip + clickability without forking the chip markup --
  // all optional, so MultichatDock can wire richer health state while EventsDock
  // (which doesn't pass them) keeps the original brand-color-only look untouched.
  interface Props {
    platforms: readonly ChatPlatform[];
    value: "all" | ChatPlatform;
    showAll?: boolean;
    onSelect: (v: "all" | ChatPlatform) => void;
    dotColorOf?: (p: ChatPlatform) => string;
    titleOf?: (p: ChatPlatform) => string | undefined;
    disabledOf?: (p: ChatPlatform) => boolean;
  }
  let { platforms, value, showAll = false, onSelect, dotColorOf, titleOf, disabledOf }: Props = $props();
</script>

{#if showAll}
  <button class="chip" class:on={value === "all"} onclick={() => onSelect("all")}>All</button>
{/if}
{#each platforms as p (p)}
  <button
    class="chip"
    class:on={value === p}
    style:--chip={PLATFORM_COLORS[p]}
    disabled={disabledOf?.(p) ?? false}
    title={titleOf?.(p)}
    onclick={() => onSelect(p)}
  >
    <span class="cdot" style:background={dotColorOf?.(p) ?? PLATFORM_COLORS[p]}></span>
    {PLATFORM_LABELS[p]}
  </button>
{/each}

<style>
  .chip {
    display: flex;
    align-items: center;
    gap: 4px;
    padding: 3px 8px;
    font-size: 10px;
    font-family: var(--font-ui);
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    cursor: pointer;
  }
  .chip.on {
    border-color: var(--chip, var(--color-accent));
    color: var(--chip, var(--color-accent));
    background: color-mix(in srgb, var(--chip, var(--color-accent)) 14%, transparent);
  }
  .chip:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }
  .cdot {
    width: 6px;
    height: 6px;
    flex: 0 0 auto;
  }
</style>
