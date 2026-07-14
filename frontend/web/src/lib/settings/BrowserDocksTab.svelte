<script lang="ts">
  import { browserDockStore } from "$lib/stores/browserDockStore.svelte";

  // Manager for the user-defined browser docks (Task 12). Each {title,url} becomes
  // a Dockview panel hosting an <iframe> on the Studio page; the store persists the
  // set to browser_docks.json and the reconciler opens/closes panels to match.
  // Live-apply: add/edit/remove mutate the store, which persists immediately.

  void browserDockStore.load();

  let newTitle = $state("");
  let newUrl = $state("");

  // Inline edit: the id being edited (null = none) + its draft fields.
  let editingId = $state<string | null>(null);
  let editTitle = $state("");
  let editUrl = $state("");

  let canAdd = $derived(newUrl.trim().length > 0);

  async function add(): Promise<void> {
    if (!canAdd) return;
    const title = newTitle.trim() || newUrl.trim();
    await browserDockStore.add({ title, url: newUrl.trim() });
    newTitle = "";
    newUrl = "";
  }

  function startEdit(id: string, title: string, url: string): void {
    editingId = id;
    editTitle = title;
    editUrl = url;
  }

  function cancelEdit(): void {
    editingId = null;
  }

  async function saveEdit(id: string): Promise<void> {
    if (editUrl.trim().length === 0) return;
    await browserDockStore.update(id, { title: editTitle.trim() || editUrl.trim(), url: editUrl.trim() });
    editingId = null;
  }

  async function remove(id: string): Promise<void> {
    if (editingId === id) editingId = null;
    await browserDockStore.remove(id);
  }
</script>

<section class="group">
  <h4>Add a Browser Dock</h4>
  <p class="dim hint">
    Each entry opens as a panel on the Studio page rendering the URL in an embedded frame. Some sites block embedding
    (X-Frame-Options / CSP) — full platform dashboards often won't load, but chat/widget popout URLs (e.g.
    <code>twitch.tv/embed/&lt;channel&gt;/chat?parent=localhost</code>, StreamElements/Streamlabs widgets) embed fine.
  </p>

  <div class="addform">
    <input class="in" type="text" placeholder="Title (optional)" bind:value={newTitle} />
    <input
      class="in"
      type="text"
      placeholder="https://…"
      bind:value={newUrl}
      onkeydown={(e) => {
        if (e.key === "Enter") void add();
      }}
    />
    <button class="btn primary" disabled={!canAdd} onclick={() => void add()}>Add</button>
  </div>
</section>

<section class="group">
  <h4>Current Browser Docks</h4>
  {#if browserDockStore.docks.length === 0}
    <p class="dim">No browser docks yet. Add one above.</p>
  {:else}
    <ul class="list">
      {#each browserDockStore.docks as d (d.id)}
        <li class="row">
          {#if editingId === d.id}
            <div class="editform">
              <input class="in" type="text" placeholder="Title" bind:value={editTitle} />
              <input class="in" type="text" placeholder="https://…" bind:value={editUrl} />
              <button class="btn primary" disabled={editUrl.trim().length === 0} onclick={() => void saveEdit(d.id)}>
                Save
              </button>
              <button class="btn" onclick={cancelEdit}>Cancel</button>
            </div>
          {:else}
            <div class="meta">
              <span class="rtitle">{d.title}</span>
              <span class="rurl">{d.url}</span>
            </div>
            <div class="actions">
              <button class="btn" onclick={() => startEdit(d.id, d.title, d.url)}>Edit</button>
              <button class="btn danger" onclick={() => void remove(d.id)}>Remove</button>
            </div>
          {/if}
        </li>
      {/each}
    </ul>
  {/if}
</section>

<style>
  .group {
    padding: 12px 0;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .group:last-child {
    border-bottom: none;
  }
  .group h4 {
    margin: 0 0 10px;
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-dim);
  }
  .hint {
    font-size: 12px;
    line-height: 1.5;
    margin: 0 0 12px;
    max-width: 560px;
  }
  .hint code {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-text);
  }
  .addform,
  .editform {
    display: flex;
    gap: 8px;
    align-items: center;
  }
  .addform .in {
    flex: 1;
    min-width: 0;
  }
  .addform .in:first-child {
    flex: 0 0 200px;
  }
  .editform {
    flex: 1;
  }
  .editform .in {
    flex: 1;
    min-width: 0;
  }
  .in {
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    border-radius: 0;
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
  }
  .in:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .btn {
    flex: 0 0 auto;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    border-radius: 0;
    color: var(--color-text);
    font: inherit;
    font-size: 12px;
    padding: 7px 14px;
    cursor: pointer;
  }
  .btn:hover:not(:disabled) {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .btn:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .btn.primary {
    background: var(--color-accent);
    color: var(--color-accent-ink);
    border-color: var(--color-accent);
  }
  .btn.primary:hover:not(:disabled) {
    color: var(--color-accent-ink);
  }
  .btn.danger:hover:not(:disabled) {
    border-color: var(--color-live);
    color: var(--color-live);
  }
  .list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 8px 10px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .meta {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .rtitle {
    font-size: 13px;
    color: var(--color-text);
  }
  .rurl {
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .actions {
    flex: 0 0 auto;
    display: flex;
    gap: 8px;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
</style>
