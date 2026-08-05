<script lang="ts">
  import type { Snippet } from "svelte";
  import PageHeader from "$lib/ui/PageHeader.svelte";

  // The one chrome every top-level page wears: the flex-column page frame plus the
  // shared header. Pages pass what differs (title, sub, optional actions) and render
  // their body as children; they never lay out the frame or the header themselves.
  //
  // It exists because both halves had already drifted. Every page carried its own copy
  // of the same `.page` rule, and some copies had picked up `min-height: 0` while others
  // had not -- a difference that decides whether a scrolling child works at all. The
  // Canvases page meanwhile grew a 34px breadcrumb of its own instead of the 58px header
  // the other six shared. Neither divergence is reachable from here: there is one frame
  // and one header, and a page cannot express a different one without editing this file.
  //
  // The Studio page is the deliberate exception and does NOT use this -- it is the live
  // console, it has no page header, and its root carries the show/hide that keeps the
  // preview surface mounted across navigation.
  interface Props {
    title: string;
    sub?: string;
    actions?: Snippet;
    children: Snippet;
  }
  let { title, sub, actions, children }: Props = $props();
</script>

<div class="page">
  <PageHeader {title} {sub} {actions} />
  {@render children()}
</div>

<style>
  .page {
    height: 100%;
    display: flex;
    flex-direction: column;
    /* Load-bearing: without it a flex child cannot shrink below its content, and any
       body that scrolls internally overflows the window instead. */
    min-height: 0;
    background: var(--color-base);
    color: var(--color-text);
  }
</style>
