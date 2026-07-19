<script lang="ts">
  // Autocomplete destination picker: lists stream profiles the way the Streams
  // list does (avatar + name + platform/destination), so the user can tell one
  // destination apart from another instead of reading a bare label. Selection UI
  // only — the caller owns what happens onSelect (the bind action). The option-row
  // rendering (avatar / name / platform label + color) is defined ONCE here.
  import Avatar from "$lib/ui/Avatar.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS } from "$lib/theme/platformColors";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import type { StreamProfileInfo } from "$lib/api/bridge";

  interface Props {
    profiles: StreamProfileInfo[];
    /** Profile uuids to omit (e.g. already bound to this canvas). */
    hideUuids?: string[];
    onSelect: (profileUuid: string) => void;
    placeholder?: string;
  }
  let { profiles, hideUuids = [], onSelect, placeholder = "Search destinations…" }: Props = $props();

  // Populate oauth statuses so a profile's linked channel avatar resolves the same way
  // the Streams list does (ref-counted; free when another consumer already subscribes).
  $effect(() => oauthStore.subscribe());

  // The real channel avatar for a profile's linked account, or "" to fall back to the
  // monogram. Same account-id resolution as the Streams rows (one accessor, no drift).
  function avatarFor(p: StreamProfileInfo): string {
    return oauthStore.connectedStatusForAccount(p.accountId)?.avatarUrl ?? "";
  }

  let query = $state("");
  let open = $state(false);
  let active = $state(0);
  let rootEl = $state<HTMLDivElement | null>(null);

  const hidden = $derived(new Set(hideUuids));

  // Robust display name: label -> platform -> a literal, so a fresh profile with an
  // empty label still names itself.
  function profileName(p: StreamProfileInfo): string {
    return p.label?.trim() || p.platform?.trim() || "Untitled profile";
  }
  // Brand accent for the profile's platform; muted for custom/WHIP with no brand color.
  function platformColor(p: StreamProfileInfo): string {
    return PLATFORM_COLORS[p.platform.trim().toLowerCase()] ?? "var(--color-muted)";
  }
  // The destination detail line: the full service string (e.g. "YouTube - RTMPS"),
  // falling back to the brand label or the raw platform.
  function platformLabel(p: StreamProfileInfo): string {
    const key = p.platform.trim().toLowerCase();
    return p.serviceLabel?.trim() || PLATFORM_LABELS[key] || p.platform || "Unknown";
  }

  const filtered = $derived.by(() => {
    const q = query.trim().toLowerCase();
    const base = profiles.filter((p) => !hidden.has(p.uuid));
    if (!q) {
      return base;
    }
    return base.filter(
      (p) =>
        profileName(p).toLowerCase().includes(q) ||
        p.platform.toLowerCase().includes(q) ||
        p.serviceLabel.toLowerCase().includes(q),
    );
  });

  // Keep the highlighted index inside the (shrinking) filtered list.
  $effect(() => {
    if (active > filtered.length - 1) {
      active = Math.max(0, filtered.length - 1);
    }
  });

  function choose(p: StreamProfileInfo): void {
    onSelect(p.uuid);
    query = "";
    open = false;
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "ArrowDown") {
      e.preventDefault();
      open = true;
      active = Math.min(active + 1, filtered.length - 1);
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      active = Math.max(active - 1, 0);
    } else if (e.key === "Enter") {
      if (open && filtered[active]) {
        e.preventDefault();
        choose(filtered[active]);
      }
    } else if (e.key === "Escape") {
      if (open) {
        e.preventDefault();
        open = false;
      }
    }
  }

  // Click-outside closes the list.
  $effect(() => {
    if (!open) {
      return;
    }
    const onDoc = (e: MouseEvent): void => {
      if (rootEl && !rootEl.contains(e.target as Node)) {
        open = false;
      }
    };
    document.addEventListener("mousedown", onDoc);
    return () => document.removeEventListener("mousedown", onDoc);
  });
</script>

<div class="ps" bind:this={rootEl}>
  <input
    class="ps-input"
    type="text"
    role="combobox"
    aria-expanded={open}
    aria-controls="ps-list"
    aria-autocomplete="list"
    aria-activedescendant={open && filtered[active] ? `ps-opt-${filtered[active].uuid}` : undefined}
    {placeholder}
    bind:value={query}
    onfocus={() => (open = true)}
    onclick={() => (open = true)}
    oninput={() => (open = true)}
    onkeydown={onKeydown}
  />
  {#if open}
    <div class="ps-list" id="ps-list" role="listbox">
      {#if filtered.length === 0}
        <p class="ps-empty">No matching destinations</p>
      {:else}
        {#each filtered as p, i (p.uuid)}
          <button
            type="button"
            id="ps-opt-{p.uuid}"
            class="ps-opt"
            class:active={i === active}
            role="option"
            aria-selected={i === active}
            onmouseenter={() => (active = i)}
            onclick={() => choose(p)}
          >
            <span class="ps-av"><Avatar url={avatarFor(p)} name={profileName(p)} size={26} /></span>
            <span class="ps-text">
              <span class="ps-name">{profileName(p)}</span>
              <span class="ps-sub">
                <span class="ps-dot" style:background={platformColor(p)}></span>
                <span class="ps-plat">{platformLabel(p)}</span>
              </span>
            </span>
          </button>
        {/each}
      {/if}
    </div>
  {/if}
</div>

<style>
  .ps {
    position: relative;
    width: 100%;
  }
  .ps-input {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font: inherit;
    font-size: 13px;
    padding: 7px 10px;
  }
  .ps-input:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  .ps-list {
    position: absolute;
    z-index: 20;
    top: calc(100% + 4px);
    left: 0;
    right: 0;
    max-height: 232px;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 5px;
    padding: 6px;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
  }
  .ps-empty {
    margin: 0;
    padding: 10px;
    font-family: var(--font-mono);
    font-size: 11px;
    color: var(--color-muted);
  }
  .ps-opt {
    display: flex;
    align-items: center;
    gap: 9px;
    width: 100%;
    text-align: left;
    padding: 8px 10px;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    border-left: 3px solid transparent;
    color: var(--color-text);
    cursor: pointer;
    font: inherit;
  }
  .ps-opt.active {
    border-color: color-mix(in srgb, var(--color-accent) 45%, var(--color-border));
    border-left-color: var(--color-accent);
    background: color-mix(in srgb, var(--color-accent) 10%, transparent);
  }
  .ps-av {
    display: inline-flex;
    flex: 0 0 auto;
  }
  .ps-text {
    display: flex;
    flex-direction: column;
    gap: 3px;
    min-width: 0;
  }
  .ps-name {
    font-family: var(--font-ui);
    font-size: 13px;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ps-sub {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .ps-dot {
    width: 7px;
    height: 7px;
    flex: 0 0 auto;
  }
  .ps-plat {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
