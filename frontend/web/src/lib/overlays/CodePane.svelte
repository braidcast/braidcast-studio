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
    onChange,
  }: { value: string; lang: "html" | "css" | "javascript"; onChange: (v: string) => void } = $props();

  let host: HTMLDivElement;
  let view: EditorView | undefined;

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
          EditorView.updateListener.of((u) => {
            if (u.docChanged) {
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
      view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: value } });
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
