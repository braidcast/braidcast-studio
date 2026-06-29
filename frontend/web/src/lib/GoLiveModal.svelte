<script lang="ts">
  import {
    obs,
    type OAuthProvider,
    type OAuthProviderField,
    type OAuthStatus,
    type OutputBindingInfo,
    type StreamProfileInfo,
  } from "./bridge";
  import { goLiveModal, closeGoLiveModal } from "./goLiveModalOpener.svelte";
  import { showToast } from "./toastStore.svelte";
  import GoLiveFieldInput from "./GoLiveFieldInput.svelte";

  // The modal renders ENTIRELY from oauth.providers capability descriptors. Field
  // dispatch lives in GoLiveFieldInput, keyed by descriptor `type` (text/textarea/
  // tags/category/image/enum/bool/labelset) — never by platform id — so a new
  // provider (or a new field type) renders with zero changes here.

  // One armed destination: a stream profile bound via an enabled output binding.
  // `connected` (provider resolved + OAuth linked) gates the full card vs the muted
  // key-only card.
  interface Dest {
    profileUuid: string;
    profile: StreamProfileInfo;
    provider: OAuthProvider | null;
    status: OAuthStatus | null;
    connected: boolean;
  }

  let providers = $state<OAuthProvider[]>([]);
  let statuses = $state<OAuthStatus[]>([]);
  let bindings = $state<OutputBindingInfo[]>([]);
  let profiles = $state<StreamProfileInfo[]>([]);
  let loaded = $state(false);
  let isLive = $state(false);
  let submitting = $state(false);
  let view = $state<"simple" | "advanced">("simple");

  // Shared defaults every destination inherits (mock "Shared defaults" block),
  // keyed by field key. Driven by the union of connected providers' shareable
  // fields (see `sharedFields`), so any shareable field — not just title/tags —
  // gets a shared source.
  let shared = $state<Record<string, unknown>>({});

  // Per-destination overrides, keyed by profile uuid then field key. An empty
  // shareable field inherits the shared value; a non-shareable field stands alone.
  let perDest = $state<Record<string, Record<string, unknown>>>({});

  function setField(uuid: string, key: string, val: unknown): void {
    perDest[uuid] = { ...(perDest[uuid] ?? {}), [key]: val };
  }
  function getVal(uuid: string, key: string): unknown {
    return perDest[uuid]?.[key];
  }

  // "Empty" per descriptor type — the inheritance/omission predicate. A bool that
  // has been set (even to false) counts as present; everything else is empty when
  // blank/missing.
  function isEmptyVal(type: string, v: unknown): boolean {
    switch (type) {
      case "tags":
      case "labelset":
        return !Array.isArray(v) || v.length === 0;
      case "category":
        return v == null;
      case "bool":
        return v === undefined || v === null;
      default:
        return typeof v !== "string" || v.trim() === "";
    }
  }

  // Human-readable shared value for a field's inherit ghost (any shareable key).
  function sharedGhostText(f: OAuthProviderField): string {
    const v = shared[f.key];
    if (f.type === "tags" || f.type === "labelset") {
      return Array.isArray(v) ? v.join(", ") : "";
    }
    if (f.type === "category") {
      return v && typeof v === "object" ? ((v as { name?: string }).name ?? "") : "";
    }
    return typeof v === "string" ? v : "";
  }

  // Field grouping (data lists, not branches): simple shareable render as overrides
  // (ghost/amber), simple non-shareable render normally, advanced go under the
  // dashed "<Platform>-only" divider.
  function simpleShareable(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier !== "advanced" && f.shareable);
  }
  function simpleNonShareable(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier !== "advanced" && !f.shareable);
  }
  function advancedFields(p: OAuthProvider): OAuthProviderField[] {
    return p.fields.filter((f) => f.tier === "advanced");
  }

  function isOverridden(uuid: string, f: OAuthProviderField): boolean {
    return !isEmptyVal(f.type, perDest[uuid]?.[f.key]);
  }
  function overrideChip(d: Dest): string {
    if (!d.provider) {
      return "inherits shared";
    }
    const hit = simpleShareable(d.provider).find((f) => isOverridden(d.profileUuid, f));
    return hit ? hit.label.toLowerCase() + " overridden" : "inherits shared";
  }

  // Resolve a profile's provider: prefer the linked account's providerId, else match
  // the display platform against a provider id/displayName (mirrors StreamsTab).
  function resolveProvider(p: StreamProfileInfo, status: OAuthStatus | null): OAuthProvider | null {
    if (status?.connected) {
      const byId = providers.find((pv) => pv.id === status.providerId);
      if (byId) {
        return byId;
      }
    }
    const plat = (p.platform || "").trim().toLowerCase();
    if (!plat) {
      return null;
    }
    return providers.find((pv) => pv.id.toLowerCase() === plat || pv.displayName.toLowerCase() === plat) ?? null;
  }

  // Armed destinations: enabled bindings -> their profile, deduped by profile.
  const armed = $derived.by<Dest[]>(() => {
    const seen = new Set<string>();
    const out: Dest[] = [];
    for (const b of bindings) {
      if (!b.enabled || !b.profileUuid || seen.has(b.profileUuid)) {
        continue;
      }
      const profile = profiles.find((p) => p.uuid === b.profileUuid);
      if (!profile) {
        continue;
      }
      seen.add(b.profileUuid);
      const status = statuses.find((s) => s.profileUuid === b.profileUuid) ?? null;
      const provider = resolveProvider(profile, status);
      out.push({ profileUuid: b.profileUuid, profile, provider, status, connected: !!(provider && status?.connected) });
    }
    return out;
  });
  const connectedDests = $derived(armed.filter((d) => d.connected));

  // Shared-defaults descriptor: the UNION of every shareable field across connected
  // providers, deduped by key (first provider's label/type wins). Drives the shared
  // block so a provider marking a new field shareable gets a shared source with no
  // edits here.
  const sharedFields = $derived.by<OAuthProviderField[]>(() => {
    const seen = new Set<string>();
    const out: OAuthProviderField[] = [];
    for (const d of connectedDests) {
      if (!d.provider) {
        continue;
      }
      for (const f of d.provider.fields) {
        if (f.shareable && !seen.has(f.key)) {
          seen.add(f.key);
          out.push(f);
        }
      }
    }
    return out;
  });

  const primaryLabel = $derived(
    goLiveModal.mode === "golive" ? "Go Live now ▸" : isLive ? "Update info" : "Save info",
  );
  const footerNote = $derived(
    goLiveModal.mode === "golive"
      ? "Metadata pushed to each platform, then the stream starts. A failure on one platform won't block going live."
      : "Metadata is pushed to each connected platform. A failure on one won't affect the others.",
  );

  // Resolve effective values per the inheritance rule and push them. Shareable empty
  // -> shared value (omitted if the shared value is also empty); non-shareable -> its
  // own value (omitted when empty). Empty fields are never emitted, so a provider
  // that treats "present" as "set" can't blank a channel by inheriting nothing.
  function effectiveFields(d: Dest): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    if (!d.provider) {
      return out;
    }
    const pd = perDest[d.profileUuid] ?? {};
    for (const f of d.provider.fields) {
      const v = pd[f.key];
      if (f.shareable) {
        if (!isEmptyVal(f.type, v)) {
          out[f.key] = v;
        } else if (!isEmptyVal(f.type, shared[f.key])) {
          out[f.key] = shared[f.key];
        }
      } else if (!isEmptyVal(f.type, v)) {
        out[f.key] = v;
      }
    }
    return out;
  }

  // Best-effort prefill (fired, not awaited, so a slow get never blocks the open):
  // seed each connected destination's category + seed the shared title from the
  // first platform that reports one. Per-destination titles stay empty (inherit).
  async function prefill(): Promise<void> {
    await Promise.all(
      connectedDests.map(async (d) => {
        try {
          const m = await obs.call("streamMeta.get", { profileUuid: d.profileUuid });
          if (m.category?.id) {
            setField(d.profileUuid, "category", { id: m.category.id, name: m.category.name });
          }
          if (m.title && !shared.title) {
            shared.title = m.title;
          }
        } catch {
          // Ignore: prefill is best-effort.
        }
      }),
    );
  }

  $effect(() => {
    let active = true;
    Promise.all([
      obs.call("oauth.providers").catch(() => [] as OAuthProvider[]),
      obs.call("oauth.status").catch(() => [] as OAuthStatus[]),
      obs.call("outputBinding.list").catch(() => [] as OutputBindingInfo[]),
      obs.call("streamProfile.list").catch(() => [] as StreamProfileInfo[]),
      obs.call("getStreamingState").catch(() => ({ active: false })),
    ]).then(([prov, stat, binds, profs, st]) => {
      if (!active) {
        return;
      }
      providers = prov;
      statuses = stat;
      bindings = binds;
      profiles = profs;
      isLive = !!st.active;
      loaded = true;
      void prefill();
    });
    const off = obs.on("streaming.changed", (p) => (isLive = p.active));
    return () => {
      active = false;
      off();
    };
  });

  async function confirm(): Promise<void> {
    if (submitting) {
      return;
    }
    submitting = true;
    const targets = connectedDests;
    const results = await Promise.allSettled(
      targets.map((d) => obs.call("streamMeta.set", { profileUuid: d.profileUuid, fields: effectiveFields(d) })),
    );
    results.forEach((r, i) => {
      if (r.status === "rejected") {
        const name = targets[i].profile.label || targets[i].provider?.displayName || "destination";
        showToast("Stream info failed: " + name, (r.reason as Error)?.message ?? "metadata push failed");
      }
    });
    if (goLiveModal.mode === "golive") {
      try {
        await obs.call("streaming.start");
      } catch (e) {
        showToast("Go Live failed", (e as Error).message);
        submitting = false;
        return;
      }
    }
    submitting = false;
    closeGoLiveModal();
  }

  function onKeydown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      closeGoLiveModal();
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div
  class="modal-backdrop"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) closeGoLiveModal();
  }}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label="Stream Information">
    <div class="mh">
      <span class="t">● Stream Information</span>
      <span class="seg">
        <button class="cell" class:on={view === "simple"} onclick={() => (view = "simple")}>Simple</button>
        <button class="cell" class:on={view === "advanced"} onclick={() => (view = "advanced")}>Advanced</button>
      </span>
    </div>

    <div class="mb">
      {#if !loaded}
        <p class="note">Loading destinations…</p>
      {:else}
        <!-- Shared defaults: the union of connected providers' shareable fields
             (mock `.shared`), present in both modes whenever anything is shareable. -->
        {#if sharedFields.length}
          <div class="shared">
            <p class="eh tight">Shared defaults</p>
            {#each sharedFields as f, i (f.key)}
              <div class="fld" class:last={i === sharedFields.length - 1}>
                <span class="fl">{f.label}</span>
                <GoLiveFieldInput field={f} value={shared[f.key]} onChange={(v) => (shared[f.key] = v)} />
              </div>
            {/each}
          </div>
        {/if}

        {#if armed.length === 0}
          <p class="note">
            No armed destinations. Enable a destination on a canvas to push stream information.
          </p>
        {:else if view === "advanced"}
          <p class="eh">Per-destination (overrides shared when filled)</p>

          {#each armed as d (d.profileUuid)}
            {#if d.connected && d.provider}
              <div class="dest">
                <div class="dh">
                  <span class="pdot" style:background={d.provider.brandColor || "var(--color-accent)"}></span>
                  <span class="pname">{d.provider.displayName}</span>
                  <span class="pacct">· {d.profile.label}</span>
                  {#if overrideChip(d) === "inherits shared"}
                    <span class="inh">inherits shared</span>
                  {:else}
                    <span class="ovrflag">{overrideChip(d)}</span>
                  {/if}
                </div>
                <div class="body">
                  <!-- Shareable simple fields render as overrides (ghost / amber). -->
                  {#each simpleShareable(d.provider) as f (f.key)}
                    {@const filled = isOverridden(d.profileUuid, f)}
                    <div class="fld">
                      <span class="fl">{f.label}</span>
                      <GoLiveFieldInput
                        field={f}
                        value={getVal(d.profileUuid, f.key)}
                        providerId={d.provider.id}
                        ghostText={sharedGhostText(f)}
                        accent={filled}
                        onChange={(v) => setField(d.profileUuid, f.key, v)}
                      />
                      {#if filled}
                        <div class="hint acc">overrides shared for {d.provider.displayName}</div>
                      {:else}
                        <div class="hint">empty → using shared {f.label.toLowerCase()}</div>
                      {/if}
                    </div>
                  {/each}

                  <!-- Non-shareable simple fields render normally. -->
                  {#if simpleNonShareable(d.provider).length}
                    <div class="drow">
                      {#each simpleNonShareable(d.provider) as f (f.key)}
                        <div class="fld last">
                          <span class="fl">{f.label}</span>
                          <GoLiveFieldInput
                            field={f}
                            value={getVal(d.profileUuid, f.key)}
                            providerId={d.provider.id}
                            onChange={(v) => setField(d.profileUuid, f.key, v)}
                          />
                        </div>
                      {/each}
                    </div>
                  {/if}

                  <!-- Advanced / platform-only fields under the dashed divider. -->
                  <div class="adv">
                    <div class="advlbl">{d.provider.displayName}-only</div>
                    {#if advancedFields(d.provider).length === 0}
                      <p class="note">
                        {d.provider.displayName}'s API exposes only title / category / tags — nothing extra to show.
                      </p>
                    {:else}
                      {#each advancedFields(d.provider) as f (f.key)}
                        {#if f.type === "bool"}
                          <div class="togrow">
                            <GoLiveFieldInput
                              field={f}
                              value={getVal(d.profileUuid, f.key)}
                              onChange={(v) => setField(d.profileUuid, f.key, v)}
                            />
                            {f.label}
                          </div>
                        {:else}
                          <div class="fld last advfld">
                            <span class="fl">{f.label}</span>
                            <GoLiveFieldInput
                              field={f}
                              value={getVal(d.profileUuid, f.key)}
                              providerId={d.provider.id}
                              narrow={f.type === "enum"}
                              onChange={(v) => setField(d.profileUuid, f.key, v)}
                            />
                          </div>
                        {/if}
                      {/each}
                    {/if}
                  </div>
                </div>
              </div>
            {:else}
              <!-- Key-only / unconnected: muted card, no fields. -->
              <div class="dest keyonly">
                <div class="dh">
                  <span class="pdot" style:background="var(--color-muted)"></span>
                  <span class="pname">{d.profile.platform || d.profile.label}</span>
                  <span class="pacct">· {d.profile.label}</span>
                </div>
                <div class="body">
                  <p class="note">Key-only profile — no metadata API. Streams as-is.</p>
                </div>
              </div>
            {/if}
          {/each}
        {:else}
          <!-- Simple mode: shared block + only non-shareable per-platform fields. -->
          {#each armed as d (d.profileUuid)}
            {#if d.connected && d.provider}
              <div class="simple-dest">
                <div class="sd-head">
                  <span class="pdot" style:background={d.provider.brandColor || "var(--color-accent)"}></span>
                  <span class="pname">{d.provider.displayName}</span>
                  <span class="pacct">· {d.profile.label}</span>
                </div>
                {#if simpleNonShareable(d.provider).length}
                  <div class="drow">
                    {#each simpleNonShareable(d.provider) as f (f.key)}
                      <div class="fld last">
                        <span class="fl">{f.label}</span>
                        <GoLiveFieldInput
                          field={f}
                          value={getVal(d.profileUuid, f.key)}
                          providerId={d.provider.id}
                          onChange={(v) => setField(d.profileUuid, f.key, v)}
                        />
                      </div>
                    {/each}
                  </div>
                {:else}
                  <p class="note">Uses the shared defaults.</p>
                {/if}
              </div>
            {:else}
              <div class="simple-dest keyonly">
                <div class="sd-head">
                  <span class="pdot" style:background="var(--color-muted)"></span>
                  <span class="pname">{d.profile.platform || d.profile.label}</span>
                  <span class="pacct">· {d.profile.label} — key-only, streams as-is</span>
                </div>
              </div>
            {/if}
          {/each}
        {/if}
      {/if}
    </div>

    <div class="mf">
      <span class="note">{footerNote}</span>
      <button class="b pri" disabled={submitting || !loaded || armed.length === 0} onclick={() => void confirm()}>
        {submitting ? "Working…" : primaryLabel}
      </button>
    </div>
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
    max-width: 580px;
    width: 100%;
    background: var(--color-surface);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 13px;
    line-height: 1.5;
    display: flex;
    flex-direction: column;
    max-height: 88vh;
    box-shadow: 0 18px 48px rgba(0, 0, 0, 0.5);
  }
  .mh {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 14px 18px;
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .mh .t {
    font-size: 15px;
    font-weight: 600;
    color: var(--color-text);
  }
  .seg {
    display: flex;
    border: var(--border-weight) solid var(--color-border);
  }
  .seg .cell {
    padding: 5px 14px;
    font-size: 12px;
    color: var(--color-dim);
    background: none;
    border: none;
    cursor: pointer;
    font: inherit;
  }
  .seg .cell:hover {
    color: var(--color-text);
  }
  .seg .cell.on {
    background: var(--color-accent);
    color: var(--color-accent-ink);
    font-weight: 600;
  }
  .mb {
    padding: 16px 18px;
    overflow: auto;
  }
  .eh {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-dim);
    margin: 0 0 10px;
  }
  .eh.tight {
    margin-bottom: 8px;
  }
  .fl {
    display: block;
    font-size: 12px;
    color: var(--color-dim);
    margin-bottom: 4px;
  }
  .fld {
    margin-bottom: 10px;
  }
  .fld.last {
    margin-bottom: 0;
  }
  .drow {
    display: flex;
    gap: 10px;
    align-items: flex-start;
    flex-wrap: wrap;
  }
  .drow > * {
    flex: 1;
    min-width: 160px;
  }
  .shared {
    background: var(--color-surface-2);
    border: var(--border-weight) solid var(--color-border);
    padding: 12px;
    margin-bottom: 14px;
  }
  .dest {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    margin-bottom: 10px;
  }
  .dest.keyonly,
  .simple-dest.keyonly {
    opacity: 0.7;
  }
  .dh {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    border-bottom: var(--border-weight) solid var(--color-border);
    background: var(--color-surface-2);
  }
  .pdot {
    width: 8px;
    height: 8px;
    flex: 0 0 auto;
  }
  .pname {
    font-weight: 600;
  }
  .pacct {
    color: var(--color-dim);
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ovrflag {
    margin-left: auto;
    font-size: 10px;
    color: var(--color-accent);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    flex: 0 0 auto;
  }
  .inh {
    margin-left: auto;
    font-size: 10px;
    color: var(--color-muted);
    text-transform: uppercase;
    letter-spacing: 0.05em;
    flex: 0 0 auto;
  }
  .body {
    padding: 12px;
  }
  .hint {
    font-size: 10px;
    color: var(--color-muted);
    margin-top: 3px;
  }
  .hint.acc {
    color: var(--color-accent);
  }
  .adv {
    margin-top: 10px;
    padding-top: 10px;
    border-top: var(--border-weight) dashed var(--color-border);
  }
  .advlbl {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--color-accent);
    margin-bottom: 8px;
  }
  .advfld {
    margin-top: 6px;
  }
  .togrow {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    margin-bottom: 6px;
  }
  .simple-dest {
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-base);
    padding: 12px;
    margin-bottom: 10px;
  }
  .sd-head {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 10px;
  }
  .note {
    font-size: 11px;
    color: var(--color-muted);
    margin: 0;
  }
  .mf {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 14px;
    padding: 14px 18px;
    border-top: var(--border-weight) solid var(--color-border);
  }
  .mf .note {
    flex: 1;
  }
  .b.pri {
    background: var(--color-accent);
    border: var(--border-weight) solid var(--color-accent);
    color: var(--color-accent-ink);
    font-weight: 600;
    padding: 8px 16px;
    font-size: 13px;
    cursor: pointer;
    font-family: var(--font-ui);
    white-space: nowrap;
    flex: 0 0 auto;
  }
  .b.pri:disabled {
    opacity: 0.5;
    cursor: default;
  }
</style>
