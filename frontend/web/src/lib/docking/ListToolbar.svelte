<script lang="ts" module>
  import type { IconName } from "$lib/ui/Icon.svelte";

  export interface ToolAction {
    icon: IconName;
    title: string;
    disabled?: boolean;
    active?: boolean;
    onClick: () => void;
    size?: number;
  }
</script>

<script lang="ts">
  import type { Snippet } from "svelte";
  import Icon from "$lib/ui/Icon.svelte";

  // Shared bottom toolbar for the Studio list docks (Scenes/Sources, global and
  // per-canvas). `left` buttons sit at the start, `right` at the end; the `middle`
  // slot (typically a <FilterReveal/>) or an inert spacer fills the gap so `right`
  // always hugs the trailing edge. The .list-toolbar / .tool-btn primitives live in
  // app.css so every toolbar reskins together with the active theme.
  interface Props {
    left?: ToolAction[];
    right?: ToolAction[];
    middle?: Snippet;
  }
  let { left = [], right = [], middle }: Props = $props();
</script>

{#snippet btn(a: ToolAction)}
  <button class="tool-btn" class:active={a.active} title={a.title} aria-label={a.title} disabled={a.disabled} onclick={a.onClick}>
    <Icon name={a.icon} size={a.size ?? 13} />
  </button>
{/snippet}

<div class="list-toolbar">
  {#each left as a (a.title)}
    {@render btn(a)}
  {/each}

  {#if middle}
    {@render middle()}
  {:else}
    <div class="tool-spacer"></div>
  {/if}

  {#each right as a (a.title)}
    {@render btn(a)}
  {/each}
</div>
