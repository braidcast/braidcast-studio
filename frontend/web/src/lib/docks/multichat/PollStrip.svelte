<script lang="ts">
  import { tick } from "svelte";
  import type { LivePoll, LivePollOption } from "$lib/api/bridge";
  import { pollStore } from "$lib/stores/pollStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import { PLATFORM_COLORS } from "$lib/theme/platformColors";
  import Button from "$lib/ui/Button.svelte";
  import ChatOrigin from "$lib/ui/ChatOrigin.svelte";
  import IconButton from "$lib/ui/IconButton.svelte";
  import type { Attribution } from "$lib/ui/destinationSelection";

  // The polls opened in this dock's chats, running and finished, pinned above the feed.
  // It sits OUTSIDE the feed's scroll box: the feed is a virtualized list of absolutely
  // positioned rows over a sized spacer, and anything placed inside it would have to be a
  // measured row of its own.
  interface Props {
    polls: LivePoll[];
    /** The same destination attribution the feed rows use, so a poll and a chat line from
     * one chat name it identically. Polls exist only on YouTube. */
    originOf: (poll: LivePoll) => { origin: Attribution; title: string };
    /** Where focus goes when the last poll is dismissed and the strip empties. */
    fallbackFocus: () => void;
  }
  let { polls, originOf, fallbackFocus }: Props = $props();

  const PLATFORM = "youtube";

  let listEl = $state<HTMLUListElement | undefined>();
  let ending = $state<string[]>([]);

  // An action that removes the focused button (End becomes Dismiss once the poll closes;
  // Dismiss removes the whole row) must hand focus on, or a keyboard user is dropped on
  // <body>. The button only goes away when polls.changed lands, which can come before or
  // after the call's own reply, so the hand-off waits for both.
  interface Pending {
    id: string;
    index: number;
    kind: "end" | "dismiss";
    done: boolean;
    failed: boolean;
  }
  let pending = $state<Pending | null>(null);

  $effect(() => {
    const p = pending;
    if (!p || !p.done) {
      return;
    }
    const poll = polls.find((x) => x.id === p.id);
    // A refused call leaves its button where it was; a successful one is settled only
    // once the list shows it.
    if (!p.failed && poll && (p.kind === "dismiss" || poll.status === "active")) {
      return;
    }
    pending = null;
    void tick().then(() => restoreFocus(p.id, p.index));
  });

  function restoreFocus(id: string, index: number): void {
    const a = document.activeElement;
    if (a && a !== document.body && a.isConnected) {
      return;
    }
    const rows = Array.from(listEl?.querySelectorAll<HTMLElement>("[data-poll-row]") ?? []);
    const row = rows.find((r) => r.dataset.pollRow === id) ?? rows[Math.min(index, rows.length - 1)];
    const btn = row?.querySelector<HTMLElement>("button");
    if (btn) {
      btn.focus();
    } else {
      fallbackFocus();
    }
  }

  async function run(poll: LivePoll, index: number, kind: Pending["kind"]): Promise<void> {
    pending = { id: poll.id, index, kind, done: false, failed: false };
    let failed = false;
    try {
      if (kind === "end") {
        await pollStore.end(poll.id);
      } else {
        await pollStore.dismiss(poll.id);
      }
    } catch (e) {
      failed = true;
      // A failed end is recorded on the poll by the host and shown on its row, so only a
      // refused dismiss needs saying here.
      if (kind === "dismiss") {
        showToast("Couldn't dismiss this poll", (e as Error).message);
      }
    }
    if (pending?.id === poll.id) {
      pending = { ...pending, done: true, failed };
    }
  }

  async function end(poll: LivePoll, index: number): Promise<void> {
    if (ending.includes(poll.id)) {
      return;
    }
    ending = [...ending, poll.id];
    try {
      await run(poll, index, "end");
    } finally {
      ending = ending.filter((id) => id !== poll.id);
    }
  }

  /** Votes on the options that reported a count. Null when none did. */
  function totalOf(options: LivePollOption[]): number | null {
    let total: number | null = null;
    for (const o of options) {
      if (o.tally !== null) {
        total = (total ?? 0) + o.tally;
      }
    }
    return total;
  }

  const pct = (tally: number, total: number): number => (total > 0 ? Math.round((tally / total) * 100) : 0);
</script>

{#if polls.length > 0}
  <section class="polls" aria-label="Polls">
    <ul bind:this={listEl}>
      {#each polls as p, i (p.id)}
        {@const src = originOf(p)}
        {@const active = p.status === "active"}
        {@const busy = ending.includes(p.id)}
        {@const total = totalOf(p.options)}
        {@const top = Math.max(0, ...p.options.map((o) => o.tally ?? 0))}
        <li class="poll" data-poll-row={p.id} style:border-left-color={PLATFORM_COLORS[PLATFORM]}>
          <div class="phead">
            <ChatOrigin platform={PLATFORM} origin={src.origin} title={src.title} />
            <span class="pstate" class:live={active}>{active ? "Poll live" : "Poll ended"}</span>
            <span class="pacts">
              {#if active}
                <!-- aria-disabled, not disabled, while it is ending: a native disable drops
                     focus to <body> the instant it lands, so the result is never heard. -->
                <Button
                  size="xs"
                  tone="live"
                  aria-disabled={busy}
                  aria-busy={busy}
                  onclick={() => void end(p, i)}>{busy ? "Ending…" : "End poll"}</Button
                >
              {/if}
              {#if !active || p.error}
                <IconButton
                  icon="x"
                  size={22}
                  iconSize={11}
                  aria-label={`Dismiss poll: ${p.question}`}
                  title="Dismiss"
                  onclick={() => void run(p, i, "dismiss")}
                />
              {/if}
            </span>
          </div>
          <p class="pq selectable">{p.question}</p>
          {#if p.error}
            <p class="perr">{p.error}</p>
          {/if}
          {#if active}
            <ol class="popts">
              {#each p.options as o, j (j)}
                <li>{o.text}</li>
              {/each}
            </ol>
            <p class="pnote">Results appear when the poll ends.</p>
          {:else}
            <ol class="res">
              {#each p.options as o, j (j)}
                <li class:lead={o.tally !== null && o.tally > 0 && o.tally === top}>
                  <span class="rtext">{o.text}</span>
                  {#if o.tally === null || total === null}
                    <span class="rnum"
                      ><span aria-hidden="true">—</span><span class="sr-only">no count reported</span></span
                    >
                  {:else}
                    <span class="rnum"
                      >{o.tally}<span class="sr-only"> {o.tally === 1 ? "vote" : "votes"},</span>
                      · {pct(o.tally, total)}%</span
                    >
                    <span class="rbar" aria-hidden="true"
                      ><span class="rfill" style:width={pct(o.tally, total) + "%"}></span></span
                    >
                  {/if}
                </li>
              {/each}
            </ol>
          {/if}
        </li>
      {/each}
    </ul>
  </section>
{/if}

<style>
  /* Capped so a stack of finished polls cannot push the chat off the dock; it scrolls
     on its own past that. */
  .polls {
    flex: 0 1 auto;
    max-height: 45%;
    overflow-y: auto;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  /* Same stripe as a chat row from the same platform, so the poll reads as belonging
     to that chat. */
  .poll {
    padding: 5px 8px 7px 7px;
    border-left: 3px solid transparent;
    font-size: 12px;
    color: var(--color-text);
  }
  .poll + .poll {
    border-top: var(--border-weight) solid var(--color-border-2);
  }
  .phead {
    display: flex;
    align-items: center;
    gap: 6px;
    min-height: 24px;
  }
  .pstate {
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.09em;
    text-transform: var(--label-case);
    color: var(--color-muted);
  }
  .pstate.live {
    color: var(--color-accent);
  }
  .pacts {
    margin-left: auto;
    display: flex;
    align-items: center;
    gap: 4px;
  }
  /* aria-disabled, not disabled (see the call site), so `button:disabled` never matches;
     this restates its treatment so an ending button reads as unavailable. The hover
     selector is listed separately because Button's toned-hover rule outranks the plain
     one. */
  .pacts :global(button[aria-disabled="true"]),
  .pacts :global(button[aria-disabled="true"]:hover) {
    opacity: 0.5;
    cursor: default;
    background: none;
  }
  .pq {
    margin: 2px 0 0;
    font-weight: 600;
    line-height: 1.4;
    overflow-wrap: anywhere;
  }
  .perr {
    margin: 3px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.5;
    color: var(--color-warn);
    overflow-wrap: anywhere;
  }
  .popts {
    display: flex;
    flex-wrap: wrap;
    gap: 3px 5px;
    margin: 4px 0 0;
    padding: 0;
    list-style: none;
  }
  .popts li {
    padding: 1px 6px;
    border: var(--border-weight) solid var(--color-border);
    font-size: 11px;
    color: var(--color-dim);
    overflow-wrap: anywhere;
  }
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
