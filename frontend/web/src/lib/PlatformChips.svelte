<script lang="ts">
  import { PLATFORM_COLORS, PLATFORM_LABELS } from "./theme/platformColors";
  import type { ChatPlatform } from "./bridge";

  // A row of platform selector chips shared by EventsDock (feed filter) and
  // MultichatDock (send destination). The two panels feed it DIFFERENT platform
  // lists on purpose -- Events lists platforms you have an account for, Multichat
  // lists platforms whose chat transport is live (you can't send to a dead one) --
  // so the source stays in each dock; only the identical markup/CSS lives here.
  //
  // `value` is the selected chip: "all" or a platform. The "All" chip renders only
  // when showAll is true (meaningless with <2 platforms). onSelect reports clicks.
  interface Props {
    platforms: readonly ChatPlatform[];
    value: "all" | ChatPlatform;
    showAll?: boolean;
    onSelect: (v: "all" | ChatPlatform) => void;
  }
  let { platforms, value, showAll = false, onSelect }: Props = $props();
</script>

{#if showAll}
  <button class="chip" class:on={value === "all"} onclick={() => onSelect("all")}>All</button>
{/if}
{#each platforms as p (p)}
  <button
    class="chip"
    class:on={value === p}
    style:--chip={PLATFORM_COLORS[p]}
    onclick={() => onSelect(p)}
  >
    <span class="cdot" style:background={PLATFORM_COLORS[p]}></span>
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
  .cdot {
    width: 6px;
    height: 6px;
    flex: 0 0 auto;
  }
</style>
