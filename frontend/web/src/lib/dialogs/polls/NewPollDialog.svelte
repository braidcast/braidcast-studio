<script lang="ts" module>
  import type { Attribution } from "$lib/ui/destinationSelection";

  /** One chat a poll can be opened in. Keyed by the chat TRANSPORT, not the destination:
   * two destinations sharing one channel-wide chat are one chat and get one poll. */
  export interface PollTarget {
    key: string;
    accountId: string;
    /** "" addresses the account's channel-wide chat, as polls.create reads it. */
    profileUuid: string;
    name: string;
    origin: Attribution;
    originTitle: string;
  }

  // The host's bounds (Chat::kMinPollOptions / kMaxPollOptions, poll_registry.hpp); it
  // rejects anything outside them, so the form never offers a count it would refuse.
  const MIN_OPTIONS = 2;
  const MAX_OPTIONS = 4;
</script>

<script lang="ts">
  import { tick } from "svelte";
  import type { PollTemplate } from "$lib/api/bridge";
  import SavedItemPicker, { type SavedItemCopy } from "$lib/dialogs/SavedItemPicker.svelte";
  import { pollStore } from "$lib/stores/pollStore.svelte";
  import { pollTemplateStore } from "$lib/stores/pollTemplateStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import Button from "$lib/ui/Button.svelte";
  import ChatOrigin from "$lib/ui/ChatOrigin.svelte";
  import IconButton from "$lib/ui/IconButton.svelte";
  import Modal from "$lib/ui/Modal.svelte";

  interface Props {
    /** Every YouTube chat that is connected right now. Live: a stream that ends while the
     * form is open drops out of it. */
    targets: PollTarget[];
    onClose: () => void;
  }
  let { targets, onClose }: Props = $props();

  pollTemplateStore.start();

  let mounted = true;
  $effect(() => () => {
    mounted = false;
  });

  let question = $state("");
  let options = $state<string[]>(Array.from({ length: MIN_OPTIONS }, () => ""));
  // Held as the chats left OUT, so every chat is in by default and one that connects
  // while the form is open joins without the form having to notice.
  let excluded = $state<string[]>([]);
  let busy = $state(false);
  /** Per-chat failure of the last attempt, by target key. */
  let failures = $state<Record<string, string>>({});
  /** Chats a partial attempt did start on; they are excluded so a retry cannot double them. */
  let startedOn = $state<string[]>([]);
  let pickerOpen = $state(false);

  let questionEl = $state<HTMLInputElement | undefined>();
  let optionEls = $state<(HTMLInputElement | undefined)[]>([]);

  const chosen = $derived(targets.filter((t) => !excluded.includes(t.key)));
  // The checklist also stays while a live chat is left out: after a partial failure the
  // targets can shrink to one excluded chat, and without its checkbox nothing could bring
  // it back in.
  const multi = $derived(targets.length > 1 || targets.some((t) => excluded.includes(t.key)));
  const failedCount = $derived(Object.keys(failures).length);

  const blocker = $derived.by(() => {
    if (targets.length === 0) {
      return "No YouTube chat is connected any more.";
    }
    if (chosen.length === 0) {
      return "Pick at least one stream.";
    }
    if (question.trim() === "") {
      return "Add a question.";
    }
    if (options.some((o) => o.trim() === "")) {
      return "Every option needs text.";
    }
    // The host's rule: options must differ after trimming, or the tallies cannot be told apart.
    if (new Set(options.map((o) => o.trim())).size !== options.length) {
      return "Make each option different.";
    }
    return "";
  });
  const canStart = $derived(blocker === "" && !busy);

  // Modal focuses its own first focusable (the header close button); the question is what
  // this dialog is for, so it takes focus instead.
  $effect(() => {
    questionEl?.focus();
  });

  function toggle(key: string, on: boolean): void {
    excluded = on ? excluded.filter((k) => k !== key) : [...excluded, key];
  }

  async function addOption(): Promise<void> {
    if (options.length >= MAX_OPTIONS) {
      return;
    }
    options.push("");
    await tick();
    optionEls[options.length - 1]?.focus();
  }

  async function removeOption(i: number): Promise<void> {
    if (options.length <= MIN_OPTIONS) {
      return;
    }
    options.splice(i, 1);
    await tick();
    // The row that slid up into this slot, or the new last row when the last was removed.
    optionEls[Math.min(i, options.length - 1)]?.focus();
  }

  function fillFrom(t: PollTemplate): void {
    question = t.question;
    const next = t.options.slice(0, MAX_OPTIONS);
    while (next.length < MIN_OPTIONS) {
      next.push("");
    }
    options = next;
    failures = {};
  }

  async function start(): Promise<void> {
    if (!canStart) {
      return;
    }
    const batch = chosen;
    const params = { question: question.trim(), options: options.map((o) => o.trim()) };
    busy = true;
    failures = {};
    // One create per chat, in parallel: they are independent broadcasts, and one refusing
    // (a poll already running there) must not hold up or cancel the others.
    const results = await Promise.allSettled(
      batch.map((t) => pollStore.create({ accountId: t.accountId, profileUuid: t.profileUuid, ...params })),
    );
    busy = false;
    const failed: Record<string, string> = {};
    const ok: string[] = [];
    results.forEach((r, i) => {
      if (r.status === "fulfilled") {
        ok.push(batch[i].key);
      } else {
        const reason = r.reason as { userMessage?: string; message?: string } | undefined;
        failed[batch[i].key] = reason?.userMessage ?? reason?.message ?? String(r.reason);
      }
    });
    if (Object.keys(failed).length === 0) {
      if (mounted) {
        onClose();
      }
      return;
    }
    if (!mounted) {
      // Closed while the creates were in flight, so there is no form left to show them in.
      const lines = Object.entries(failed).map(([key, why]) => `${nameOf(key, batch)}: ${why}`);
      const count = lines.length;
      showToast(`Poll not started on ${count} ${count === 1 ? "stream" : "streams"}`, "Poll not started", {
        lines,
        assertive: true,
      });
      return;
    }
    excluded = [...excluded, ...ok];
    startedOn = [...startedOn, ...ok];
    failures = failed;
  }

  function onFieldKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" && !e.isComposing) {
      e.preventDefault();
      void start();
    }
  }

  const nameOf = (key: string, among: PollTarget[] = targets): string =>
    among.find((t) => t.key === key)?.name ?? "a stream";

  // How many of the last attempt failed, and where it did land. Per-stream reasons sit
  // beside their streams, so with several this only has to count.
  const outcome = $derived.by(() => {
    if (failedCount === 0) {
      return "";
    }
    if (!multi) {
      return Object.values(failures)[0];
    }
    const tried = failedCount + startedOn.length;
    let line = `Couldn't start on ${failedCount} of ${tried} ${tried === 1 ? "stream" : "streams"}.`;
    if (startedOn.length > 0) {
      const were = startedOn.length === 1 ? "is" : "are";
      line +=
        ` Started on ${startedOn.map((k) => nameOf(k)).join(", ")}, which ${were} now left out` +
        " so a retry doesn't open a second poll there.";
    }
    return line;
  });

  const uid = $props.id();
  const errId = (key: string): string => `${uid}-err-${key}`;

  const PICKER_COPY: SavedItemCopy = {
    noun: "saved poll",
    heading: "Saved polls",
    listLabel: "Saved polls",
    renamePlaceholder: "Empty name reads by question",
    emptyTitle: "No saved polls yet",
    emptySub: "Every poll you start is kept here, most recently used first, so you can run it again.",
    note:
      "Loading one fills the form in — nothing is posted until you start the poll, and everything stays editable.",
    deleteMessage: (label) => `Delete "${label}"? This removes the saved poll only — no running poll changes.`,
  };

  const templateLabel = (t: PollTemplate): string => t.name.trim() || t.question;
  const templateDetail = (t: PollTemplate): string =>
    (t.name.trim() ? t.question + " — " : "") + t.options.join(" · ");
</script>

<Modal
  title="New poll"
  {onClose}
  width={440}
  note={busy ? "" : blocker}
  actions={[{ label: "Saved polls…", onclick: () => (pickerOpen = true) }]}
  cancel={{ label: "Cancel", onclick: onClose }}
  confirm={{
    label: busy ? "Starting…" : chosen.length > 1 ? `Start on ${chosen.length} streams` : "Start poll",
    onclick: () => void start(),
    disabled: !canStart,
  }}
>
  <div class="cv-field">
    <label class="cv-field__l" for={`${uid}-q`}>Question</label>
    <input
      id={`${uid}-q`}
      class="wide"
      type="text"
      bind:this={questionEl}
      bind:value={question}
      spellcheck="true"
      autocomplete="off"
      placeholder="What should we play next?"
      onkeydown={onFieldKeydown}
    />
  </div>

  <div class="cv-field" role="group" aria-labelledby={`${uid}-opts`}>
    <div class="cv-field__l" id={`${uid}-opts`}>
      Options <span class="cv-field__sub">{options.length} of {MAX_OPTIONS}</span>
    </div>
    <ol class="opts">
      {#each options as _, i (i)}
        <li class="opt">
          <span class="optn" aria-hidden="true">{i + 1}</span>
          <input
            class="wide"
            type="text"
            bind:this={optionEls[i]}
            bind:value={options[i]}
            spellcheck="true"
            autocomplete="off"
            aria-label={`Option ${i + 1}`}
            onkeydown={onFieldKeydown}
          />
          <!-- aria-disabled, not disabled: CEF dispatches no mouse events to a disabled
               control, so the title explaining why it is off would never show. -->
          <IconButton
            icon="x"
            size={28}
            height="var(--control-height)"
            iconSize={12}
            variant="surface"
            aria-label={`Remove option ${i + 1}`}
            title={options.length <= MIN_OPTIONS ? `A poll needs at least ${MIN_OPTIONS} options` : "Remove option"}
            aria-disabled={options.length <= MIN_OPTIONS}
            onclick={() => void removeOption(i)}
          />
        </li>
      {/each}
    </ol>
    {#if options.length < MAX_OPTIONS}
      <div class="addrow">
        <Button size="sm" variant="dashed" onclick={() => void addOption()}>Add option</Button>
      </div>
    {/if}
  </div>

  {#if multi}
    <div class="cv-field" role="group" aria-labelledby={`${uid}-dests`}>
      <div class="cv-field__l" id={`${uid}-dests`}>
        Post in <span class="cv-field__sub">one poll per stream</span>
      </div>
      <ul class="dests">
        {#each targets as t (t.key)}
          {@const err = failures[t.key]}
          <li>
            <label class="dest">
              <input
                type="checkbox"
                checked={!excluded.includes(t.key)}
                onchange={(e) => toggle(t.key, e.currentTarget.checked)}
                aria-describedby={err ? errId(t.key) : undefined}
              />
              <ChatOrigin platform="youtube" origin={t.origin} title={t.originTitle} />
              <span class="dname">{t.name}</span>
              {#if startedOn.includes(t.key)}
                <span class="started">started</span>
              {/if}
            </label>
            {#if err}
              <p class="err" id={errId(t.key)}>{err}</p>
            {/if}
          </li>
        {/each}
      </ul>
    </div>
  {:else if targets.length === 1}
    {@const t = targets[0]}
    <div class="cv-field">
      <span class="cv-field__l">Post in</span>
      <span class="dest">
        <ChatOrigin platform="youtube" origin={t.origin} title={t.originTitle} />
        <span class="dname">{t.name}</span>
      </span>
    </div>
  {/if}

  <!-- Always mounted: a live region inserted together with its text is not reliably
       announced. -->
  <div role="alert">
    {#if outcome}<p class="err">{outcome}</p>{/if}
  </div>
</Modal>

{#if pickerOpen}
  <SavedItemPicker
    title="Saved Polls"
    items={pollTemplateStore.items}
    loaded={pollTemplateStore.loaded}
    error={pollTemplateStore.error}
    copy={PICKER_COPY}
    label={templateLabel}
    detail={templateDetail}
    touch={(id) => pollTemplateStore.touch(id)}
    remove={(id) => pollTemplateStore.remove(id)}
    rename={(id, name) => pollTemplateStore.rename(id, name)}
    onPick={fillFrom}
    onClose={() => (pickerOpen = false)}
  />
{/if}

<style>
  .wide {
    width: 100%;
  }
  .opts,
  .dests {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .opt {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .opt .wide {
    flex: 1;
  }
  .optn {
    flex: 0 0 auto;
    width: 1.4em;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    text-align: right;
  }
  .addrow {
    margin-top: 8px;
    padding-left: calc(1.4em + 6px);
  }
  .dest {
    display: flex;
    align-items: center;
    gap: 7px;
    min-width: 0;
    font-size: 12px;
    color: var(--color-text);
  }
  label.dest {
    cursor: pointer;
  }
  .dname {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .started {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.09em;
    text-transform: var(--label-case);
    color: var(--color-accent);
  }
  .err {
    margin: 4px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.5;
    color: var(--color-live);
    overflow-wrap: anywhere;
  }
  .dests .err {
    padding-left: 22px;
  }
</style>
