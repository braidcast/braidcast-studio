<script lang="ts">
  // Chip-style tag editor (mock `.tags` / `.tag`). Holds its own "adding" draft so
  // each instance (shared block + every per-destination override) is independent.
  // When empty and `ghostText` is set, shows the inherited value as italic ghost
  // text (`↳ …`) with a "+ add" affordance to start overriding.
  interface Props {
    values: string[];
    onChange: (next: string[]) => void;
    ghostText?: string;
    accent?: boolean;
  }
  let { values, onChange, ghostText, accent = false }: Props = $props();

  let adding = $state(false);
  let draft = $state("");

  function commit(): void {
    const t = draft.trim();
    if (t && !values.includes(t)) {
      onChange([...values, t]);
    }
    draft = "";
    adding = false;
  }

  function remove(tag: string): void {
    onChange(values.filter((v) => v !== tag));
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "Enter" || e.key === ",") {
      e.preventDefault();
      commit();
    } else if (e.key === "Escape") {
      e.preventDefault();
      draft = "";
      adding = false;
    }
  }

  function startAdd(): void {
    adding = true;
    draft = "";
  }
</script>

{#if values.length === 0 && ghostText && !adding}
  <div class="ghost-line">
    <span class="ghost">↳ {ghostText}</span>
    <button type="button" class="tag add" onclick={startAdd}>+ add</button>
  </div>
{:else}
  <div class="tags" class:accent>
    {#each values as t (t)}
      <span class="tag">
        {t}
        <button type="button" class="x" title="Remove tag" aria-label="Remove tag" onclick={() => remove(t)}>×</button>
      </span>
    {/each}
    {#if adding}
      <!-- svelte-ignore a11y_autofocus -->
      <input
        class="draft"
        type="text"
        bind:value={draft}
        autofocus
        placeholder="tag"
        onkeydown={onKeydown}
        onblur={commit}
      />
    {:else}
      <button type="button" class="tag add" onclick={startAdd}>+ add</button>
    {/if}
  </div>
{/if}

<style>
  .ghost-line {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .ghost {
    font-style: italic;
    color: var(--color-muted);
    font-size: 12px;
  }
  .tags {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
    align-items: center;
  }
  .tag {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    padding: 3px 9px;
    font-size: 12px;
    color: var(--color-dim);
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }
  .tags.accent .tag:not(.add) {
    border-color: var(--color-accent);
    color: var(--color-text);
  }
  .tag.add {
    cursor: pointer;
    color: var(--color-muted);
  }
  .tag.add:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .x {
    background: none;
    border: none;
    color: inherit;
    cursor: pointer;
    font: inherit;
    padding: 0;
    line-height: 1;
  }
  .x:hover {
    color: var(--color-live);
  }
  .draft {
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-text);
    font: inherit;
    font-size: 12px;
    padding: 3px 8px;
    width: 90px;
  }
  .draft:focus {
    outline: none;
  }
</style>
