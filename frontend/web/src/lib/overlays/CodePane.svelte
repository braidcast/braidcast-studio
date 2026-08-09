<script lang="ts">
  // A controlled CodeMirror 6 editor, one instance per language (html/css/js). The
  // `value` prop is the source of truth: when the selected widget changes wholesale
  // the $effect re-seeds the doc; local typing flows back out via onChange. The view
  // is torn down in onDestroy so nothing leaks across widget switches / page unmounts.
  import { onMount, onDestroy } from "svelte";
  import { EditorView, basicSetup } from "codemirror";
  import { EditorState } from "@codemirror/state";
  import { html } from "@codemirror/lang-html";
  import { css } from "@codemirror/lang-css";
  import { javascript } from "@codemirror/lang-javascript";
  import { oneDark } from "@codemirror/theme-one-dark";

  let {
    value,
    lang,
    label,
    readonly = false,
    onChange,
  }: {
    value: string;
    lang: "html" | "css" | "javascript";
    /** Accessible name for the editable region; the visible kicker is a sibling. */
    label: string;
    /** Refuses edits from the keyboard while leaving the text focusable, selectable and
     * scrollable — an uneditable pane still has to be readable without a mouse. Fixed at
     * mount: the two call sites live in mutually exclusive branches, so flipping it always
     * arrives as a fresh instance. */
    readonly?: boolean;
    onChange: (v: string) => void;
  } = $props();

  let host: HTMLDivElement;
  let view: EditorView | undefined;
  // True only while we programmatically re-seed the doc on a widget switch, so the
  // updateListener can skip that synthetic docChange -- otherwise a freshly-loaded
  // widget would save itself back ~500ms later. Only user keystrokes schedule a save.
  let seeding = false;

  const langExt = () => (lang === "html" ? html() : lang === "css" ? css() : javascript());

  onMount(() => {
    view = new EditorView({
      parent: host,
      state: EditorState.create({
        doc: value,
        extensions: [
          basicSetup,
          langExt(),
          oneDark,
          EditorState.readOnly.of(readonly),
          // The content div stays contenteditable either way — readOnly refuses the input
          // rather than removing the affordance — so the state has to be said in ARIA too.
          EditorView.contentAttributes.of(
            readonly ? { "aria-label": label, "aria-readonly": "true" } : { "aria-label": label },
          ),
          EditorView.updateListener.of((u) => {
            if (u.docChanged && !seeding) {
              onChange(u.state.doc.toString());
            }
          }),
        ],
      }),
    });
  });

  onDestroy(() => view?.destroy());

  // Re-seed the doc when the selected widget changes (value prop replaced wholesale).
  // Guard against feeding our own keystrokes back in (value === current doc).
  $effect(() => {
    if (view && value !== view.state.doc.toString()) {
      seeding = true;
      try {
        view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: value } });
      } finally {
        seeding = false;
      }
    }
  });
</script>

<div class="code-host" bind:this={host}></div>

<style>
  .code-host {
    height: 100%;
    overflow: auto;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  /* CodeMirror sizes to the host; keep its own chrome flush to the industrial frame. */
  .code-host :global(.cm-editor) {
    height: 100%;
    font-family: var(--font-mono);
    font-size: 12px;
  }
  .code-host :global(.cm-editor.cm-focused) {
    outline: none;
  }
  .code-host :global(.cm-scroller) {
    font-family: var(--font-mono);
  }
</style>
