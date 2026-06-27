<script lang="ts">
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

  async function load() {
    try {
      rows = await obs.call("sources.findMissing");
      // Seed each row's edit field with its original path so the user can edit just
      // the broken segment instead of retyping the whole path.
      const seeded: Record<string, string> = {};
      for (const r of rows) {
        seeded[r.source] = newPaths[r.source] ?? r.originalPath;
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
        defaultPath: newPaths[row.source] || row.originalPath || undefined,
      });
      if (path != null) {
        newPaths = { ...newPaths, [row.source]: path };
      }
    } catch (e) {
      report(e);
    }
  }

  async function relink(row: MissingFile) {
    const newPath = (newPaths[row.source] ?? "").trim();
    if (!newPath) {
      return;
    }
    busy = row.source;
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

  function onKeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      onClose();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Missing Files">
    <header class="modal-head">
      <h3>Missing Files</h3>
      <button class="icon close" title="Close" onclick={onClose}>✕</button>
    </header>

    <div class="modal-body">
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
          {#each rows as row (row.source)}
            <li class="row">
              <div class="meta">
                <span class="name" title={row.source}>{row.source}</span>
                <span class="orig" title={row.originalPath}>{row.originalPath}</span>
              </div>
              <div class="edit">
                <input
                  type="text"
                  placeholder="new file path"
                  value={newPaths[row.source] ?? ""}
                  oninput={(e) => (newPaths = { ...newPaths, [row.source]: e.currentTarget.value })}
                />
                <button class="btn ghost" onclick={() => void locate(row)}>Locate…</button>
                <button
                  class="btn"
                  disabled={busy === row.source || !(newPaths[row.source] ?? "").trim()}
                  onclick={() => void relink(row)}>Relink</button
                >
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>

    <footer class="modal-foot">
      <button class="btn ghost" onclick={onClose}>Close</button>
    </footer>
  </div>
</div>

<style>
  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.55);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 100;
    padding: 24px;
  }
  .modal {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    width: min(640px, 100%);
    max-height: 86vh;
    display: flex;
    flex-direction: column;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
    font-family: var(--font-ui);
  }
  .modal-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 8px 11px;
    background: var(--color-surface);
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .modal-head h3 {
    margin: 0;
    font-size: 11px;
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    color: var(--color-text);
    font-weight: 600;
  }
  .modal-body {
    padding: 12px;
    overflow: auto;
  }
  .modal-foot {
    display: flex;
    justify-content: flex-end;
    padding: 8px 11px;
    border-top: var(--border-weight) solid var(--color-border);
  }

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
  input[type="text"] {
    flex: 1;
    min-width: 0;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 11px;
    padding: 4px 6px;
  }
  input[type="text"]:focus {
    outline: none;
    border-color: var(--color-accent);
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

  .icon {
    background: none;
    border: none;
    color: var(--color-muted);
    cursor: pointer;
    padding: 2px 4px;
    font-size: 13px;
    line-height: 1;
    height: auto;
  }
  .icon:hover {
    color: var(--color-text);
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
