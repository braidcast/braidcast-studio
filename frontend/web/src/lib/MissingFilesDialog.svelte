<script lang="ts">
  import Modal from "./Modal.svelte";
  import { obs, type MissingFile } from "./bridge";

  interface Props {
    onClose: () => void;
  }
  let { onClose }: Props = $props();

  // The native preview overlay is suspended by the opener for this dialog's whole
  // lifetime, so it never occludes the modal.

  let rows = $state<MissingFile[]>([]);
  let newPaths = $state<Record<string, string>>({});
  let loaded = $state(false);
  let busy = $state<string | null>(null);
  let error = $state<string | null>(null);

  function report(e: unknown) {
    error = (e as Error).message;
  }

  // One source can own several missing files (e.g. a slideshow/playlist), so rows
  // are keyed by source+path, not source alone — otherwise duplicate keys crash the
  // #each and the edit fields conflate.
  function rowKey(r: MissingFile): string {
    return r.source + " " + r.originalPath;
  }

  async function load() {
    try {
      rows = await obs.call("sources.findMissing");
      // Seed each row's edit field with its original path so the user can edit just
      // the broken segment instead of retyping the whole path.
      const seeded: Record<string, string> = {};
      for (const r of rows) {
        seeded[rowKey(r)] = newPaths[rowKey(r)] ?? r.originalPath;
      }
      newPaths = seeded;
      error = null;
    } catch (e) {
      report(e);
    } finally {
      loaded = true;
    }
  }

  $effect(() => {
    void load();
  });

  // A relink mutates the scene; re-scan when anything changes externally too.
  $effect(() => {
    return obs.on("scenes.changed", () => void load());
  });

  async function locate(row: MissingFile) {
    try {
      const { path } = await obs.call("dialog.openFile", {
        mode: "open",
        defaultPath: newPaths[rowKey(row)] || row.originalPath || undefined,
      });
      if (path != null) {
        newPaths = { ...newPaths, [rowKey(row)]: path };
      }
    } catch (e) {
      report(e);
    }
  }

  async function relink(row: MissingFile) {
    const newPath = (newPaths[rowKey(row)] ?? "").trim();
    if (!newPath) {
      return;
    }
    busy = rowKey(row);
    try {
      await obs.call("sources.relinkMissing", { source: row.source, originalPath: row.originalPath, newPath });
      error = null;
      await load();
    } catch (e) {
      report(e);
    } finally {
      busy = null;
    }
  }
</script>

<Modal title="Missing Files" {onClose} width={640}>
  {#if error}<p class="error">{error}</p>{/if}

  {#if !loaded}
    <p class="dim">Scanning…</p>
  {:else if rows.length === 0}
    <p class="dim">No missing files.</p>
  {:else}
    <p class="dim note">
      Paste or browse to the file's new location, then Relink. There is no drag-and-drop here — edit the path below.
    </p>
    <ul class="rows">
      {#each rows as row (rowKey(row))}
        <li class="row">
          <div class="meta">
            <span class="name" title={row.source}>{row.source}</span>
            <span class="orig" title={row.originalPath}>{row.originalPath}</span>
          </div>
          <div class="edit">
            <input
              type="text"
              placeholder="new file path"
              value={newPaths[rowKey(row)] ?? ""}
              oninput={(e) => (newPaths = { ...newPaths, [rowKey(row)]: e.currentTarget.value })}
            />
            <button onclick={() => void locate(row)}>Locate…</button>
            <button
              class="accent"
              disabled={busy === rowKey(row) || !(newPaths[rowKey(row)] ?? "").trim()}
              onclick={() => void relink(row)}>Relink</button
            >
          </div>
        </li>
      {/each}
    </ul>
  {/if}

  {#snippet footer()}
    <button class="btn" onclick={onClose}>Close</button>
  {/snippet}
</Modal>

<style>
  .rows {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .row {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 8px;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .meta {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 0;
  }
  .name {
    font-size: 11px;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .orig {
    font-size: 10px;
    color: var(--color-muted);
    font-family: var(--font-mono, monospace);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .edit {
    display: flex;
    gap: 6px;
    align-items: center;
  }
  .edit input[type="text"] {
    flex: 1;
    min-width: 0;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  .edit input[type="text"]:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .edit button {
    height: auto;
    padding: 5px 10px;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    white-space: nowrap;
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
  .error {
    color: var(--color-live);
    margin: 0 0 8px;
    font-size: 11px;
  }
</style>
