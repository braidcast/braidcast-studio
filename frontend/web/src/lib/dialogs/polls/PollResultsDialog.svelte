<script lang="ts">
  import type { LivePoll } from "$lib/api/bridge";
  import Modal from "$lib/ui/Modal.svelte";
  import PollResults from "$lib/ui/PollResults.svelte";

  // The results of the polls a stream stop ended. The stop ends each running poll in the
  // background, so these are YouTube's final counts -- except where that call failed, when
  // the poll keeps the last live result it had and says so.
  interface Props {
    polls: LivePoll[];
    onClose: () => void;
  }
  let { polls, onClose }: Props = $props();
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
      <section class="poll" aria-label={`Results: ${p.question}`}>
        <h3 class="pq selectable">{p.question}</h3>
        <PollResults poll={p} />
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
</style>
