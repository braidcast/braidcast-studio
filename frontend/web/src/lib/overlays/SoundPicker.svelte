<script lang="ts">
  // The value control for a `sound` field: the bundled library first, this widget's own
  // uploads second. The stored value is "" (no sound), "library:<id>" or "assets/<file>".
  // Preview plays here in the editor through a local Audio element -- never through the
  // overlay -- so auditioning a sound has no effect on anything a scene is showing.
  import { onDestroy, untrack } from "svelte";
  import type { OverlayAsset } from "$lib/api/bridge";
  import Button from "$lib/ui/Button.svelte";
  import IconButton, { ICONBTN_ROW } from "$lib/ui/IconButton.svelte";
  import {
    LIBRARY_PREFIX,
    libraryFileUrl,
    librarySoundId,
    loadSoundLibrary,
    SOUND_CATEGORIES,
    SoundLibraryMissing,
    type LibrarySound,
  } from "$lib/overlays/soundLibrary";
  import { assetFileOf, assetPreviewUrl, formatBytes } from "$lib/overlays/scopes/scopedAssets";

  let {
    value,
    name,
    assets,
    widgetUrl,
    uploading,
    ariaDescribedBy,
    onUpload,
    onChange,
  }: {
    value: string;
    name: string;
    /** The widget's uploads; the sound ones are listed under My uploads. */
    assets: OverlayAsset[];
    widgetUrl: string;
    uploading: boolean;
    ariaDescribedBy?: string;
    onUpload: (file: File) => void;
    onChange: (next: string) => void;
  } = $props();

  type Tab = "library" | "uploads";
  const TABS: { id: Tab; label: string }[] = [
    { id: "library", label: "Library" },
    { id: "uploads", label: "My uploads" },
  ];
  const ALL = "All";

  const uid = $props.id();
  // Opens on the tab the current value lives in; switching tabs afterwards is the user's.
  let tab = $state<Tab>(untrack(() => (assetFileOf(value) ? "uploads" : "library")));
  let category = $state<string>(ALL);
  let query = $state("");
  let library = $state<LibrarySound[] | null>(null);
  let libraryError = $state<string | null>(null);
  let libraryMissing = $state(false);
  let fileInput = $state<HTMLInputElement | null>(null);

  loadSoundLibrary()
    .then((l) => (library = l))
    .catch((e: unknown) => {
      libraryMissing = e instanceof SoundLibraryMissing;
      libraryError = (e as Error).message;
    });

  const uploads = $derived(assets.filter((a) => a.kind === "sound"));
  const shown = $derived.by(() => {
    const q = query.trim().toLowerCase();
    return (library ?? []).filter(
      (s) => (category === ALL || s.category === category) && (q === "" || s.name.toLowerCase().includes(q)),
    );
  });
  const selectedLabel = $derived.by(() => {
    const id = librarySoundId(value);
    if (id) {
      return library?.find((s) => s.id === id)?.name ?? `Missing library sound "${id}"`;
    }
    return assetFileOf(value) ?? "No sound";
  });

  // One preview at a time, stopped on teardown so a sound never outlives the editor.
  let audio: HTMLAudioElement | null = null;
  let previewing = $state<string | null>(null);
  function stopPreview(): void {
    audio?.pause();
    audio = null;
    previewing = null;
  }
  function preview(key: string, url: string): void {
    if (previewing === key) {
      stopPreview();
      return;
    }
    stopPreview();
    const a = new Audio(url);
    a.onended = () => {
      if (audio === a) {
        stopPreview();
      }
    };
    audio = a;
    previewing = key;
    void a.play().catch(() => stopPreview());
  }
  onDestroy(stopPreview);

  const seconds = (ms: number): string => `${(ms / 1000).toFixed(1)} s`;

  function onTabKey(e: KeyboardEvent, i: number): void {
    const step = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
    if (step !== 0) {
      e.preventDefault();
      tab = TABS[(i + step + TABS.length) % TABS.length].id;
      document.getElementById(`${uid}-tab-${tab}`)?.focus();
    }
  }
</script>

<div class="sp">
  <div class="sp__now">
    <span class="sp__nowlabel" title={selectedLabel}>{selectedLabel}</span>
    {#if value}
      <Button size="xs" variant="bare" onclick={() => onChange("")}>No sound</Button>
    {/if}
  </div>

  <div class="sp__tabs" role="tablist" aria-label="{name} source">
    {#each TABS as t, i (t.id)}
      <button
        type="button"
        id="{uid}-tab-{t.id}"
        class="sp__tab"
        class:on={tab === t.id}
        role="tab"
        aria-selected={tab === t.id}
        aria-controls="{uid}-panel"
        tabindex={tab === t.id ? 0 : -1}
        onclick={() => (tab = t.id)}
        onkeydown={(e) => onTabKey(e, i)}>{t.label}</button
      >
    {/each}
  </div>

  <div class="sp__panel" id="{uid}-panel" role="tabpanel" aria-labelledby="{uid}-tab-{tab}">
    {#if tab === "library"}
      <div class="sp__filters">
        <div class="sp__chips" role="group" aria-label="Sound category">
          {#each [ALL, ...SOUND_CATEGORIES] as c (c)}
            <button type="button" class="sp__chip" class:on={category === c} aria-pressed={category === c} onclick={() => (category = c)}
              >{c}</button
            >
          {/each}
        </div>
        <label class="sp__search">
          <span class="sp__searchlabel">Search</span>
          <input type="search" placeholder="Sound name" bind:value={query} />
        </label>
      </div>
      {#if libraryMissing}
        <p class="sp__msg">This build has no library sounds yet. Upload one under My uploads.</p>
      {:else if libraryError}
        <p class="sp__msg sp__msg--err">The sound library could not be loaded ({libraryError}).</p>
      {:else if !library}
        <p class="sp__msg">Loading sounds…</p>
      {:else if shown.length === 0}
        <p class="sp__msg">No sound matches.</p>
      {:else}
        <ul class="sp__rows" aria-label="{name} library" aria-describedby={ariaDescribedBy}>
          {#each shown as s (s.id)}
            {@const v = LIBRARY_PREFIX + s.id}
            <li class="sp__row" class:on={value === v}>
              <IconButton
                {...ICONBTN_ROW}
                icon={previewing === v ? "stop" : "play"}
                aria-label="{previewing === v ? 'Stop' : 'Play'} {s.name}"
                onclick={() => preview(v, libraryFileUrl(s.file))}
              />
              <button type="button" class="sp__pick" aria-pressed={value === v} onclick={() => onChange(v)}>
                <span class="sp__name">{s.name}</span>
                <span class="sp__meta">{s.category} · {seconds(s.durationMs)}</span>
              </button>
            </li>
          {/each}
        </ul>
      {/if}
    {:else}
      <div class="sp__upload">
        <input
          bind:this={fileInput}
          class="sp__file"
          type="file"
          accept="audio/*"
          tabindex="-1"
          aria-hidden="true"
          onchange={(e) => {
            const f = e.currentTarget.files?.[0];
            if (f) {
              onUpload(f);
            }
            e.currentTarget.value = "";
          }}
        />
        <Button size="sm" variant="surface" disabled={uploading} onclick={() => fileInput?.click()}>
          {uploading ? "Uploading…" : "Upload a sound"}
        </Button>
      </div>
      {#if uploads.length === 0}
        <p class="sp__msg">No sounds uploaded to this overlay yet.</p>
      {:else}
        <ul class="sp__rows" aria-label="{name} uploads">
          {#each uploads as a (a.file)}
            {@const v = "assets/" + a.file}
            <li class="sp__row" class:on={value === v}>
              <IconButton
                {...ICONBTN_ROW}
                icon={previewing === v ? "stop" : "play"}
                aria-label="{previewing === v ? 'Stop' : 'Play'} {a.file}"
                onclick={() => preview(v, assetPreviewUrl(widgetUrl, a.file))}
              />
              <button type="button" class="sp__pick" aria-pressed={value === v} onclick={() => onChange(v)}>
                <span class="sp__name">{a.file}</span>
                {#if a.bytes}<span class="sp__meta">{formatBytes(a.bytes)}</span>{/if}
              </button>
            </li>
          {/each}
        </ul>
      {/if}
    {/if}
  </div>
</div>

<style>
  .sp {
    display: flex;
    flex-direction: column;
    gap: 6px;
    width: 100%;
    min-width: 0;
  }
  .sp__now {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .sp__nowlabel {
    flex: 1;
    min-width: 0;
    font-size: 12px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .sp__tabs {
    display: flex;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .sp__tab {
    height: auto;
    padding: 5px 10px;
    background: transparent;
    border: 0;
    border-bottom: 2px solid transparent;
    color: var(--color-muted);
    font-size: 11px;
  }
  .sp__tab:hover {
    color: var(--color-text);
  }
  .sp__tab.on {
    color: var(--color-text);
    border-bottom-color: var(--color-accent);
  }
  .sp__tab:focus-visible,
  .sp__chip:focus-visible,
  .sp__pick:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .sp__panel {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .sp__filters {
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .sp__chips {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }
  .sp__chip {
    height: auto;
    padding: 3px 8px;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    font-size: 10px;
  }
  .sp__chip.on {
    border-color: var(--color-accent);
    color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 12%, transparent);
  }
  .sp__search {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .sp__searchlabel {
    font-size: 11px;
    color: var(--color-dim);
  }
  .sp__search input {
    flex: 1;
    min-width: 0;
  }
  .sp__rows {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 220px;
    overflow-y: auto;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
  }
  .sp__row {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 2px 6px;
    border-left: 2px solid transparent;
  }
  .sp__row + .sp__row {
    border-top: var(--border-weight) solid var(--color-border);
  }
  .sp__row.on {
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
  }
  .sp__pick {
    flex: 1;
    min-width: 0;
    height: auto;
    display: flex;
    align-items: baseline;
    gap: 8px;
    padding: 5px 2px;
    background: transparent;
    border: 0;
    text-align: left;
    color: var(--color-text);
    cursor: pointer;
  }
  .sp__name {
    flex: 1;
    min-width: 0;
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .sp__meta {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .sp__msg {
    margin: 0;
    font-size: 11px;
    color: var(--color-muted);
  }
  .sp__msg--err {
    color: var(--color-live-text);
  }
  .sp__file {
    display: none;
  }
</style>
