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
  import Modal from "./Modal.svelte";
  import Segmented from "./Segmented.svelte";
  import Icon from "./dock/Icon.svelte";

  const VIEW_OPTIONS = [
    { label: "Simple", value: "simple" },
    { label: "Advanced", value: "advanced" },
  ];

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
    // Token scopes are stale: the backend refuses streamMeta, so this card is shown
    // muted (like key-only) and excluded from the push — it still goes live via key.
    needsReconnect: boolean;
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
      out.push({
        profileUuid: b.profileUuid,
        profile,
        provider,
        status,
        connected: !!(provider && status?.connected),
        needsReconnect: !!(provider && status?.needsReconnect && !status?.connected),
      });
    }
    return out;
  });
  // Only fully-connected destinations are pushed to (prefill get + confirm set). A
  // needsReconnect destination is connected:false, so it is excluded from BOTH loops
  // here and still goes live via its stream key.
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
    goLiveModal.mode === "golive" ? "Go Live now" : isLive ? "Update info" : "Save info",
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
    // Partial-failure tolerance: a failed metadata push never blocks going live. One
    // aggregate toast (showToast replaces, so per-destination toasts would clobber
    // each other) names the platform(s) in human terms, not raw API strings.
    const failed = results
      .map((r, i) => (r.status === "rejected" ? { dest: targets[i], reason: r.reason as Error } : null))
      .filter((x): x is { dest: Dest; reason: Error } => x !== null);
    const goingLive = goLiveModal.mode === "golive";
    const tail = goingLive ? " — going live anyway" : "";
    if (failed.length === 1) {
      const name = failed[0].dest.provider?.displayName || failed[0].dest.profile.label || "this platform";
      showToast("Couldn't update " + name + " stream info" + tail, failed[0].reason?.message ?? "metadata push failed");
    } else if (failed.length > 1) {
      const names = failed.map((f) => f.dest.provider?.displayName || f.dest.profile.label).join(", ");
      showToast("Couldn't update stream info for " + failed.length + " destinations" + tail, names);
    }
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
</script>

<Modal title="Stream Information" onClose={closeGoLiveModal} width={580} maxHeight="88vh">
  {#snippet headExtra()}
    {#if isLive}<span class="live-dot" title="Live"></span>{/if}
    <Segmented options={VIEW_OPTIONS} value={view} onChange={(v) => (view = v as "simple" | "advanced")} />
  {/snippet}

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
              <!-- Key-only / unconnected / stale-token: muted card, no fields. -->
              <div class="dest keyonly">
                <div class="dh">
                  <span class="pdot" style:background={d.needsReconnect ? "var(--color-accent)" : "var(--color-muted)"}
                  ></span>
                  <span class="pname">{d.provider?.displayName || d.profile.platform || d.profile.label}</span>
                  <span class="pacct">· {d.profile.label}</span>
                </div>
                <div class="body">
                  {#if d.needsReconnect}
                    <p class="note warn">
                      <Icon name="warn" size={11} /> Authorization expired — reconnect in Streams to edit metadata. Streams
                      as-is.
                    </p>
                  {:else}
                    <p class="note">Key-only profile — no metadata API. Streams as-is.</p>
                  {/if}
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
                  <span class="pdot" style:background={d.needsReconnect ? "var(--color-accent)" : "var(--color-muted)"}
                  ></span>
                  <span class="pname">{d.provider?.displayName || d.profile.platform || d.profile.label}</span>
                  {#if d.needsReconnect}
                    <span class="pacct"
                      >· {d.profile.label} — <Icon name="warn" size={10} /> reconnect in Streams to edit metadata, streams as-is</span
                    >
                  {:else}
                    <span class="pacct">· {d.profile.label} — key-only, streams as-is</span>
                  {/if}
                </div>
              </div>
            {/if}
          {/each}
        {/if}
      {/if}

  {#snippet footer()}
    <span class="foot-note">{footerNote}</span>
    <button class="accent" disabled={submitting || !loaded || armed.length === 0} onclick={() => void confirm()}>
      {submitting ? "Working…" : primaryLabel}
    </button>
  {/snippet}
</Modal>

<style>
  .live-dot {
    flex: 0 0 auto;
    width: 7px;
    height: 7px;
    background: var(--color-live);
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
  .note.warn {
    color: var(--color-accent);
    display: flex;
    align-items: center;
    gap: 4px;
  }
  .foot-note {
    flex: 1 1 auto;
    font-size: 11px;
    color: var(--color-muted);
  }
</style>
