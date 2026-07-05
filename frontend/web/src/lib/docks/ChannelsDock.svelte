<script lang="ts">
  // One card per connected account: identity (avatar + name + platform dot), the
  // link state (CONNECTED / RECONNECT chip), the audience total with its per-platform
  // label, and live viewers when the account is streaming. Reads the merged
  // channelsStore.rows — never the three underlying feeds directly.
  import { channelsStore } from "../channelsStore.svelte";
  import { streamProfileStore } from "../streamProfileStore.svelte";
  import { openOAuthConnect } from "../oauthConnectOpener.svelte";
  import { PLATFORM_COLORS, PLATFORM_LABELS } from "../theme/platformColors";
  import Avatar from "../Avatar.svelte";
  import type { StreamProfileInfo } from "../bridge";

  // Host supplies tab chrome + strips __* keys; this body declares no props.
  let {}: Record<string, unknown> = $props();

  // The reconnect flow is profile-scoped (openOAuthConnect wants a profileUuid), but
  // a row is account-scoped. Resolve a stream profile bound to this account to bridge
  // the two; several profiles may share an accountId, so the first match is enough.
  // Ensure the shared profile list is loaded (idempotent) so the lookup can resolve.
  streamProfileStore.start();

  function profileForAccount(accountId: string): StreamProfileInfo | undefined {
    return streamProfileStore.profiles.find((p) => p.accountId === accountId);
  }

  function audienceLabel(kind: string): string {
    return kind === "subscribers" ? "Subscribers" : "Followers";
  }

  function audienceText(r: (typeof channelsStore.rows)[number]): string {
    if (r.audienceHidden) {
      return "Hidden";
    }
    if (r.audienceCount < 0) {
      return r.providerId === "kick" ? "— (as of last stream)" : "—";
    }
    // YouTube reports subscriber counts API-rounded, so mark the total approximate.
    const approx = r.audienceKind === "subscribers";
    return (approx ? "~" : "") + r.audienceCount.toLocaleString();
  }

  function platformName(r: (typeof channelsStore.rows)[number]): string {
    return PLATFORM_LABELS[r.providerId] ?? profileForAccount(r.accountId)?.platform ?? r.providerId;
  }

  // Reuse the app-wide OAuth connect modal against the account's bound profile. No
  // bound profile => nothing to relink through, so the chip is disabled (never fires).
  function reconnect(r: (typeof channelsStore.rows)[number]): void {
    const p = profileForAccount(r.accountId);
    if (!p) {
      return;
    }
    openOAuthConnect({ profileUuid: p.uuid, providerId: r.providerId, platformName: platformName(r) });
  }
</script>

<div class="dock-body">
  {#if channelsStore.rows.length === 0}
    <p class="dock-msg">No channels connected.</p>
  {:else}
    <ul class="list">
      {#each channelsStore.rows as r (r.accountId)}
        {@const color = PLATFORM_COLORS[r.providerId] ?? "var(--color-muted)"}
        {@const hasProfile = profileForAccount(r.accountId) !== undefined}
        <li class="card" class:stale={r.needsReconnect} style:--dot={color}>
          <Avatar url={r.avatarUrl} name={r.displayName || r.login} size={36} />
          <div class="ident">
            <div class="name">
              <span class="dot" style:background={color}></span>
              <span class="nm">{r.displayName || r.login}</span>
            </div>
            {#if r.needsReconnect}
              <button
                class="chip warn"
                disabled={!hasProfile}
                title={hasProfile ? "Reconnect this account" : "No stream profile bound to this account"}
                onclick={() => reconnect(r)}
              >
                Reconnect
              </button>
            {:else}
              <span class="chip ok">Connected</span>
            {/if}
          </div>
          <div class="metrics">
            <div class="metric">
              <span class="k">{audienceLabel(r.audienceKind)}</span>
              <span class="v">{audienceText(r)}</span>
            </div>
            {#if r.viewers >= 0}
              <div class="metric">
                <span class="k">Viewers</span>
                <span class="v live">{r.viewers.toLocaleString()}</span>
              </div>
            {/if}
          </div>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .dock-body {
    display: flex;
    flex-direction: column;
  }

  .dock-msg {
    padding: 22px 12px;
    font-family: var(--font-mono);
    font-size: 11px;
    letter-spacing: 0.06em;
    text-transform: var(--label-case);
    color: var(--color-muted);
    text-align: center;
  }

  .list {
    list-style: none;
    margin: 0;
    padding: 6px;
    display: flex;
    flex-direction: column;
    gap: 4px;
  }

  /* Hairline card with a platform-colored left rail, mirroring StatsDock's rows. */
  .card {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px 8px 9px;
    border: var(--border-weight) solid var(--color-border);
    border-left: 2px solid var(--dot);
    background: var(--color-base);
  }
  .card.stale {
    background: var(--color-surface);
  }

  .ident {
    display: flex;
    flex-direction: column;
    gap: 5px;
    min-width: 0;
    flex: 1;
  }
  .name {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .dot {
    width: 7px;
    height: 7px;
    flex-shrink: 0;
  }
  .nm {
    font-size: 12px;
    color: var(--color-text);
    letter-spacing: var(--letter-spacing);
    text-transform: var(--label-case);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .chip {
    align-self: flex-start;
    font-family: var(--font-mono);
    font-size: 8.5px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    padding: 2px 6px;
    border: var(--border-weight) solid var(--color-border);
    background: transparent;
    line-height: 1;
  }
  .chip.ok {
    color: var(--color-ok);
    border-color: var(--color-ok-bg);
  }
  .chip.warn {
    color: var(--color-warn);
    border-color: var(--color-warn);
    cursor: pointer;
  }
  .chip.warn:hover:not(:disabled) {
    color: var(--color-live);
    border-color: var(--color-live);
  }
  .chip.warn:disabled {
    color: var(--color-muted);
    border-color: var(--color-border);
    cursor: not-allowed;
    opacity: 0.7;
  }

  .metrics {
    display: flex;
    align-items: baseline;
    gap: 14px;
    margin-left: auto;
    flex-shrink: 0;
  }
  .metric {
    display: flex;
    flex-direction: column;
    align-items: flex-end;
    gap: 3px;
  }
  .k {
    font-family: var(--font-mono);
    font-size: 8.5px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--color-muted);
    white-space: nowrap;
  }
  .v {
    font-family: var(--font-mono);
    font-size: 15px;
    font-weight: 600;
    line-height: 1;
    color: var(--color-text);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .v.live {
    color: var(--color-live);
  }
</style>
