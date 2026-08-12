<script lang="ts">
  import { tick } from "svelte";
  import { focusOnMount } from "$lib/utils/focusActions";

  // Chip-style tag editor (mock `.tags` / `.tag`). Holds its own draft so each instance
  // (shared block + every per-destination override) is independent. When empty and
  // `ghostLabel` is set, shows the inherited value as italic ghost text with a "+ add"
  // affordance to start overriding. The label arrives already composed so the inherit
  // cue is worded in exactly one place.
  interface Props {
    values: string[];
    onChange: (next: string[]) => void;
    ghostLabel?: string;
    accent?: boolean;
  }
  let { values, onChange, ghostLabel, accent = false }: Props = $props();

  // One string for the button that leaves the ghost state and for the prompt on the box
  // it turns into, so both name the same action.
  const ADD_PROMPT = "+ add";

  let adding = $state(false);
  let draft = $state("");
  let draftEl = $state<HTMLInputElement | null>(null);

  // The tag under in-place edit, held by value rather than by index because the {#each}
  // below is keyed by the tag string: that identity survives the parent handing back a
  // rebuilt `values`, an index does not. "" means nothing is being edited, which no real
  // tag can be -- parseTags drops empties.
  let editTag = $state("");
  let editDraft = $state("");

  // Splits rather than taking the text whole: a tag list is almost always pasted, and
  // every source of one -- YouTube's own field, a keyword tool, an old description --
  // hands it over comma-separated. Shared with the in-place edit, which is just as
  // reachable a paste target as the draft box.
  function parseTags(text: string): string[] {
    const out: string[] = [];
    for (const raw of text.split(/[,\n]/)) {
      const t = raw.trim();
      if (t && !out.includes(t)) {
        out.push(t);
      }
    }
    return out;
  }

  async function focusDraft(): Promise<void> {
    await tick();
    draftEl?.focus();
  }

  function commit(): void {
    const next = [...values];
    for (const t of parseTags(draft)) {
      if (!next.includes(t)) {
        next.push(t);
      }
    }
    draft = "";
    adding = false;
    if (next.length !== values.length) {
      onChange(next);
    }
  }

  function cancelEdit(): void {
    editTag = "";
    editDraft = "";
  }

  function commitEdit(refocus: boolean): void {
    const at = values.indexOf(editTag);
    const text = editDraft;
    // Cleared before onChange: the commit rewrites this tag's string, which is its
    // {#each} key, so Svelte swaps the block for a fresh one -- and a still-set editTag
    // would open that replacement straight back into edit mode.
    cancelEdit();
    if (refocus) {
      void focusDraft();
    }
    if (at < 0) {
      return;
    }
    const rest = values.filter((_, i) => i !== at);
    // Editing a tag onto one that already exists collapses the two rather than adding a
    // second copy, the same way a pasted list drops its own repeats. Here a duplicate is
    // not merely untidy: it would give two {#each} blocks the same key.
    const parts = parseTags(text).filter((t) => !rest.includes(t));
    if (parts.length === 1 && parts[0] === values[at]) {
      return;
    }
    rest.splice(at, 0, ...parts);
    onChange(rest);
  }

  function startEdit(tag: string): void {
    if (editTag !== "") {
      commitEdit(false);
    }
    editTag = tag;
    editDraft = tag;
  }

  function startAdd(): void {
    adding = true;
    draft = "";
    void focusDraft();
  }

  function remove(tag: string, e: MouseEvent): void {
    onChange(values.filter((v) => v !== tag));
    if (e.detail === 0) {
      // Keyboard activation, so the button holding focus is about to be destroyed:
      // without this the focus falls to <body> and the next Tab restarts at the top of
      // the dialog.
      void focusDraft();
    }
  }

  // Lets a chip button take its click without taking focus. Otherwise mousedown blurs
  // the draft box, the commit that follows reflows the row, and Chromium then delivers
  // the click to the container rather than to the button that moved out from under the
  // pointer.
  function keepFocus(e: MouseEvent): void {
    e.preventDefault();
  }

  function onDraftKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" || e.key === ",") {
      e.preventDefault();
      commit();
    } else if (e.key === "Escape") {
      // Stops here rather than bubbling to the modal: the first Escape abandons the
      // draft tag, and only a second one closes the dialog behind it. Claimed only when
      // there is something to abandon -- the box is always present now, so an
      // unconditional stop would swallow the modal's Escape whenever focus happened to
      // be resting in an empty one.
      if (draft === "" && !adding) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      draft = "";
      adding = false;
    } else if (e.key === "Backspace" && draft === "" && values.length > 0) {
      // Steps into the previous tag instead of deleting it. The caret lands at its end,
      // so holding Backspace keeps eating characters and an emptied edit drops the tag
      // anyway -- the destructive path stays reachable by the same gesture, without a
      // single keypress being able to lose a tag outright.
      e.preventDefault();
      startEdit(values[values.length - 1]);
    }
  }

  function onEditKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" || e.key === ",") {
      e.preventDefault();
      commitEdit(true);
    } else if (e.key === "Escape") {
      // Same layering as the draft: this Escape abandons the edit, not the dialog.
      e.preventDefault();
      e.stopPropagation();
      cancelEdit();
      void focusDraft();
    }
  }
</script>

{#if values.length === 0 && ghostLabel && !adding}
  <div class="ghost-line">
    <span class="ghost">{ghostLabel}</span>
    <button type="button" class="tag add" onclick={startAdd}>{ADD_PROMPT}</button>
  </div>
{:else}
  <div class="tags" class:accent>
    {#each values as t (t)}
      {#if t === editTag}
        <span class="grow editing" data-value={editDraft}>
          <input
            type="text"
            size="1"
            aria-label="Edit tag {t}"
            bind:value={editDraft}
            use:focusOnMount
            onkeydown={onEditKeydown}
            onblur={() => commitEdit(false)}
          />
        </span>
      {:else}
        <span class="tag">
          <button
            type="button"
            class="label"
            title="Edit tag"
            aria-label="Edit tag {t}"
            onmousedown={keepFocus}
            onclick={() => startEdit(t)}>{t}</button
          >
          <button
            type="button"
            class="x"
            title="Remove tag"
            aria-label="Remove tag {t}"
            onmousedown={keepFocus}
            onclick={(e) => remove(t, e)}>×</button
          >
        </span>
      {/if}
    {/each}
    <span class="grow" data-value={draft || ADD_PROMPT}>
      <input
        type="text"
        size="1"
        aria-label="Add tag"
        placeholder={ADD_PROMPT}
        bind:this={draftEl}
        bind:value={draft}
        onkeydown={onDraftKeydown}
        onblur={commit}
      />
    </span>
  </div>
{/if}

<style>
  .ghost-line {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .ghost {
    font-style: italic;
    color: var(--color-muted);
    font-size: 12px;
  }
  .tags {
    display: flex;
    gap: 4px;
    flex-wrap: wrap;
    align-items: center;
  }
  .tag {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 3px 8px;
    font-size: 11px;
    color: var(--color-dim);
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
  .tags.accent .tag {
    border-color: var(--color-accent);
    color: var(--color-text);
  }
  .tag.add {
    cursor: pointer;
    color: var(--color-muted);
  }
  .tag.add:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  /* The tag's own text is the edit target. Its cue is a dotted underline that is only
     given a color on hover and focus, so it reads as a shape rather than a hue and the
     chip does not change size when it appears -- the accent border is already spoken
     for by the override state. */
  .label {
    background: none;
    border: none;
    color: inherit;
    font: inherit;
    padding: 0;
    cursor: text;
    text-decoration: underline dotted transparent;
    text-underline-offset: 2px;
  }
  .tag:hover .label,
  .label:focus-visible {
    text-decoration-color: currentColor;
  }
  .x {
    background: none;
    border: none;
    color: inherit;
    cursor: pointer;
    font: inherit;
    padding: 0;
    line-height: 1;
  }
  .x:hover {
    color: var(--color-live);
  }
  .label:focus-visible,
  .x:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
  /* Box that sizes itself to what is typed. The ::after mirrors the value into the same
     grid cell as the input, so the cell is exactly as wide as the text; `size="1"` on
     the input keeps its own 20-character preferred width from setting the floor instead.
     Bordered only while focused, so an unused box is a prompt rather than a hole in the
     row. */
  .grow {
    display: inline-grid;
    align-items: center;
    font-size: 11px;
    max-width: 100%;
  }
  .grow::after,
  .grow input {
    grid-area: 1 / 1;
    padding: 3px 8px;
    font: inherit;
    border: var(--border-weight) solid transparent;
    box-sizing: border-box;
  }
  .grow::after {
    content: attr(data-value) " ";
    visibility: hidden;
    white-space: pre;
  }
  .grow input {
    width: 100%;
    min-width: 0;
    background: none;
    color: var(--color-text);
  }
  .grow input::placeholder {
    color: var(--color-muted);
  }
  .grow input:focus,
  .grow.editing input {
    outline: none;
    border-color: var(--color-accent);
    background: var(--color-base);
  }
</style>
