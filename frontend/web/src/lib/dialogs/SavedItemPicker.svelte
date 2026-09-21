<script lang="ts" module>
  /** What a saved-item row needs to be listed, aged, renamed and deleted. */
  export interface SavedItem {
    id: string;
    name: string;
    createdAtMs: number;
    lastUsedAtMs: number;
  }

  /** The words that differ between lists. `noun` is lower case ("preset"); it builds the
   * delete title, the rename field's name and the failure toasts. */
  export interface SavedItemCopy {
    noun: string;
    heading: string;
    listLabel: string;
    renamePlaceholder: string;
    emptyTitle: string;
    emptySub: string;
    note: string;
    deleteMessage: (label: string) => string;
  }
</script>

<script lang="ts" generics="T extends SavedItem">
  import { tick } from "svelte";
  import CollectionDialog from "$lib/dialogs/CollectionDialog.svelte";
  import { nowTickStore } from "$lib/stores/nowTickStore.svelte";
  import { showToast } from "$lib/stores/toastStore.svelte";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import IconButton from "$lib/ui/IconButton.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import { selectOnMount } from "$lib/utils/focusActions";
  import { fmtSince } from "$lib/utils/format";

  // Picks one saved, most-recently-used-first item and hands it back. It owns the list,
  // the rename and the delete; what APPLYING one means belongs to the surface that opened
  // it. The list and its mutations come in as props, so the same picker serves any host
  // store with the touch/remove/rename shape (lib/stores/mruListStore.svelte.ts).
  interface Props {
    title: string;
    items: T[];
    loaded: boolean;
    error: string | null;
    copy: SavedItemCopy;
    label: (item: T) => string;
    /** Optional second line naming what the item holds, above its age. */
    detail?: (item: T) => string;
    touch: (id: string) => Promise<void>;
    remove: (id: string) => Promise<void>;
    rename: (id: string, name: string) => Promise<void>;
    onPick: (item: T) => void;
    onClose: () => void;
  }
  let { title, items, loaded, error, copy, label, detail, touch, remove, rename, onPick, onClose }: Props =
    $props();

  // Every row ages against one shared clock, so a picker left open does not keep claiming
  // "2m ago" ten minutes later. Released on destroy: the effect returns the store's own
  // unsubscribe, and the last one out stops the interval. No visibility check of its own --
  // a modal that is not on screen is not mounted, which is the case a dockview tab is not.
  $effect(() => nowTickStore.subscribe());
  const now = $derived(nowTickStore.nowMs);

  const Noun = $derived(copy.noun.charAt(0).toUpperCase() + copy.noun.slice(1));

  let listEl = $state<HTMLUListElement | undefined>();
  let active = $state(0);
  let renamingId = $state<string | null>(null);
  let renameValue = $state("");
  let pendingDelete = $state<T | null>(null);

  const uid = $props.id();
  const listId = `${uid}-list`;
  const optionId = (i: number): string => `${uid}-opt-${i}`;

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
  const activeIndex = $derived(Math.max(0, Math.min(active, items.length - 1)));

  // Focus is on the input while a rename is open, so it has to be handed back or the
  // keyboard route out of the list dead-ends on the document body.
  async function endRename(): Promise<void> {
    renamingId = null;
    await tick();
    listEl?.focus();
  }

  async function pick(item: T): Promise<void> {
    // Ordered most-recently-used first, so using one restamps it. Fired before the
    // caller's own work and not awaited into it: a failed restamp costs an ordering,
    // never the apply the user asked for.
    void touch(item.id).catch(() => {});
    onPick(item);
    onClose();
  }

  function startRename(item: T): void {
    renamingId = item.id;
    renameValue = item.name;
  }

  async function commitRename(): Promise<void> {
    const id = renamingId;
    if (id === null) {
      return;
    }
    const name = renameValue.trim();
    await endRename();
    try {
      // An empty name is a reset to the content fallback, not a refusal -- the host
      // documents it that way, so the box may legitimately be cleared.
      await rename(id, name);
    } catch (e) {
      showToast(`Couldn't rename this ${copy.noun}`, (e as Error).message);
    }
  }

  async function confirmDelete(item: T): Promise<void> {
    try {
      await remove(item.id);
    } catch (e) {
      showToast(`Couldn't delete this ${copy.noun}`, (e as Error).message);
    }
  }

  // Arrow keys move the highlight, Enter takes it, F2 renames and Delete removes the
  // highlighted row, so every action here is reachable without a pointer. Escape is left
  // to bubble: Modal owns it and closing the dialog is what it should do.
  function onListKeydown(e: KeyboardEvent): void {
    if (items.length === 0) {
      return;
    }
    if (e.key === "ArrowDown") {
      active = (activeIndex + 1) % items.length;
    } else if (e.key === "ArrowUp") {
      active = (activeIndex - 1 + items.length) % items.length;
    } else if (e.key === "Home") {
      active = 0;
    } else if (e.key === "End") {
      active = items.length - 1;
    } else if (e.key === "Enter") {
      void pick(items[activeIndex]);
    } else if (e.key === "F2") {
      startRename(items[activeIndex]);
    } else if (e.key === "Delete") {
      pendingDelete = items[activeIndex];
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

<Modal {title} {onClose} width={460} cancel={{ label: "Close", onclick: onClose }}>
  <div class="ph">
    <span class="ph__title">{copy.heading}</span>
    <span class="ph__count">{items.length}</span>
  </div>

  {#if !loaded}
    <p class="note">Loading {copy.noun}s…</p>
  {:else if error}
    <p class="err">{error}</p>
  {:else if items.length === 0}
    <EmptyState compact title={copy.emptyTitle} sub={copy.emptySub}>
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
      aria-label={copy.listLabel}
      aria-activedescendant={renamingId === null ? optionId(activeIndex) : undefined}
      bind:this={listEl}
      onkeydown={onListKeydown}
    >
      {#each items as p, i (p.id)}
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
                aria-label={`${Noun} name`}
                placeholder={copy.renamePlaceholder}
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
                  <span class="cv-ci__name">{label(p)}</span>
                  {#if detail}
                    <span class="cv-ci__sub">{detail(p)}</span>
                  {/if}
                  <span class="cv-ci__sub">
                    Created {fmtSince(p.createdAtMs, now) || "unknown"} · last used
                    {fmtSince(p.lastUsedAtMs, now) || "never"}
                  </span>
                </span>
              </button>
              <span class="acts">
                <IconButton
                  icon="edit"
                  iconSize={12}
                  aria-label={`Rename ${label(p)}`}
                  title="Rename (F2)"
                  onclick={() => startRename(p)}
                />
                <IconButton
                  icon="trash"
                  iconSize={12}
                  danger
                  aria-label={`Delete ${label(p)}`}
                  title="Delete (Del)"
                  onclick={() => (pendingDelete = p)}
                />
              </span>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
    <p class="note">{copy.note}</p>
  {/if}
</Modal>

{#if pendingDelete}
  {@const doomed = pendingDelete}
  <CollectionDialog
    kind="confirm"
    title={`Delete ${copy.noun}`}
    message={copy.deleteMessage(label(doomed))}
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
</style>
