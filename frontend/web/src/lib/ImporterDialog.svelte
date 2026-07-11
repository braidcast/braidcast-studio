<script lang="ts">
  import Modal from "./Modal.svelte";
  import { obs, type ImporterScan, type ImporterImportCollection, type ImporterImportResult } from "./bridge";

  interface Props {
    onClose: () => void;
  }
  let { onClose }: Props = $props();

  // The native preview overlay is suspended by the opener for this dialog's whole
  // lifetime, so it never occludes the modal.

  let scan = $state<ImporterScan | null>(null);
  let loaded = $state(false);
  let scanning = $state(false);
  let error = $state<string | null>(null);

  // Selection model, keyed by collection.file (the import key):
  //   collSelected[file]  -> include this collection at all (default: all on)
  //   expanded[file]      -> show its per-scene checkbox list
  //   sceneSel[file][name]-> per-scene checkbox (default: all on)
  // Nested records are reassigned wholesale on mutation so runes track them.
  let collSelected = $state<Record<string, boolean>>({});
  let expanded = $state<Record<string, boolean>>({});
  let sceneSel = $state<Record<string, Record<string, boolean>>>({});

  let importService = $state(false);
  let importVideo = $state(false);
  let importAudio = $state(false);
  // Global audio devices (Desktop/Mic) + their filters live at each collection's root,
  // not in the settings scan. Off by default: importing overwrites the fork's single
  // shared global audio set, so it is an explicit opt-in (also the lost-filter recovery).
  let importGlobalAudio = $state(false);

  let busy = $state(false);
  let result = $state<ImporterImportResult | null>(null);

  function report(e: unknown) {
    error = (e as Error).message;
  }

  // Seed the selection from a fresh scan: every collection + every scene checked.
  function seed(s: ImporterScan) {
    const sel: Record<string, boolean> = {};
    const exp: Record<string, boolean> = {};
    const scenes: Record<string, Record<string, boolean>> = {};
    for (const c of s.collections) {
      sel[c.file] = true;
      exp[c.file] = false;
      const m: Record<string, boolean> = {};
      for (const name of c.scenes) {
        m[name] = true;
      }
      scenes[c.file] = m;
    }
    collSelected = sel;
    expanded = exp;
    sceneSel = scenes;
    importService = s.service?.present ?? false;
    importVideo = s.video != null;
    importAudio = s.audio != null;
    importGlobalAudio = false;
  }

  // Pass path="" to auto-detect the OBS Studio install; a real path re-scans there.
  async function runScan(path: string) {
    scanning = true;
    result = null;
    try {
      const s = await obs.call("importer.scan", { path });
      scan = s;
      if (s.found) {
        seed(s);
      }
      error = null;
    } catch (e) {
      report(e);
    } finally {
      scanning = false;
      loaded = true;
    }
  }

  $effect(() => {
    void runScan("");
  });

  async function browse() {
    try {
      const { path } = await obs.call("dialog.openFile", { mode: "directory" });
      if (path != null) {
        loaded = false;
        await runScan(path);
      }
    } catch (e) {
      report(e);
    }
  }

  function toggleColl(file: string, checked: boolean) {
    collSelected = { ...collSelected, [file]: checked };
  }

  function toggleExpand(file: string) {
    expanded = { ...expanded, [file]: !expanded[file] };
  }

  function toggleScene(file: string, name: string, checked: boolean) {
    sceneSel = { ...sceneSel, [file]: { ...sceneSel[file], [name]: checked } };
  }

  // Scenes checked for one collection, in scan order.
  function selectedScenes(file: string): string[] {
    const c = scan?.collections.find((x) => x.file === file);
    if (!c) {
      return [];
    }
    const m = sceneSel[file] ?? {};
    return c.scenes.filter((n) => m[n]);
  }

  // Assemble the import collections array from the current selection. A collection
  // contributes only when checked AND it has at least one scene selected (or no
  // scenes at all). All scenes selected -> omit `scenes` (whole collection); a
  // strict subset -> send that subset.
  function buildCollections(): ImporterImportCollection[] {
    if (!scan) {
      return [];
    }
    const out: ImporterImportCollection[] = [];
    for (const c of scan.collections) {
      if (!collSelected[c.file]) {
        continue;
      }
      if (c.scenes.length === 0) {
        out.push({ file: c.file });
        continue;
      }
      const sel = selectedScenes(c.file);
      if (sel.length === 0) {
        continue;
      }
      if (sel.length === c.scenes.length) {
        out.push({ file: c.file });
      } else {
        out.push({ file: c.file, scenes: sel });
      }
    }
    return out;
  }

  let collections = $derived(buildCollections());
  let canImport = $derived(
    !busy &&
      result == null &&
      (collections.length > 0 || importService || importVideo || importAudio),
  );

  async function runImport() {
    if (!scan) {
      return;
    }
    busy = true;
    try {
      const res = await obs.call("importer.import", {
        path: scan.path,
        collections,
        importService,
        importVideo,
        importAudio,
        importGlobalAudio,
      });
      result = res;
      error = null;
    } catch (e) {
      report(e);
    } finally {
      busy = false;
    }
  }
</script>

<Modal title="Import from OBS Studio" {onClose} width={640}>
  {#if error}<p class="error">{error}</p>{/if}

      {#if !loaded}
        <p class="dim">Scanning for OBS Studio…</p>
      {:else if !scan || !scan.found}
        <p class="dim">OBS Studio not found.</p>
        <p class="dim note">Pick the OBS Studio config folder (the one containing <code>basic</code>).</p>
        <button class="btn" disabled={scanning} onclick={() => void browse()}>Browse…</button>
      {:else if result}
        <p class="ok">Imported {result.imported.collections} collection{result.imported.collections === 1 ? "" : "s"}.</p>
        <ul class="summary">
          {#if result.imported.service}<li>Stream destination imported.</li>{/if}
          {#if result.imported.video}<li>Video settings imported.</li>{/if}
          {#if result.imported.audio}<li>Audio settings imported.</li>{/if}
        </ul>
        {#if result.warnings.length > 0}
          <p class="dim note">Warnings:</p>
          <ul class="warnings">
            {#each result.warnings as w, i (i)}
              <li class="dim">{w}</li>
            {/each}
          </ul>
        {/if}
      {:else}
        <p class="dim note">Source: <span class="path" title={scan.path}>{scan.path}</span></p>

        <h4 class="section">Scene Collections</h4>
        {#if scan.collections.length === 0}
          <p class="dim">No scene collections found.</p>
        {:else}
          <ul class="rows">
            {#each scan.collections as c (c.file)}
              <li class="row">
                <div class="coll-head">
                  <label class="check grow">
                    <input
                      type="checkbox"
                      checked={collSelected[c.file]}
                      onchange={(e) => toggleColl(c.file, e.currentTarget.checked)}
                    />
                    <span class="name" title={c.name}>{c.name}</span>
                    <span class="count">{c.scenes.length} scene{c.scenes.length === 1 ? "" : "s"}</span>
                  </label>
                  {#if c.scenes.length > 0}
                    <button class="btn ghost xs" onclick={() => toggleExpand(c.file)}>
                      {expanded[c.file] ? "Hide" : "Scenes"}
                    </button>
                  {/if}
                </div>
                {#if expanded[c.file] && c.scenes.length > 0}
                  <ul class="scenes" class:dimmed={!collSelected[c.file]}>
                    {#each c.scenes as name (name)}
                      <li>
                        <label class="check">
                          <input
                            type="checkbox"
                            disabled={!collSelected[c.file]}
                            checked={sceneSel[c.file]?.[name] ?? false}
                            onchange={(e) => toggleScene(c.file, name, e.currentTarget.checked)}
                          />
                          {name}
                        </label>
                      </li>
                    {/each}
                  </ul>
                {/if}
              </li>
            {/each}
          </ul>
        {/if}

        {#if scan.service || scan.video || scan.audio || scan.collections.length}
          <h4 class="section">Settings</h4>
          {#if scan.service}
            <label class="check">
              <input type="checkbox" checked={importService} onchange={(e) => (importService = e.currentTarget.checked)} />
              Stream destination ({scan.service.label})
            </label>
          {/if}
          {#if scan.video}
            <label class="check">
              <input type="checkbox" checked={importVideo} onchange={(e) => (importVideo = e.currentTarget.checked)} />
              Video settings ({scan.video.baseWidth}×{scan.video.baseHeight} @ {scan.video.fps} fps)
            </label>
          {/if}
          {#if scan.audio}
            <label class="check">
              <input type="checkbox" checked={importAudio} onchange={(e) => (importAudio = e.currentTarget.checked)} />
              Audio settings ({scan.audio.sampleRate} Hz, {scan.audio.channels})
            </label>
          {/if}
          {#if scan.collections.length}
            <label class="check">
              <input
                type="checkbox"
                checked={importGlobalAudio}
                onchange={(e) => (importGlobalAudio = e.currentTarget.checked)}
              />
              Desktop &amp; Mic audio devices, with their filters
            </label>
          {/if}
        {/if}

        <p class="dim note small">Reads your OBS Studio data read-only; nothing in OBS is modified.</p>
        <p class="dim note small">Stream destinations are imported as profiles; wire them to canvases in Outputs.</p>
      {/if}

  {#snippet footer()}
    {#if scan && scan.found && !result}
      <button class="btn ghost" disabled={scanning} onclick={() => void browse()}>Browse…</button>
      <button class="btn" disabled={!canImport} onclick={() => void runImport()}>
        {busy ? "Importing…" : "Import"}
      </button>
    {/if}
    <button class="btn ghost" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .section {
    margin: 14px 0 8px;
    font-size: 10px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-muted);
    font-weight: 600;
  }
  .section:first-of-type {
    margin-top: 4px;
  }

  .rows {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .row {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .coll-head {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .check {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    color: var(--color-text);
    cursor: pointer;
  }
  .check.grow {
    flex: 1;
    min-width: 0;
  }
  .name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .count {
    color: var(--color-muted);
    font-size: 10px;
    white-space: nowrap;
  }
  .scenes {
    list-style: none;
    margin: 0;
    padding: 4px 0 2px 20px;
    display: flex;
    flex-direction: column;
    gap: 3px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .scenes.dimmed {
    opacity: 0.5;
  }

  .summary,
  .warnings {
    list-style: none;
    margin: 0 0 8px;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 3px;
    font-size: 11px;
    color: var(--color-text);
  }
  .warnings li {
    font-size: 10px;
  }

  .path {
    font-family: var(--font-mono, monospace);
    color: var(--color-muted);
  }

  .btn {
    height: auto;
    padding: 5px 10px;
    font-family: var(--font-ui);
    font-size: 11px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    white-space: nowrap;
  }
  .btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn:disabled {
    color: var(--color-muted);
    cursor: default;
  }
  .btn.ghost {
    background: none;
  }
  .btn.xs {
    padding: 2px 7px;
    font-size: 10px;
  }

  .dim {
    color: var(--color-muted);
    margin: 0;
    padding: 4px 0;
    font-size: 11px;
  }
  .note {
    padding: 0 0 8px;
  }
  .small {
    font-size: 10px;
    padding: 0 0 4px;
  }
  .ok {
    color: var(--color-text);
    margin: 0 0 8px;
    font-size: 12px;
    font-weight: 600;
  }
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
  code {
    font-family: var(--font-mono, monospace);
    font-size: 10px;
    color: var(--color-text);
  }
</style>
