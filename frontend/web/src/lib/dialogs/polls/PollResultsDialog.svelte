<script lang="ts">
  import type { LivePoll } from "$lib/api/bridge";
  import { onMount, untrack } from "svelte";
  import { prefersReducedMotion } from "svelte/motion";
  import Modal from "$lib/ui/Modal.svelte";
  import PollResults from "$lib/ui/PollResults.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import { fmtCount } from "$lib/utils/format";
  import { sharesOf, pollLeaders, pct, hasAnyResult } from "$lib/utils/pollShares";

  // The results of the polls a stream stop ended. The stop ends each running poll in the
  // background, so these are YouTube's final counts -- except where that call failed, when
  // the poll keeps the last live result it had and says so.
  interface Props {
    polls: LivePoll[];
    onClose: () => void;
  }
  let { polls, onClose }: Props = $props();

  interface Hero {
    tie: boolean;
    indices: number[];
    pct: number;
    options: { text: string; votes: number | null }[];
  }

  interface PollState {
    hero: Hero | null;
    /** True when the poll has no winner yet cannot be called voteless: nothing ever
     * reported a result (see `hasAnyResult`), or a vote total arrived without per-option
     * shares. It gets the plain breakdown; only a known, real zero gets the "No votes
     * were cast" line. */
    noResults: boolean;
  }

  /** The winner (or tied winners) of one poll, for its results-dialog hero, plus,
   * when there is no hero, whether that is a known zero-vote result (see `noResults`).
   * Built on `pollLeaders`, the same leader math the live rows and the dock's collapsed
   * row use, so the hero can never disagree with the breakdown below it about who won. */
  function stateFor(p: LivePoll): PollState {
    const shares = sharesOf(p.options);
    const leaders = pollLeaders(shares);
    if (leaders.share === null) {
      return { hero: null, noResults: !hasAnyResult(p.options, p.totalVotes) || (p.totalVotes ?? 0) > 0 };
    }
    return {
      hero: {
        tie: leaders.indices.length > 1,
        indices: leaders.indices,
        pct: pct(leaders.share),
        options: leaders.indices.map((j) => ({ text: p.options[j].text, votes: p.options[j].tally })),
      },
      noResults: false,
    };
  }

  // Reduced motion is read once, at open: the celebration is one-shot for this dialog's
  // whole lifetime, so nothing here needs to react to the OS setting changing mid-dialog.
  // The app has no reduced-motion setting of its own; like every CSS gate in the app,
  // this follows the OS preference -- `prefersReducedMotion` is Svelte's own reactive
  // wrapper around that media query, and `.current` is read once here (not kept
  // reactive) so a later OS toggle can't restart or cancel an in-progress celebration.
  const reduceMotion = prefersReducedMotion.current;

  // One confetti burst per dialog open, from the first poll that has a winner -- not one
  // per poll, which on a multi-poll stop would turn "celebratory" into a fireworks show.
  // `untrack`: this reads `polls` once, at open, by design (see reduceMotion above); it
  // must not become a reactive computation that could re-pick a hero on a later render.
  const firstHeroId = untrack(() => polls.find((p) => stateFor(p).hero !== null)?.id ?? null);

  interface ConfettiPiece {
    id: number;
    color: string;
    left: string;
    top: string;
    size: string;
    delay: string;
    dx: string;
    dy: string;
    rot: string;
  }
  const CONFETTI_COLORS = ["var(--color-accent)", "var(--meter-green)", "var(--meter-yellow)", "var(--color-live)"];

  function makeConfetti(count: number): ConfettiPiece[] {
    const out: ConfettiPiece[] = [];
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2;
      const dist = 46 + Math.random() * 64;
      out.push({
        id: i,
        color: CONFETTI_COLORS[i % CONFETTI_COLORS.length],
        left: `${38 + Math.random() * 24}%`,
        top: `${28 + Math.random() * 24}%`,
        size: `${3 + Math.round(Math.random() * 3)}px`,
        delay: `${Math.round(Math.random() * 140)}ms`,
        dx: `${Math.cos(angle) * dist}px`,
        dy: `${Math.sin(angle) * dist - 18}px`,
        rot: `${Math.round(Math.random() * 540 - 270)}deg`,
      });
    }
    return out;
  }
  const confetti = reduceMotion || firstHeroId === null ? [] : makeConfetti(36);
  let showConfetti = $state(confetti.length > 0);
  onMount(() => {
    if (confetti.length === 0) {
      return;
    }
    const t = setTimeout(() => (showConfetti = false), 1300);
    return () => clearTimeout(t);
  });

  // Ticks an aria-hidden node's text from 0 to `to` on an ease-out curve. The real,
  // final value lives in a separate visually-hidden node instead, so a screen reader is
  // never read a stream of intermediate numbers -- see the sr-only span at each call site.
  // `update` handles a poll's % changing while the dialog stays open (pollStore.results
  // can be reassigned by a later polls.results event without remounting the dialog): it
  // always tweens (or jumps, under reduced motion) from the currently displayed value,
  // never restarting from 0.
  function countUp(node: HTMLElement, to: number): { update(to: number): void; destroy(): void } {
    let raf = 0;
    let current = 0;
    let target = to;

    function cancel(): void {
      if (raf) {
        cancelAnimationFrame(raf);
        raf = 0;
      }
    }

    function animateTo(next: number, animate: boolean): void {
      cancel();
      if (!animate) {
        current = next;
        node.textContent = String(next);
        return;
      }
      const duration = 600;
      const start = performance.now();
      const from = current;
      raf = requestAnimationFrame(function tick(now: number) {
        const t = Math.min(1, (now - start) / duration);
        const eased = 1 - Math.pow(1 - t, 3);
        current = Math.round(from + (next - from) * eased);
        node.textContent = String(current);
        if (t < 1) {
          raf = requestAnimationFrame(tick);
        }
      });
    }

    animateTo(target, !reduceMotion);

    return {
      update(next: number) {
        if (next === target) {
          return;
        }
        target = next;
        animateTo(next, !reduceMotion);
      },
      destroy: cancel,
    };
  }
</script>

<Modal
  title={polls.length === 1 ? "Poll results" : `Poll results (${polls.length})`}
  {onClose}
  width={420}
  closeOnBackdrop
  confirm={{ label: "Done", onclick: onClose }}
>
  <div class="polls">
    {#each polls as p (p.id)}
      {@const state = stateFor(p)}
      {@const hero = state.hero}
      <section class="poll" aria-label={`Results: ${p.question}`}>
        <h3 class="pq selectable">{p.question}</h3>
        {#if hero}
          {@const digitWidth = `${String(hero.pct).length}ch`}
          <div class="hero">
            <span class="trophy" aria-hidden="true"><Icon name="trophy" size={18} /></span>
            <p class="eyebrow">{hero.tie ? "It's a tie" : "Winner"}</p>
            {#if hero.tie}
              <ul class="tie-list">
                {#each hero.options as o, idx (idx)}
                  <li>
                    <span class="tie-text">{o.text}</span>
                    <span class="tie-pct">
                      <span class="pctdigit" style:min-width={digitWidth} aria-hidden="true" use:countUp={hero.pct}
                        >0</span
                      ><span aria-hidden="true">%</span><span class="sr-only">{hero.pct}%</span>
                    </span>
                  </li>
                {/each}
              </ul>
            {:else}
              {@const winner = hero.options[0]}
              <p class="wtext">{winner.text}</p>
              <p class="wpct">
                <span
                  class="pctnum pctdigit"
                  style:min-width={digitWidth}
                  aria-hidden="true"
                  use:countUp={hero.pct}>0</span
                ><span class="pctnum" aria-hidden="true">%</span>
                <span class="sr-only">{hero.pct}%</span>
                {#if winner.votes !== null}
                  <span class="wvotes">{fmtCount(winner.votes)} {winner.votes === 1 ? "vote" : "votes"}</span>
                {/if}
              </p>
            {/if}
            {#if showConfetti && p.id === firstHeroId}
              <div class="confetti" aria-hidden="true">
                {#each confetti as c (c.id)}
                  <span
                    class="confetti-piece"
                    style:left={c.left}
                    style:top={c.top}
                    style:width={c.size}
                    style:height={c.size}
                    style:background={c.color}
                    style:animation-delay={c.delay}
                    style:--dx={c.dx}
                    style:--dy={c.dy}
                    style:--rot={c.rot}
                  ></span>
                {/each}
              </div>
            {/if}
          </div>
        {/if}
        {#if hero}
          <PollResults poll={p} excludeIndices={hero.indices} />
        {:else if state.noResults}
          <PollResults poll={p} />
        {:else}
          <PollResults poll={p} emptyNote="No votes were cast" />
        {/if}
        {#if p.error}
          <p class="pwarn">YouTube did not confirm the final count, so these are the last live results.</p>
        {/if}
      </section>
    {/each}
  </div>
</Modal>

<style>
  .polls {
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .poll + .poll {
    padding-top: 12px;
    border-top: var(--border-weight) solid var(--color-border-2);
  }
  .pq {
    margin: 0;
    font-size: 13px;
    font-weight: 600;
    line-height: 1.4;
    color: var(--color-text);
    overflow-wrap: anywhere;
  }
  .pwarn {
    margin: 6px 0 0;
    font-size: 11px;
    line-height: 1.5;
    color: var(--color-warn);
  }
  /* Winner hero. Every ink in it is --color-text: the card's own accent tint is what
     reads as celebratory, and keeping the text on the app's one high-contrast token
     (rather than accent-colored digits) is what clears 4.5:1 in every shipped theme --
     Graphite's blue accent measures under 4.5:1 against its own dark surface at any
     tint, so accent stays confined to the icon, the border and the background wash. */
  .hero {
    position: relative;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 3px;
    margin: 8px 0 10px;
    padding: 14px 12px 12px;
    text-align: center;
    overflow: hidden;
    background: color-mix(in srgb, var(--color-accent) 12%, var(--color-surface-2));
    border: var(--border-weight) solid color-mix(in srgb, var(--color-accent) 55%, transparent);
  }
  .trophy {
    display: flex;
    color: var(--color-accent);
  }
  @media (prefers-reduced-motion: no-preference) {
    .hero {
      animation: hero-in 280ms cubic-bezier(0.16, 1, 0.3, 1) both;
    }
  }
  @keyframes hero-in {
    from {
      opacity: 0;
      transform: scale(0.96);
    }
    to {
      opacity: 1;
      transform: scale(1);
    }
  }
  .eyebrow {
    margin: 1px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    font-weight: 600;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
  }
  .wtext {
    margin: 2px 0 0;
    max-width: 100%;
    font-size: 15px;
    font-weight: 700;
    line-height: 1.3;
    color: var(--color-text);
    overflow-wrap: anywhere;
  }
  .wpct {
    margin: 2px 0 0;
    display: flex;
    align-items: baseline;
    justify-content: center;
    flex-wrap: wrap;
    gap: 6px;
  }
  .pctnum {
    font-size: 30px;
    font-weight: 800;
    line-height: 1;
    color: var(--color-text);
    font-variant-numeric: tabular-nums;
  }
  .wvotes {
    font-family: var(--font-mono);
    font-size: 10.5px;
    color: var(--color-text);
  }
  /* Reserves the counting digits' final width (set inline, per instance, from the
     winning %'s own digit count) so the count-up settling on a wider number -- 9 -> 10,
     99 -> 100 -- can't shift the centered .wpct row, or the tie rows' right edge,
     sideways as it animates. Right-aligned so the added digit appears on the left,
     where a ticking number naturally grows, instead of nudging the "%" sign along. */
  .pctdigit {
    display: inline-block;
    text-align: right;
  }
  .tie-list {
    margin: 4px 0 0;
    padding: 0;
    width: 100%;
    list-style: none;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .tie-list li {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 10px;
    font-size: 12.5px;
    font-weight: 600;
    color: var(--color-text);
  }
  .tie-text {
    min-width: 0;
    text-align: left;
    overflow-wrap: anywhere;
  }
  .tie-pct {
    flex: none;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }

  /* Confetti: a fixed set of absolutely-positioned pieces, generated once on open and
     dropped from the DOM (`showConfetti`) once the burst finishes -- never regenerated
     by a re-render, and never built at all under reduced motion. */
  .confetti {
    position: absolute;
    inset: 0;
    pointer-events: none;
  }
  .confetti-piece {
    position: absolute;
    opacity: 0;
  }
  @media (prefers-reduced-motion: no-preference) {
    .confetti-piece {
      animation: confetti-burst 1.1s ease-out forwards;
    }
  }
  @keyframes confetti-burst {
    0% {
      transform: translate(0, 0) rotate(0deg);
      opacity: 1;
    }
    100% {
      transform: translate(var(--dx), var(--dy)) rotate(var(--rot));
      opacity: 0;
    }
  }
</style>
