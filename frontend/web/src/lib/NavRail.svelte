<script lang="ts">
  // The 70px left icon rail (mock's <nav>): a home/logo button → Studio, a divider,
  // five page buttons (Stream/Schedule/Monitor/AI/Settings), a flex spacer, then a
  // footer (Decision B) homing the orphaned menu-bar items — a compact Scene
  // Collection switcher + an About button. Markup/icons/active styling copied from
  // the mock's <nav> + _navItem/home helpers, mapped onto the project tokens
  // (--c-* → --color-*). Zero border-radius.
  import { onMount } from "svelte";
  import { obs, type CollectionInfo } from "./bridge";
import { EV } from "./eventNames";
  import { pageStore, setPage, type Page } from "./pageStore.svelte";
  import { openAbout } from "./aboutOpener.svelte";
  import CollectionDialog, { type DialogSpec } from "./CollectionDialog.svelte";
  import { seamStore, SEAM, bumpDiagonals, offsetRight, polygon } from "./seamStore.svelte";

  // One entry per nav button; the icon is rendered by the matching {#if} in the
  // markup keyed off this id, so adding a view is a single entry + one icon branch.
  const NAV_ITEMS: { id: Page; label: string; title: string }[] = [
    { id: "canvases", label: "Canvases", title: "Canvases" },
    { id: "streams", label: "Streams", title: "Stream Profiles" },
    { id: "overlays", label: "Overlays", title: "Overlay Widgets" },
    { id: "schedule", label: "Schedule", title: "Schedule" },
    { id: "monitor", label: "Monitor", title: "Monitor" },
    { id: "ai", label: "AI", title: "AI Control" },
    { id: "settings", label: "Settings", title: "Settings" },
  ];

  // --- Seam bump: measure the active item's center Y and publish it to seamStore ---
  let railEl = $state<HTMLElement>();

  // The bump is the rail's own material; the accent rim is a LINE-px normal offset of
  // its gap-facing diagonals, so it traces the active triangle at a constant width.
  const bumpClip = $derived(polygon(bumpDiagonals(seamStore.ym)));
  const bumpEdgeClip = $derived.by(() => {
    const ym = seamStore.ym;
    const rim = offsetRight(bumpDiagonals(ym), SEAM.LINE);
    return polygon([...rim, [0, ym + SEAM.HALF], [0, ym - SEAM.HALF]]);
  });

  // Center Y of the active button relative to the rail top. The active element is the
  // .home button on Studio, else the matching .nav-item; footer buttons are never
  // active pages, so a single .active selector is unambiguous. Measured (not derived
  // from static offsets) because the home button, divider, and gap:2px make hand-rolled
  // offsets fragile across layout/resize/font-load.
  function measureSeam(): void {
    const rail = railEl;
    if (!rail) {
      return;
    }
    const el = rail.querySelector<HTMLElement>(".active");
    if (!el) {
      return;
    }
    const r = el.getBoundingClientRect();
    seamStore.ym = r.top - rail.getBoundingClientRect().top + r.height / 2;
  }

  // Runs after DOM updates and before paint, so the class toggle for the new page is
  // already applied when we measure — the bump lands without a mount slide.
  $effect(() => {
    pageStore.page;
    measureSeam();
  });

  // --- Scene Collection switcher (rehomed from the menu bar, Phase 6a logic) ---
  let collections = $state<CollectionInfo[]>([]);
  let dialog = $state<DialogSpec | null>(null);
  let menuOpen = $state(false);

  const active = $derived(collections.find((c) => c.active));

  function refreshCollections() {
    obs
      .call("collections.list")
      .then((list) => (collections = list ?? []))
      .catch((e) => console.log("collections.list failed: " + (e as Error).message));
  }

  onMount(() => {
    refreshCollections();
    const offCollections = obs.on(EV.collectionsChanged, refreshCollections);
    // Rail height/layout shifts (window resize, font load) move the active center too.
    const ro = new ResizeObserver(() => measureSeam());
    if (railEl) {
      ro.observe(railEl);
    }
    return () => {
      offCollections();
      ro.disconnect();
    };
  });

  function showError(e: unknown) {
    dialog = { kind: "alert", title: "Scene Collections", message: (e as Error).message };
  }

  function switchCollection(id: string) {
    menuOpen = false;
    obs.call("collections.switch", { id }).catch(showError);
  }

  function newCollection() {
    menuOpen = false;
    dialog = {
      kind: "prompt",
      title: "New Scene Collection",
      confirmLabel: "Create",
      onCommit: (name) => {
        if (!name) {
          return;
        }
        obs.call("collections.create", { name }).catch(showError);
      },
    };
  }

  function renameCollection() {
    menuOpen = false;
    if (!active) {
      return;
    }
    const current = active;
    dialog = {
      kind: "prompt",
      title: "Rename Scene Collection",
      initial: current.name,
      confirmLabel: "Rename",
      onCommit: (name) => {
        if (!name || name === current.name) {
          return;
        }
        obs.call("collections.rename", { id: current.id, name }).catch(showError);
      },
    };
  }

  function duplicateCollection() {
    menuOpen = false;
    if (!active) {
      return;
    }
    const current = active;
    dialog = {
      kind: "prompt",
      title: "Duplicate Scene Collection",
      initial: current.name + " copy",
      confirmLabel: "Duplicate",
      onCommit: (name) => {
        if (!name) {
          return;
        }
        obs.call("collections.duplicate", { id: current.id, name }).catch(showError);
      },
    };
  }

  function deleteCollection() {
    menuOpen = false;
    if (!active) {
      return;
    }
    const current = active;
    dialog = {
      kind: "confirm",
      title: "Delete Scene Collection",
      message: `Delete "${current.name}"? This cannot be undone.`,
      confirmLabel: "Delete",
      onCommit: () => {
        obs.call("collections.remove", { id: current.id }).catch(showError);
      },
    };
  }
</script>

<nav class="rail" bind:this={railEl}>
  <span class="bump-edge" style="width:{SEAM.D + 4}px; clip-path:{bumpEdgeClip}"></span>
  <span class="bump" style="width:{SEAM.D}px; clip-path:{bumpClip}"></span>

  <button
    class="home"
    class:active={pageStore.page === "studio"}
    title="Home · Studio"
    onclick={() => setPage("studio")}
  >
    <div class="logo">
      <div class="logo-back"></div>
      <div class="logo-fore"><span class="logo-tri"></span></div>
    </div>
  </button>

  <div class="divider"></div>

  <div class="nav-items">
    {#each NAV_ITEMS as item (item.id)}
      <button
        class="nav-item"
        class:active={pageStore.page === item.id}
        title={item.title}
        onclick={() => setPage(item.id)}
      >
        {#if item.id === "canvases"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
            <rect x="3.5" y="3.5" width="12" height="9" />
            <rect x="8.5" y="11.5" width="12" height="9" fill="var(--color-rail)" />
          </svg>
        {:else if item.id === "streams"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">
            <circle cx="12" cy="12" r="2.1" fill="currentColor" stroke="none" />
            <path d="M8 8.2 a5.4 5.4 0 0 0 0 7.6" />
            <path d="M16 8.2 a5.4 5.4 0 0 1 0 7.6" />
            <path d="M5.4 5.6 a9 9 0 0 0 0 12.8" />
            <path d="M18.6 5.6 a9 9 0 0 1 0 12.8" />
          </svg>
        {:else if item.id === "overlays"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
            <rect x="3.5" y="4.5" width="17" height="12" />
            <path d="M3.5 13 L8 9 L11.5 12.5 L15 8 L20.5 13.5" />
            <line x1="8" y1="20" x2="16" y2="20" />
          </svg>
        {:else if item.id === "schedule"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">
            <rect x="3.5" y="5" width="17" height="15" />
            <line x1="3.5" y1="9.2" x2="20.5" y2="9.2" />
            <line x1="8" y1="3.4" x2="8" y2="6.6" />
            <line x1="16" y1="3.4" x2="16" y2="6.6" />
            <rect x="7" y="12" width="2.4" height="2.4" fill="currentColor" stroke="none" />
            <rect x="14.6" y="12" width="2.4" height="2.4" fill="currentColor" stroke="none" />
          </svg>
        {:else if item.id === "monitor"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">
            <line x1="4" y1="20" x2="20" y2="20" />
            <rect x="6" y="12" width="3" height="6" fill="currentColor" stroke="none" />
            <rect x="10.5" y="7" width="3" height="11" fill="currentColor" stroke="none" />
            <rect x="15" y="14" width="3" height="4" fill="currentColor" stroke="none" />
          </svg>
        {:else if item.id === "ai"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="currentColor">
            <path d="M12 3.4 L13.7 10.3 L20.6 12 L13.7 13.7 L12 20.6 L10.3 13.7 L3.4 12 L10.3 10.3 Z" />
          </svg>
        {:else if item.id === "settings"}
          <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">
            <line x1="4" y1="8" x2="20" y2="8" />
            <circle cx="15" cy="8" r="2.5" fill="var(--color-rail)" />
            <line x1="4" y1="16" x2="20" y2="16" />
            <circle cx="9" cy="16" r="2.5" fill="var(--color-rail)" />
          </svg>
        {/if}
        <span class="label">{item.label}</span>
      </button>
    {/each}
  </div>

  <div class="spacer"></div>

  <div class="footer">
    <div class="collection">
      <button class="foot-btn collection-btn" title="Scene Collection — click to switch" onclick={() => (menuOpen = !menuOpen)}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
          <path d="M12 3.5 L20 8 L12 12.5 L4 8 Z" />
          <path d="M4 12 L12 16.5 L20 12" />
          <path d="M4 16 L12 20.5 L20 16" />
        </svg>
        <span class="foot-kicker">Collection</span>
        <span class="foot-label name">{active?.name ?? "—"}</span>
      </button>

      {#if menuOpen}
        <div class="menu-backdrop" role="presentation" onclick={() => (menuOpen = false)}></div>
        <div class="menu" role="menu" aria-label="Scene Collection">
          {#each collections as c (c.id)}
            <button class="menu-item" class:on={c.active} role="menuitemradio" aria-checked={c.active} onclick={() => switchCollection(c.id)}>
              <span class="tick">{c.active ? "●" : ""}</span><span class="menu-text">{c.name}</span>
            </button>
          {/each}
          <div class="menu-sep"></div>
          <button class="menu-item" role="menuitem" onclick={newCollection}>New…</button>
          <button class="menu-item" role="menuitem" onclick={renameCollection}>Rename…</button>
          <button class="menu-item" role="menuitem" onclick={duplicateCollection}>Duplicate…</button>
          <button class="menu-item" role="menuitem" disabled={collections.length <= 1} onclick={deleteCollection}>Delete</button>
        </div>
      {/if}
    </div>

    <button class="foot-btn" title="About Braidcast" onclick={() => openAbout()}>
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round">
        <circle cx="12" cy="12" r="8.5" />
        <line x1="12" y1="11" x2="12" y2="16.5" />
        <circle cx="12" cy="7.6" r="0.6" fill="currentColor" stroke="none" />
      </svg>
      <span class="foot-label">About</span>
    </button>
  </div>
</nav>

{#if dialog}
  <CollectionDialog {...dialog} onClose={() => (dialog = null)} />
{/if}

<style>
  .rail {
    position: relative;
    width: 70px;
    flex: 0 0 70px;
    background: var(--color-rail);
    display: flex;
    flex-direction: column;
    align-items: stretch;
    padding: 12px 0 10px;
    z-index: 5;
  }

  /* Rail bump: the rail's own material juts to a point at the active item. Sits just
     past the straight edge (left:100%) with the rail's overflow visible, so it reads as
     one shape with the rail. Both layers clip in px from seamStore.ym; the accent rim
     sits behind so only its LINE-px edge shows. */
  .bump,
  .bump-edge {
    position: absolute;
    left: 100%;
    top: 0;
    height: 100%;
    pointer-events: none;
    z-index: 6;
  }
  .bump {
    background: var(--color-rail);
  }
  .bump-edge {
    background: var(--color-accent);
  }
  @media (prefers-reduced-motion: no-preference) {
    .bump,
    .bump-edge {
      transition: clip-path 0.28s cubic-bezier(0.4, 0, 0.15, 1);
    }
  }

  /* Home / logo button (mock home.s). */
  .home {
    position: relative;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 11px 0 13px;
    border: 0;
    height: auto;
    cursor: pointer;
    width: 100%;
    background: none;
  }
  .home.active {
    background: color-mix(in srgb, var(--color-accent) 9%, transparent);
  }
  .logo {
    position: relative;
    width: 30px;
    height: 30px;
  }
  .logo-back {
    position: absolute;
    left: 0;
    top: 1px;
    width: 21px;
    height: 15px;
    border: 1.5px solid var(--color-dim);
  }
  .home.active .logo-back {
    border-color: var(--color-accent);
  }
  .logo-fore {
    position: absolute;
    right: 0;
    bottom: 1px;
    width: 21px;
    height: 15px;
    background: var(--color-accent);
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .logo-tri {
    width: 0;
    height: 0;
    border-left: 6px solid var(--color-accent-ink);
    border-top: 4px solid transparent;
    border-bottom: 4px solid transparent;
    margin-left: 2px;
  }

  .divider {
    height: 1px;
    background: var(--color-border-2);
    margin: 9px 14px 7px;
  }

  .nav-items {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  /* Nav button (mock _navItem.s). */
  .nav-item {
    position: relative;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 5px;
    padding: 9px 0 8px;
    border: 0;
    height: auto;
    cursor: pointer;
    font-family: var(--font-ui);
    width: 100%;
    background: none;
    color: var(--color-dim);
  }
  .nav-item:hover {
    color: var(--color-text);
  }
  .nav-item.active {
    background: color-mix(in srgb, var(--color-accent) 9%, transparent);
    color: var(--color-accent);
  }
  .nav-item .label {
    font-size: 9px;
    letter-spacing: 0.04em;
  }

  .spacer {
    flex: 1;
  }

  /* Footer (Decision B): collection switcher + About. */
  .footer {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .collection {
    position: relative;
  }
  .foot-btn {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 4px;
    padding: 8px 4px 7px;
    border: 0;
    height: auto;
    width: 100%;
    cursor: pointer;
    background: none;
    color: var(--color-dim);
    font-family: var(--font-ui);
  }
  .foot-btn:hover {
    color: var(--color-text);
  }
  .foot-label {
    font-size: 8px;
    letter-spacing: 0.04em;
    max-width: 100%;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .collection-btn {
    gap: 2px;
  }
  .foot-kicker {
    font-family: var(--font-mono);
    font-size: 7px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--color-muted);
  }
  .foot-label.name {
    font-size: 9px;
    color: var(--color-text);
    max-width: 62px;
  }

  /* Collection popup, anchored to the right of the rail at the button. */
  .menu-backdrop {
    position: fixed;
    inset: 0;
    z-index: 40;
  }
  .menu {
    position: absolute;
    left: 100%;
    bottom: 0;
    min-width: 180px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.6);
    padding: 4px 0;
    z-index: 41;
  }
  .menu-item {
    display: flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    text-align: left;
    background: none;
    border: 0;
    height: auto;
    padding: 6px 10px;
    font-family: var(--font-ui);
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    cursor: pointer;
  }
  .menu-item:hover:not(:disabled) {
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
    color: var(--color-accent);
  }
  .menu-item.on {
    color: var(--color-accent);
  }
  .menu-item:disabled {
    color: var(--color-muted);
    cursor: default;
  }
  .tick {
    width: 10px;
    flex: 0 0 auto;
    font-size: 9px;
  }
  .menu-text {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .menu-sep {
    height: 1px;
    background: var(--color-border);
    margin: 4px 0;
  }
</style>
