<script lang="ts">
  import { tick } from "svelte";
  import type { StreamInfoPreset } from "$lib/api/bridge";
  import { presetLabel } from "$lib/dialogs/streamInfoPresets/applyPreset";
  import CollectionDialog from "$lib/dialogs/CollectionDialog.svelte";
  import { nowTickStore } from "$lib/stores/nowTickStore.svelte";
  import { streamInfoPresetStore } from "$lib/stores/streamInfoPresetStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import { selectOnMount } from "$lib/utils/focusActions";
  import { fmtSince } from "$lib/utils/format";

  // Picks one saved stream-info sheet and hands it back. It owns the list, the rename
  // and the delete; what APPLYING one means belongs to the surface that opened it, since
  // the Go Live modal writes inherit layers and the schedule editor writes flat rows.
  interface Props {
    /** Worded by the caller: the same list is "load into this go-live" in one place and
     * "load into this entry" in the other. */
    title?: string;
    onPick: (preset: StreamInfoPreset) => void;
    onClose: () => void;
  }
  let { title = "Saved Stream Info", onPick, onClose }: Props = $props();

  streamInfoPresetStore.start();

  // Every row ages against one shared clock, so a picker left open does not keep claiming
  // "2m ago" ten minutes later. Released on destroy: the effect returns the store's own
  // unsubscribe, and the last one out stops the interval. No visibility check of its own --
  // a modal that is not on screen is not mounted, which is the case a dockview tab is not.
  $effect(() => nowTickStore.subscribe());
  const now = $derived(nowTickStore.nowMs);

  const presets = $derived(streamInfoPresetStore.presets);

  let listEl = $state<HTMLUListElement | undefined>();
  let active = $state(0);
  let renamingId = $state<string | null>(null);
  let renameValue = $state("");
  let pendingDelete = $state<StreamInfoPreset | null>(null);

  const listId = "preset-list";
  const optionId = (i: number): string => `preset-opt-${i}`;

  // Modal focuses its own first focusable (the close button); the list is what this
  // dialog is for, so it takes focus instead and arrow keys work without a click.
  $effect(() => {
    listEl?.focus();
  });

  // Clamped on READ, never written back. A delete resolves before the host's changed
  // event re-lists, so the stored index outlives the row it named for a moment; every
  // use going through the clamp is what stops that window from indexing past the end.
  // Clamping downward also lands on the row that slid up into the highlight, which is
  // the one the user is looking at.
  const activeIndex = $derived(Math.max(0, Math.min(active, presets.length - 1)));

  // Focus is on the input while a rename is open, so it has to be handed back or the
  // keyboard route out of the list dead-ends on the document body.
  async function endRename(): Promise<void> {
    renamingId = null;
    await tick();
    listEl?.focus();
  }

  async function pick(p: StreamInfoPreset): Promise<void> {
    // Ordered most-recently-used first, so using one restamps it. Fired before the
    // caller's own work and not awaited into it: a failed restamp costs an ordering,
    // never the apply the user asked for.
    void streamInfoPresetStore.touch(p.id).catch(() => {});
    onPick(p);
    onClose();
  }

  function startRename(p: StreamInfoPreset): void {
    renamingId = p.id;
    renameValue = p.name;
  }

  async function commitRename(): Promise<void> {
    const id = renamingId;
    if (id === null) {
      return;
    }
    const name = renameValue.trim();
    await endRename();
    try {
      // An empty name is a reset to the title fallback, not a refusal -- the host
      // documents it that way, so the box may legitimately be cleared.
      await streamInfoPresetStore.rename(id, name);
    } catch (e) {
      showToast("Couldn't rename this preset", (e as Error).message);
    }
  }

  async function confirmDelete(p: StreamInfoPreset): Promise<void> {
    try {
      await streamInfoPresetStore.remove(p.id);
    } catch (e) {
      showToast("Couldn't delete this preset", (e as Error).message);
    }
  }

  // Arrow keys move the highlight, Enter takes it, F2 renames and Delete removes the
  // highlighted row, so every action here is reachable without a pointer. Escape is left
  // to bubble: Modal owns it and closing the dialog is what it should do.
  function onListKeydown(e: KeyboardEvent): void {
    if (presets.length === 0) {
      return;
    }
    if (e.key === "ArrowDown") {
      active = (activeIndex + 1) % presets.length;
    } else if (e.key === "ArrowUp") {
      active = (activeIndex - 1 + presets.length) % presets.length;
    } else if (e.key === "Home") {
      active = 0;
    } else if (e.key === "End") {
      active = presets.length - 1;
    } else if (e.key === "Enter") {
      void pick(presets[activeIndex]);
    } else if (e.key === "F2") {
      startRename(presets[activeIndex]);
    } else if (e.key === "Delete") {
      pendingDelete = presets[activeIndex];
    } else {
      return;
    }
    e.preventDefault();
  }

  function onRenameKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      void commitRename();
    } else if (e.key === "Escape") {
      // Consumed here: the first Escape abandons the rename, and only a second one
      // closes the dialog behind it.
      void endRename();
      e.stopPropagation();
    } else {
      return;
    }
    e.preventDefault();
  }
</script>

<Modal {title} {onClose} width={460}>
  <div class="ph">
    <span class="ph__title">Presets</span>
    <span class="ph__count">{presets.length}</span>
  </div>

  {#if !streamInfoPresetStore.loaded}
    <p class="note">Loading presets…</p>
  {:else if streamInfoPresetStore.error}
    <p class="err">{streamInfoPresetStore.error}</p>
  {:else if presets.length === 0}
    <EmptyState
      compact
      title="No saved stream info yet"
      sub="Turn on Remember when you go live and this go-live's title, description and tags are saved here for next time."
    >
      {#snippet icon()}
        <Icon name="list" size={22} />
      {/snippet}
    </EmptyState>
  {:else}
    <ul
      class="plist"
      id={listId}
      role="listbox"
      tabindex="0"
      aria-label="Saved stream info presets"
      aria-activedescendant={renamingId === null ? optionId(activeIndex) : undefined}
      bind:this={listEl}
      onkeydown={onListKeydown}
    >
      {#each presets as p, i (p.id)}
        <!-- The <li> and the row carry no semantics of their own: role="option" has to
             read as a child of the listbox, and a generic listitem between the two breaks
             that relationship for a screen reader. The rename/delete buttons sit BESIDE
             the option rather than inside it -- an option's content is presentational, so
             a button within one would be unreachable; F2 and Delete on the highlighted
             row are the keyboard route to the same two actions. -->
        <li role="presentation">
          <div class="row" class:on={i === activeIndex} role="presentation">
            {#if renamingId === p.id}
              <input
                class="rename"
                use:selectOnMount
                bind:value={renameValue}
                spellcheck="false"
                aria-label="Preset name"
                placeholder="Empty name reads by title"
                onkeydown={onRenameKeydown}
                onblur={() => void commitRename()}
              />
            {:else}
              <button
                type="button"
                class="cv-ci opt"
                id={optionId(i)}
                role="option"
                tabindex="-1"
                aria-selected={i === activeIndex}
                onclick={() => void pick(p)}
                onmouseenter={() => (active = i)}
              >
                <span class="cv-ci__body">
                  <span class="cv-ci__name">{presetLabel(p)}</span>
                  <span class="cv-ci__sub">
                    Created {fmtSince(p.createdAtMs, now) || "unknown"} · last used
                    {fmtSince(p.lastUsedAtMs, now) || "never"}
                  </span>
                </span>
              </button>
              <span class="acts">
                <button
                  type="button"
                  class="act"
                  aria-label={`Rename ${presetLabel(p)}`}
                  title="Rename (F2)"
                  onclick={() => startRename(p)}
                >
                  <Icon name="edit" size={12} />
                </button>
                <button
                  type="button"
                  class="act danger"
                  aria-label={`Delete ${presetLabel(p)}`}
                  title="Delete (Del)"
                  onclick={() => (pendingDelete = p)}
                >
                  <Icon name="trash" size={12} />
                </button>
              </span>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
    <p class="note">
      Loading one fills the fields in as a starting point — nothing is sent until you go
      live, and everything stays editable.
    </p>
  {/if}

  {#snippet footer()}
    <button class="ghost" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

{#if pendingDelete}
  {@const doomed = pendingDelete}
  <CollectionDialog
    kind="confirm"
    title="Delete preset"
    message={`Delete "${presetLabel(doomed)}"? This removes the saved sheet only — nothing already applied to a channel changes.`}
    confirmLabel="Delete"
    onCommit={() => void confirmDelete(doomed)}
    onClose={() => (pendingDelete = null)}
  />
{/if}

<style>
  /* Head strip: same mono micro-label + count as the canvases list, which is the row
     vocabulary the list below reuses. */
  .ph {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding-bottom: 6px;
    margin-bottom: 6px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .ph__title {
    font-family: var(--font-mono);
    font-size: 9.5px;
    letter-spacing: 0.18em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .ph__count {
    font-family: var(--font-mono);
    font-size: 9px;
    color: var(--color-muted);
  }
  .plist {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 46vh;
    overflow-y: auto;
  }
  .plist:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 2px;
  }
  /* The row is the option plus its actions; the highlight lives here so the whole row
     lights up rather than just the part the pointer is over. */
  .row {
    display: flex;
    align-items: center;
    gap: 2px;
    border: var(--border-weight) solid transparent;
  }
  li + li .row {
    margin-top: 2px;
  }
  .row.on {
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
    border-color: var(--color-accent);
  }
  /* .cv-ci carries the shared row shape (padding, name/sub stack, badge); the border and
     the highlight move up to .row so the actions sit inside the same box. Qualified by
     .row so these win over the shared rules regardless of stylesheet order. */
  .row > .opt {
    flex: 1;
    min-width: 0;
    border: 0;
  }
  .row > .opt:hover {
    background: none;
  }
  .row.on .cv-ci__name {
    color: var(--color-text);
  }
  .acts {
    display: flex;
    flex: 0 0 auto;
    padding-right: 6px;
  }
  .act {
    width: 26px;
    height: 26px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    background: transparent;
    border: var(--border-weight) solid transparent;
    color: var(--color-muted);
    transition: color 0.12s ease;
  }
  .act:hover {
    color: var(--color-text);
    border-color: var(--color-border);
  }
  .act.danger:hover {
    color: var(--color-live);
    border-color: color-mix(in srgb, var(--color-live) 45%, transparent);
  }
  .act:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .rename {
    flex: 1;
    min-width: 0;
    margin: 2px 6px 2px 0;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 12.5px;
    padding: 7px 9px;
    outline: none;
  }
  .note {
    margin: 10px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-muted);
  }
  .err {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-live);
  }
  @media (prefers-reduced-motion: reduce) {
    .act {
      transition: none;
    }
  }
</style>
