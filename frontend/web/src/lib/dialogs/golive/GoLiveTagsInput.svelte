<script lang="ts">
  import { tick } from "svelte";
  import { admitTags, tagRuleText, type TagLimits } from "$lib/dialogs/golive/fieldValue";
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
    /** The provider's tag limits, straight off its descriptor. Enforced here, at the
     * moment a tag arrives, because the provider that enforces them refuses the WHOLE
     * metadata push over one bad tag and that refusal blocks the go-live — so the first
     * sign of a rule broken while typing must not be a stream that never started. An
     * absent limit is not enforced, and a call site with no descriptor enforces none. */
    limits?: TagLimits;
  }
  let { values, onChange, ghostLabel, accent = false, limits = {} }: Props = $props();

  // One string for the button that leaves the ghost state and for the prompt on the box
  // it turns into, so both name the same action.
  const ADD_PROMPT = "+ add";

  // How full the list has to get before a running count is worth the space. A "3/10"
  // beside three chips is a number nobody is reading; the same readout at 7 is the
  // warning that the eleventh tag is not going to fit.
  const LIMIT_HINT_RATIO = 0.7;

  let adding = $state(false);
  let draft = $state("");
  let draftEl = $state<HTMLInputElement | null>(null);
  let draftFocused = $state(false);

  // Why the last tag offered was turned away, "" when none was. Every path that admits
  // tags sets it, so the reason is stated once wherever the refusal happened.
  let note = $state("");

  const uid = $props.id();
  const noteId = `${uid}-note`;

  // The tag under in-place edit, held by value rather than by index because the {#each}
  // below is keyed by the tag string: that identity survives the parent handing back a
  // rebuilt `values`, an index does not. "" means nothing is being edited, which no real
  // tag can be -- parseTags drops empties.
  let editTag = $state("");
  let editDraft = $state("");

  const countHint = $derived(
    limits.maxTags !== undefined && values.length >= Math.ceil(limits.maxTags * LIMIT_HINT_RATIO)
      ? `${values.length}/${limits.maxTags} tags`
      : "",
  );
  const charsUsed = $derived(values.reduce((n, t) => n + t.length, 0));
  const totalHint = $derived(
    limits.maxTotalChars !== undefined && charsUsed >= Math.ceil(limits.maxTotalChars * LIMIT_HINT_RATIO)
      ? `${charsUsed}/${limits.maxTotalChars} characters`
      : "",
  );
  // The per-tag rule is taught while a box has focus rather than parked under the control:
  // at rest it is a sentence nobody asked for, and at the moment of typing it is the thing
  // that stops the refusal below from ever being needed.
  const ruleHint = $derived(draftFocused || editTag !== "" ? tagRuleText(limits) : "");
  const hint = $derived([countHint, totalHint, ruleHint].filter((s) => s !== "").join(" · "));

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
    const parts = parseTags(draft);
    if (parts.length === 0) {
      note = "";
      draft = "";
      adding = false;
      return;
    }
    const admitted = admitTags(values, parts, limits);
    note = admitted.note;
    // What was refused stays in the box it came from, where the caret already is and where
    // it can be fixed -- a tag turned away silently is worse than the go-live it was going
    // to cost. Assigned rather than appended: this box WAS the text being admitted.
    draft = admitted.rejected.join(", ");
    // Something left to fix keeps the box open, or the ghost branch below would fold it
    // away and take the refused text with it.
    adding = admitted.rejected.length > 0;
    if (admitted.accepted.length > 0) {
      onChange([...values, ...admitted.accepted]);
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
    const parts = parseTags(text);
    if (parts.length === 1 && parts[0] === values[at]) {
      return;
    }
    note = "";
    const rest = values.filter((_, i) => i !== at);
    // Editing a tag onto one that already exists collapses the two rather than adding a
    // second copy, the same way a pasted list drops its own repeats. Here a duplicate is
    // not merely untidy: it would give two {#each} blocks the same key. admitTags absorbs
    // it, which is also where the provider's limits are applied to an edit.
    const admitted = admitTags(rest, parts, limits);
    note = admitted.note;
    if (admitted.rejected.length > 0) {
      // Appended, not assigned: unlike commit() above, the draft box is a bystander here
      // and may already hold text of its own.
      draft = [draft, ...admitted.rejected].filter((s) => s !== "").join(", ");
      adding = true;
    }
    if (admitted.accepted.length === 0 && admitted.rejected.length > 0) {
      // Nothing admissible to put in its place, so the tag keeps what it had. The text that
      // was meant to replace it is in the draft box, so the edit is not lost either.
      // Gated on a refusal rather than on an empty result: an edit that empties the tag, or
      // collapses it onto one already present, is meant to remove it and still does.
      return;
    }
    rest.splice(at, 0, ...admitted.accepted);
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
    note = "";
    void focusDraft();
  }

  function remove(tag: string, e: MouseEvent): void {
    // A refusal the removed tag was making room against no longer stands.
    note = "";
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
      // The refusal named text that is being abandoned along with the box holding it.
      note = "";
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
            aria-describedby={note || hint ? noteId : undefined}
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
        aria-describedby={note || hint ? noteId : undefined}
        placeholder={ADD_PROMPT}
        bind:this={draftEl}
        bind:value={draft}
        onkeydown={onDraftKeydown}
        onfocus={() => (draftFocused = true)}
        onblur={() => {
          draftFocused = false;
          commit();
        }}
      />
    </span>
  </div>
{/if}

<!-- One line for everything the limits have to say, so the rule, the running count and the
     refusal never compete for the same space. A refusal outranks the rest: it is the only
     one of them that is about text the user is holding right now. Both states say it in
     words -- a count read as "9/10 tags" and a sentence naming the rule -- so neither
     depends on the color it is printed in. -->
{#if note}
  <div class="tag-note refused" id={noteId} role="alert">{note}</div>
{:else if hint}
  <div class="tag-note" id={noteId}>{hint}</div>
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
  .tag-note {
    font-size: 10px;
    color: var(--color-muted);
    margin-top: 4px;
    line-height: 1.35;
  }
  .tag-note.refused {
    color: var(--color-live);
  }
  .grow input:focus,
  .grow.editing input {
    outline: none;
    border-color: var(--color-accent);
    background: var(--color-base);
  }
</style>
