<script lang="ts">
  import { tick } from "svelte";
  import { admitTags, tagRuleText, type TagLimits } from "$lib/dialogs/golive/fieldValue";
  import { focusOnMount } from "$lib/utils/focusActions";

  // Chip-style tag editor. Holds its own draft so each instance (shared block + every
  // per-destination override) is independent. Every tag is a chip in both states, and
  // every chip is editable in place; the inherited state differs by treatment (dashed,
  // dimmed, italic) rather than by shape, with the row's own "using …"/"overrides …" tag
  // carrying the distinction in words.
  interface Props {
    values: string[];
    onChange: (next: string[]) => void;
    /** The tags this layer would inherit, for when it holds none of its own. Shown as
     * chips, because an inherited tag list is the list that will be sent — printing it as
     * a sentence made a set of values read as prose. */
    ghostValues?: string[];
    accent?: boolean;
    /** The provider's tag limits, straight off its descriptor. Enforced here, at the
     * moment a tag arrives, because the provider that enforces them refuses the WHOLE
     * metadata push over one bad tag and that refusal blocks the go-live — so the first
     * sign of a rule broken while typing must not be a stream that never started. An
     * absent limit is not enforced, and a call site with no descriptor enforces none. */
    limits?: TagLimits;
    /** Display name of the platform whose rules `limits` are, named in every sentence
     * about them. The rules differ per platform — Twitch caps ten lowercase words where
     * YouTube takes any 500 characters' worth — so a refusal that does not say who is
     * refusing leaves the streamer to guess. */
    platform?: string;
  }
  let { values, onChange, ghostValues = [], accent = false, limits = {}, platform = "" }: Props = $props();

  const ADD_PROMPT = "+ add";

  // Said on the chips of an inherited list, because touching one of them is what turns
  // the whole list into this layer's own — a consequence worth naming before the click,
  // not only in the row tag that changes after it.
  const OVERRIDE_NOTE = " — overrides the inherited tags";

  // Taught while a box has focus, since the two separators are the whole reason a pasted
  // list becomes chips rather than one long tag.
  const SEPARATOR_HINT = "comma or Enter separates tags";

  // How close to a limit the list has to get before a running readout is worth the space.
  // Headroom rather than a fraction of the limit, because the two limits are counted in
  // different units: three tags from a cap of ten is the edge, while the same 70% of a
  // 500-character budget still has room for eight more tags. A readout that appears
  // there is a warning about nothing, and a control that warns about nothing is one
  // whose warnings stop being read.
  const COUNT_HEADROOM = 3;
  const CHARS_HEADROOM = 60;

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

  const inheriting = $derived(values.length === 0 && ghostValues.length > 0);
  // The list every edit path starts from. While inheriting, that is the inherited list
  // itself: overriding means diverging from those tags, so the first edit keeps the other
  // twelve instead of replacing all thirteen with the one tag that was touched. Emptying
  // the list again is still the way back to inheriting.
  const base = $derived(inheriting ? ghostValues : values);

  // Both readouts name the platform, because both limits are one platform's and the same
  // list is perfectly legal on the next card over.
  const countHint = $derived(
    limits.maxTags !== undefined && base.length >= limits.maxTags - COUNT_HEADROOM
      ? `${base.length}/${limits.maxTags} tags on ${platform || "this platform"}`
      : "",
  );
  const charsUsed = $derived(base.reduce((n, t) => n + t.length, 0));
  const totalHint = $derived(
    limits.maxTotalChars !== undefined && charsUsed >= limits.maxTotalChars - CHARS_HEADROOM
      ? `${charsUsed}/${limits.maxTotalChars} characters on ${platform || "this platform"}`
      : "",
  );
  // The per-tag rule is taught while a box has focus rather than parked under the control:
  // at rest it is a sentence nobody asked for, and at the moment of typing it is the thing
  // that stops the refusal below from ever being needed.
  const ruleHint = $derived(
    tagRuleText(limits) === "" ? "" : `${platform || "these"} tags: ${tagRuleText(limits)}`,
  );
  const focused = $derived(draftFocused || editTag !== "");
  const hint = $derived(
    [countHint, totalHint, ...(focused ? [SEPARATOR_HINT, ruleHint] : [])].filter((s) => s !== "").join(" · "),
  );

  function editLabel(tag: string): string {
    return `Edit tag ${tag}${inheriting ? OVERRIDE_NOTE : ""}`;
  }
  function removeLabel(tag: string): string {
    return `Remove tag ${tag}${inheriting ? OVERRIDE_NOTE : ""}`;
  }

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
      return;
    }
    const kept = base;
    const admitted = admitTags(kept, parts, limits, platform);
    note = admitted.note;
    // What was refused stays in the box it came from, where the caret already is and where
    // it can be fixed -- a tag turned away silently is worse than the go-live it was going
    // to cost. Assigned rather than appended: this box WAS the text being admitted.
    draft = admitted.rejected.join(", ");
    if (admitted.accepted.length > 0) {
      onChange([...kept, ...admitted.accepted]);
    }
  }

  function cancelEdit(): void {
    editTag = "";
    editDraft = "";
  }

  function commitEdit(refocus: boolean): void {
    const kept = base;
    const at = kept.indexOf(editTag);
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
    if (parts.length === 1 && parts[0] === kept[at]) {
      return;
    }
    note = "";
    const rest = kept.filter((_, i) => i !== at);
    // Editing a tag onto one that already exists collapses the two rather than adding a
    // second copy, the same way a pasted list drops its own repeats. Here a duplicate is
    // not merely untidy: it would give two {#each} blocks the same key. admitTags absorbs
    // it, which is also where the provider's limits are applied to an edit.
    const admitted = admitTags(rest, parts, limits, platform);
    note = admitted.note;
    if (admitted.rejected.length > 0) {
      // Appended, not assigned: unlike commit() above, the draft box is a bystander here
      // and may already hold text of its own.
      draft = [draft, ...admitted.rejected].filter((s) => s !== "").join(", ");
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

  function remove(tag: string, e: MouseEvent): void {
    // A refusal the removed tag was making room against no longer stands.
    note = "";
    onChange(base.filter((v) => v !== tag));
    if (e.detail === 0) {
      // Keyboard activation, so the button holding focus is about to be destroyed:
      // without this the focus falls to <body> and the next Tab restarts at the top of
      // the dialog.
      void focusDraft();
    }
  }

  // Clicking the box is the primary gesture on a chip field, so the padding and the gaps
  // between chips need to focus the draft too, not only the draft's own hit area. Gated
  // on the click landing on the container itself: a chip, its ×, or an input already
  // took the click as its own target, so this only fires on the empty space around them.
  function focusDraftOnContainerClick(e: MouseEvent): void {
    if (e.target === e.currentTarget) {
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

  // A pasted list becomes chips on the paste itself rather than waiting for Enter or a
  // blur. Waiting is what let a whole keyword list sit in the box as one line of text.
  // Text carrying no separator is left to the browser, so pasting a single tag still
  // lands in the box the caret is in and can be edited before it is committed.
  function splitOnPaste(e: ClipboardEvent, current: string): string {
    const text = e.clipboardData?.getData("text") ?? "";
    if (!/[,\n]/.test(text)) {
      return "";
    }
    e.preventDefault();
    return [current, text].filter((s) => s !== "").join(",");
  }

  function onDraftPaste(e: ClipboardEvent): void {
    const combined = splitOnPaste(e, draft);
    if (combined === "") {
      return;
    }
    draft = combined;
    commit();
  }

  function onEditPaste(e: ClipboardEvent): void {
    const combined = splitOnPaste(e, editDraft);
    if (combined === "") {
      return;
    }
    editDraft = combined;
    commitEdit(true);
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
      if (draft === "") {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      draft = "";
      // The refusal named text that is being abandoned along with the box holding it.
      note = "";
    } else if (e.key === "Backspace" && draft === "" && base.length > 0) {
      // Steps into the previous tag instead of deleting it. The caret lands at its end,
      // so holding Backspace keeps eating characters and an emptied edit drops the tag
      // anyway -- the destructive path stays reachable by the same gesture, without a
      // single keypress being able to lose a tag outright.
      e.preventDefault();
      startEdit(base[base.length - 1]);
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

<!-- Every interactive affordance here (chips, ×, the draft box) is already its own
     focusable element reachable by Tab; this click is a mouse-only convenience on top
     of that, not a second way to reach something a keyboard user would otherwise miss. -->
<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<div class="tags" class:accent class:inheriting onclick={focusDraftOnContainerClick}>
  {#each base as t (t)}
    {#if t === editTag}
      <span class="grow" data-value={editDraft}>
        <input
          type="text"
          size="1"
          aria-label={editLabel(t)}
          aria-describedby={note || hint ? noteId : undefined}
          bind:value={editDraft}
          use:focusOnMount
          onkeydown={onEditKeydown}
          onpaste={onEditPaste}
          onblur={() => commitEdit(false)}
        />
      </span>
    {:else}
      <span class="tag">
        <button
          type="button"
          class="label"
          title={editLabel(t)}
          aria-label={editLabel(t)}
          onmousedown={keepFocus}
          onclick={() => startEdit(t)}>{t}</button
        >
        <button
          type="button"
          class="x"
          title={removeLabel(t)}
          aria-label={removeLabel(t)}
          onmousedown={keepFocus}
          onclick={(e) => remove(t, e)}>×</button
        >
      </span>
    {/if}
  {/each}
  <input
    class="draft"
    type="text"
    size="1"
    aria-label="Add tag"
    aria-describedby={note || hint ? noteId : undefined}
    placeholder={ADD_PROMPT}
    bind:this={draftEl}
    bind:value={draft}
    onkeydown={onDraftKeydown}
    onpaste={onDraftPaste}
    onfocus={() => (draftFocused = true)}
    onblur={() => {
      draftFocused = false;
      commit();
    }}
  />
</div>

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
  /* The field box the chips live in, matching .inp's frame so a tag list reads as one
     control rather than as loose chips on the dialog. Its own width comes from the row,
     never from what it holds: the chips wrap, and it's `.tag` and `.label`'s own
     `min-width: 0` (the draft box has its own, below) that lets a long pasted tag shrink
     to the ellipsis instead of setting a floor the modal then has to grow sideways to
     meet -- `.grow` floors at 3ch and `.x` needs none, since its glyph is fixed. The
     height stops just short of a fifth row, so a list longer than that shows a clipped
     row rather than looking like it ends where the box does. */
  .tags {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 4px;
    width: 100%;
    box-sizing: border-box;
    padding: 4px;
    max-height: 124px;
    /* Explicit on both axes: `auto` on one alone computes to `auto` on the other, which
       would put back the horizontal scrollbar this control exists to not have. */
    overflow: hidden auto;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    cursor: text;
  }
  .tags:focus-within {
    border-color: var(--color-accent);
  }
  /* Override state, the same accent frame .inp.ovr uses; the inherited state takes the
     dashed frame an inherited thumbnail already uses. */
  .tags.accent {
    border-color: var(--color-accent);
  }
  .tags.inheriting {
    border-style: dashed;
  }
  .tag {
    display: inline-flex;
    align-items: center;
    max-width: 100%;
    min-width: 0;
    background: var(--color-surface-2);
    border: var(--border-weight) solid transparent;
    color: var(--color-text);
    font-size: 11px;
    line-height: 1;
  }
  /* Dashed, dimmed and italic -- the same treatment .inp.ghost and the inherited
     thumbnail use, so the state reads the same way in every control on the row. */
  .tags.inheriting .tag {
    background: none;
    border-color: var(--color-border);
    border-style: dashed;
    color: var(--color-muted);
    font-style: italic;
  }
  /* The tag's own text is the edit target. Its cue is a dotted underline that is only
     given a color on hover and focus, so it reads as a shape rather than a hue and the
     chip does not change size when it appears. */
  /* `height: auto` here and on the boxes below opts out of the app-wide control height:
     a chip is a value, not a control, and at 30px each a list of them fills the dialog. */
  .label {
    background: none;
    border: none;
    color: inherit;
    font: inherit;
    height: auto;
    padding: 4px 2px 4px 6px;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
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
    color: var(--color-muted);
    cursor: pointer;
    font: inherit;
    font-size: 13px;
    line-height: 1;
    height: auto;
    padding: 0 6px;
    align-self: stretch;
    flex: 0 0 auto;
  }
  .tag:hover .x {
    color: var(--color-dim);
  }
  .x:hover {
    color: var(--color-live);
  }
  .label:focus-visible,
  .x:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
  /* The box that takes what is typed. It claims the rest of the row and wraps onto one of
     its own when too little is left, so there is always a wide target to click into and a
     pasted line has a definite width to be clamped to rather than a content-derived one. */
  .draft {
    flex: 1 1 12ch;
    min-width: 0;
    height: auto;
    padding: 4px 6px;
    background: none;
    border: none;
    outline: none;
    font: inherit;
    font-size: 11px;
    color: var(--color-text);
  }
  /* .tags:focus-within's border-color is the intended focus cue, but .tags.accent sets
     that same color unconditionally, so it can't tell "focused while overriding" apart
     from "overriding" -- the draft needs its own indicator to survive that state. */
  .draft:focus {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
  .draft::placeholder {
    color: var(--color-muted);
  }
  /* In-place edit: a box that sizes itself to what is typed. The ::after mirrors the value
     into the same grid cell as the input, so the cell is exactly as wide as the text. The
     single `minmax(0, 1fr)` track is what lets that cell be squeezed back down once the
     text outgrows the row: the mirror still sets the box's width while the row has room
     for it, but the track can never be wider than the box flex has settled on, so a long
     value scrolls inside the input instead of spilling out of the control. */
  .grow {
    display: inline-grid;
    grid-template-columns: minmax(0, 1fr);
    align-items: center;
    font-size: 11px;
    flex: 0 1 auto;
    min-width: 3ch;
    max-width: 100%;
  }
  .grow::after,
  .grow input {
    grid-area: 1 / 1;
    padding: 4px 6px;
    font: inherit;
    border: var(--border-weight) solid var(--color-accent);
    box-sizing: border-box;
  }
  /* The mirror still reports the text's full width to the track above -- that is what
     sizes the box -- but it may not carry that width into the box's scrollable area once
     the track has been squeezed. It paints nothing, so clipping it costs nothing. */
  .grow::after {
    content: attr(data-value) " ";
    visibility: hidden;
    white-space: pre;
    min-width: 0;
    overflow: hidden;
  }
  .grow input {
    width: 100%;
    min-width: 0;
    height: auto;
    outline: none;
    background: var(--color-surface-2);
    color: var(--color-text);
  }
  .tag-note {
    font-size: 10px;
    color: var(--color-muted);
    margin-top: 4px;
    line-height: 1.35;
    /* Interpolates raw rejected tag text (fieldValue.ts's admitTags), which can be one
       unbroken pasted token with no space or comma to wrap on. */
    overflow-wrap: anywhere;
  }
  .tag-note.refused {
    color: var(--color-live);
  }
</style>
