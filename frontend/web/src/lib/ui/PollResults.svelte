<script lang="ts">
  import type { LivePoll, LivePollOption } from "$lib/api/bridge";
  import { untrack } from "svelte";
  import Icon from "$lib/ui/Icon.svelte";

  // One poll's options, each row filled to its share of the votes, and the vote line under
  // them. Shared by the chat dock's poll strip (live and ended) and the results popup at the
  // end of a stream. `collapsible` shows the leading option alone at full size with the rest
  // as one compact line under it, which expands to every row -- the dock is narrow and chat
  // is what it is for.
  interface Props {
    poll: LivePoll;
    collapsible?: boolean;
  }
  let { poll, collapsible = false }: Props = $props();

  /** Votes on the options that reported a count. Null when none did. */
  function tallySum(options: LivePollOption[]): number | null {
    let total: number | null = null;
    for (const o of options) {
      if (o.tally !== null) {
        total = (total ?? 0) + o.tally;
      }
    }
    return total;
  }

  /** Each option's share of the votes (0..1): the platform's live ratio when it sent one,
   * else the option's count over the counted total. Null when neither is known. */
  function sharesOf(options: LivePollOption[]): (number | null)[] {
    const sum = tallySum(options);
    return options.map((o) => {
      if (o.ratio !== null) {
        return o.ratio;
      }
      return o.tally !== null && sum !== null && sum > 0 ? o.tally / sum : null;
    });
  }

  const pct = (share: number): number => Math.round(share * 100);

  const active = $derived(poll.status === "active");
  const shares = $derived(sharesOf(poll.options));
  const votes = $derived(poll.totalVotes ?? tallySum(poll.options));
  const top = $derived(Math.max(0, ...shares.map((s) => s ?? 0)));
  const isLead = (j: number): boolean => top > 0 && shares[j] === top;
  /** The row the collapsed view shows: the first leader, else the first option. */
  const leadIndex = $derived(Math.max(0, shares.findIndex((_, j) => isLead(j))));

  let expanded = $state(false);
  const showAll = $derived(!collapsible || expanded || poll.options.length < 2);

  // A count per option that goes up each time its share or tally rises; keying the number
  // on it restarts the gain flash. Zero until the first rise, so nothing flashes on mount.
  let gains = $state<number[]>([]);
  let seen: { share: number | null; tally: number | null }[] = [];
  $effect(() => {
    const prev = untrack(() => gains);
    const next = poll.options.map((o, j) => ({ share: shares[j], tally: o.tally }));
    const bumped = next.map((cur, j) => {
      const was = seen[j];
      const rose =
        was !== undefined &&
        ((cur.share ?? 0) > (was.share ?? 0) || (cur.tally ?? 0) > (was.tally ?? 0));
      return (prev[j] ?? 0) + (rose ? 1 : 0);
    });
    seen = next;
    if (bumped.some((g, j) => g !== (prev[j] ?? 0))) {
      gains = bumped;
    }
  });
</script>

{#snippet row(j: number)}
  {@const o = poll.options[j]}
  {@const share = shares[j]}
  <li class:lead={isLead(j)}>
    <span class="fill" aria-hidden="true" style:transform={`scaleX(${share ?? 0})`}></span>
    <span class="rtext">{o.text}</span>
    {#key gains[j] ?? 0}
      <span class="rnum" class:gain={(gains[j] ?? 0) > 0}>
        {#if share === null}
          <span aria-hidden="true">—</span><span class="sr-only">no result yet</span>
        {:else}
          {#if o.tally !== null}{o.tally}<span class="sr-only"> {o.tally === 1 ? "vote" : "votes"},</span>
            · {/if}{pct(share)}%
        {/if}
      </span>
    {/key}
  </li>
{/snippet}

{#if showAll}
  <ol class="res">
    {#each poll.options as _, j (j)}
      {@render row(j)}
    {/each}
  </ol>
  {#if collapsible && poll.options.length > 1}
    <button type="button" class="toggle less" aria-expanded="true" onclick={() => (expanded = false)}>
      <span>Show less</span>
      <span class="chev up" aria-hidden="true"><Icon name="caret-down" size={10} /></span>
    </button>
  {/if}
{:else}
  <ol class="res">
    {@render row(leadIndex)}
  </ol>
  <button type="button" class="toggle rest" aria-expanded="false" onclick={() => (expanded = true)}>
    <span class="sr-only">Show all {poll.options.length} options:</span>
    {#each poll.options as o, j (j)}
      {#if j !== leadIndex}
        {@const share = shares[j]}
        <span class="chip" class:lead={isLead(j)}>
          <span class="ctext">{o.text}</span>
          <span class="cnum">{share === null ? "—" : `${pct(share)}%`}</span>
        </span>
      {/if}
    {/each}
    <span class="chev" aria-hidden="true"><Icon name="caret-down" size={10} /></span>
  </button>
{/if}
{#if votes !== null}
  <p class="pnote">{votes} {votes === 1 ? "vote" : "votes"}{active ? " so far" : ""}</p>
{:else if active && shares.every((s) => s === null)}
  <p class="pnote">Waiting for the first results from YouTube…</p>
{/if}

<style>
  .pnote {
    margin: 4px 0 0;
    font-family: var(--font-mono);
    font-size: 9.5px;
    color: var(--color-muted);
  }
  .res {
    margin: 4px 0 0;
    padding: 0;
    list-style: none;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }
  /* The row is the bar: its background fills from the left to the option's share, the way
     YouTube draws a poll. The fill scales rather than resizes so a vote animates on the
     compositor instead of relaying out the text over it. */
  .res li {
    position: relative;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    column-gap: 8px;
    align-items: baseline;
    padding: 4px 6px;
    border: var(--border-weight) solid var(--color-border-2);
    isolation: isolate;
  }
  .fill {
    position: absolute;
    inset: 0;
    z-index: -1;
    transform-origin: left center;
    background: color-mix(in srgb, var(--color-muted) 18%, transparent);
    transition: transform 450ms cubic-bezier(0.22, 1, 0.36, 1);
  }
  .res li.lead {
    border-color: color-mix(in srgb, var(--color-accent) 55%, transparent);
  }
  .lead .fill {
    background: color-mix(in srgb, var(--color-accent) 26%, transparent);
  }
  .rtext {
    font-size: 11.5px;
    color: var(--color-dim);
    overflow-wrap: anywhere;
  }
  .rnum {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    white-space: nowrap;
  }
  .lead .rtext,
  .lead .rnum {
    color: var(--color-text);
  }
  .lead .rtext {
    font-weight: 600;
  }
  /* A vote landed on this option: its number briefly lifts and takes the accent. */
  .rnum.gain {
    display: inline-block;
    animation: gain 700ms ease-out;
  }
  @keyframes gain {
    0% {
      color: var(--color-accent);
      transform: translateY(-2px);
    }
    100% {
      transform: none;
    }
  }

  .toggle {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    min-width: 0;
    margin: 3px 0 0;
    padding: 3px 6px;
    background: none;
    border: none;
    color: var(--color-muted);
    font: inherit;
    font-size: 10.5px;
    text-align: left;
    cursor: pointer;
  }
  .toggle:hover {
    color: var(--color-text);
    background: color-mix(in srgb, var(--color-muted) 10%, transparent);
  }
  .toggle:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .less {
    justify-content: flex-end;
  }
  .chip {
    display: inline-flex;
    align-items: baseline;
    gap: 4px;
    min-width: 0;
    flex: 0 1 auto;
  }
  .chip + .chip::before {
    content: "·";
    margin-right: 2px;
    color: var(--color-border-2);
  }
  .ctext {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: var(--color-dim);
  }
  .cnum {
    font-family: var(--font-mono);
    font-size: 10px;
    white-space: nowrap;
  }
  .chip.lead .ctext,
  .chip.lead .cnum {
    color: var(--color-text);
  }
  .chev {
    display: inline-flex;
    margin-left: auto;
    flex: none;
  }
  .less .chev {
    margin-left: 0;
  }
  .chev.up {
    transform: rotate(180deg);
  }

  @media (prefers-reduced-motion: reduce) {
    .fill {
      transition: none;
    }
    .rnum.gain {
      animation: none;
    }
  }
</style>
