<script lang="ts">
  import type { LivePoll, LivePollOption } from "$lib/api/bridge";

  // One poll's options with their share bars and the vote line under them. Shared by the
  // chat dock's poll strip (live and ended) and the results popup at the end of a stream.
  interface Props {
    poll: LivePoll;
  }
  let { poll }: Props = $props();

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
</script>

<ol class="res">
  {#each poll.options as o, j (j)}
    {@const share = shares[j]}
    <li class:lead={share !== null && share > 0 && share === top}>
      <span class="rtext">{o.text}</span>
      {#if share === null}
        <span class="rnum"><span aria-hidden="true">—</span><span class="sr-only">no result yet</span></span>
      {:else}
        <span class="rnum"
          >{#if o.tally !== null}{o.tally}<span class="sr-only"> {o.tally === 1 ? "vote" : "votes"},</span>
            · {/if}{pct(share)}%</span
        >
        <span class="rbar" aria-hidden="true"><span class="rfill" style:width={pct(share) + "%"}></span></span>
      {/if}
    </li>
  {/each}
</ol>
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
    gap: 4px;
  }
  .res li {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    column-gap: 8px;
    row-gap: 2px;
    align-items: baseline;
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
  .rbar {
    grid-column: 1 / -1;
    display: block;
    height: 3px;
    background: var(--color-border-2);
  }
  .rfill {
    display: block;
    height: 100%;
    background: var(--color-muted);
  }
  .lead .rfill {
    background: var(--color-accent);
  }
</style>
