<script lang="ts">
  import type { LiveGoal } from "$lib/api/bridge";
  import { PLATFORM_COLORS } from "$lib/theme/platformColors";
  import ChatOrigin from "$lib/ui/ChatOrigin.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import type { Attribution } from "$lib/ui/destinationSelection";
  import { goalOver, goalProgress, goalTitle } from "./goalView";

  // The creator goals the dock's chats are showing, one compact row each, pinned under the
  // feed the way YouTube pins its goal under the chat. A sibling of the feed's scroll box
  // rather than a row inside it, so it never scrolls away and never covers the newest line.
  // Read-only: nothing here is interactive, so nothing takes focus.
  interface Props {
    goals: LiveGoal[];
    /** The same destination attribution the feed rows use. */
    originOf: (goal: LiveGoal) => { origin: Attribution; title: string };
    /** Name each goal's destination, as the feed does once it spans more than one. */
    showOrigin: boolean;
  }
  let { goals, originOf, showOrigin }: Props = $props();
</script>

{#if goals.length > 0}
  <ul class="goals" aria-label="Goals">
    {#each goals as g (g.id)}
      {@const progress = goalProgress(g)}
      {@const title = goalTitle(g)}
      {@const over = goalOver(g)}
      {@const status = over ? g.headline.trim() : ""}
      {@const spoken = [progress.spoken, status].filter(Boolean).join(", ")}
      {@const src = showOrigin ? originOf(g) : null}
      {@const where = src && src.origin.fidelity !== "none" ? ", " + src.origin.channel : ""}
      <li
        class="goal"
        class:over
        class:achieved={g.phase === "achieved"}
        style:border-left-color={PLATFORM_COLORS[g.platform]}
        title={[title, status || g.headline.trim(), progress.label].filter(Boolean).join("\n")}
      >
        <!-- A progressbar only when there is a total to measure against; a bare count reads as
             the text it is. The visible parts are hidden from it so it is announced once. -->
        <div
          class="body"
          role={progress.fraction !== null ? "progressbar" : undefined}
          aria-label={progress.fraction !== null ? "Goal: " + title + where : undefined}
          aria-valuemin={progress.fraction !== null ? 0 : undefined}
          aria-valuemax={progress.fraction !== null ? 100 : undefined}
          aria-valuenow={progress.fraction !== null ? Math.round(progress.fraction * 100) : undefined}
          aria-valuetext={progress.fraction !== null ? spoken : undefined}
        >
          <span class="icon" aria-hidden="true"><Icon name="target" size={13} /></span>
          {#if src}
            <ChatOrigin platform={g.platform} origin={src.origin} title={src.title} />
          {/if}
          <span class="desc" aria-hidden={progress.fraction !== null ? "true" : undefined}>
            {#if progress.fraction === null}<span class="sr-only">Goal: </span>{/if}{title}
          </span>
          {#if status}
            <span class="state" aria-hidden={progress.fraction !== null ? "true" : undefined}>{status}</span>
          {/if}
          {#if progress.label}
            <span class="count" aria-hidden={progress.fraction !== null ? "true" : undefined}>{progress.label}</span>
          {/if}
        </div>
        {#if progress.fraction !== null}
          <span class="track" aria-hidden="true">
            <span class="fill" style:transform={`scaleX(${progress.fraction})`}></span>
          </span>
        {/if}
      </li>
    {/each}
  </ul>
{/if}

<style>
  /* Capped so goals from several broadcasts cannot squeeze the chat; past that they scroll. */
  .goals {
    flex: 0 0 auto;
    max-height: 30%;
    overflow-y: auto;
    margin: 0;
    padding: 0;
    list-style: none;
    border-top: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  /* Same stripe as a chat row from the same platform, as PollStrip draws its polls. */
  .goal {
    position: relative;
    border-left: 3px solid transparent;
    font-size: 12px;
    color: var(--color-text);
  }
  .goal + .goal {
    border-top: var(--border-weight) solid var(--color-border-2);
  }
  .body {
    display: flex;
    align-items: center;
    gap: 6px;
    min-height: 30px;
    padding: 0 8px 0 7px;
    min-width: 0;
  }
  .icon {
    display: inline-flex;
    flex: none;
    color: var(--color-accent);
  }
  .over:not(.achieved) .icon {
    color: var(--color-muted);
  }
  .desc {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-weight: 600;
  }
  .state {
    flex: none;
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.09em;
    text-transform: var(--label-case);
    color: var(--color-dim);
    white-space: nowrap;
  }
  /* Text tokens, not the accent: --color-accent falls under 4.5:1 on the light mode and
     --color-muted under it on light, industrial and slate. --color-dim is the faintest
     neutral that clears 4.5:1 on every preset. */
  .achieved .state {
    color: var(--color-text);
  }
  .count {
    flex: none;
    font-family: var(--font-mono);
    font-size: 10.5px;
    color: var(--color-dim);
    white-space: nowrap;
  }
  /* A thin bar along the row's foot. The fill scales rather than resizes, as PollResults'
     does, so progress animates on the compositor. */
  .track {
    position: absolute;
    left: 0;
    right: 0;
    bottom: 0;
    height: 2px;
    background: var(--color-border-2);
    overflow: hidden;
  }
  .fill {
    position: absolute;
    inset: 0;
    transform-origin: left center;
    background: var(--color-dim);
    transition: transform 250ms cubic-bezier(0.22, 1, 0.36, 1);
  }
  /* Both fills clear 3:1 against the track and the row on every preset; the accent and
     --color-muted do not (amber on light is 1.7:1, muted on industrial's track 2.8:1). */
  .achieved .fill {
    background: var(--color-text);
  }
  @media (prefers-reduced-motion: reduce) {
    .fill {
      transition: none;
    }
  }
</style>
