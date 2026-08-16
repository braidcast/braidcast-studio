<script lang="ts">
  import { untrack } from "svelte";
  import type {
    OAuthProviderField,
    ScheduleDestinationInfo,
    ScheduleEntryInfo,
    StreamInfoPreset,
  } from "$lib/api/bridge";
  import GoLiveCategoryInput from "$lib/dialogs/golive/GoLiveCategoryInput.svelte";
  import GoLiveFieldInput from "$lib/dialogs/golive/GoLiveFieldInput.svelte";
  import GoLiveTagsInput from "$lib/dialogs/golive/GoLiveTagsInput.svelte";
  import PresetPicker from "$lib/dialogs/streamInfoPresets/PresetPicker.svelte";
  import { presetLabel, schedulePatchFor } from "$lib/dialogs/streamInfoPresets/applyPreset";
  import {
    destinationIdentityStore,
    unarmedLabel,
    type DestinationIdentity,
  } from "$lib/stores/destinationIdentityStore.svelte";
  import { oauthStore } from "$lib/stores/oauthStore.svelte";
  import { scheduleStore } from "$lib/stores/scheduleStore.svelte";
  import { PLATFORM_COLORS, platformKey } from "$lib/theme/platformColors";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
  import { joinNames } from "$lib/utils/format";
  import {
    destinationConflicts,
    formatDuration,
    fromDateTimeInput,
    parseDuration,
    toDateInput,
    toTimeInput,
  } from "./layout";

  // Create/edit for one planned entry. Lifted out of SchedulePage so the page owns
  // views and state and nothing else. Remounted per open by the caller, so the form
  // is initialised from props at construction rather than resynced by an effect.
  interface Props {
    /** The entry being edited; null creates a new one. */
    entry: ScheduleEntryInfo | null;
    /** Prefill for a create -- a drag on the grid sets both in one gesture. */
    initialStart: number;
    initialDurationMin: number;
    /** Every other unsettled entry, for the conflict check while editing. */
    others: ScheduleEntryInfo[];
    onClose: () => void;
    /** The caller navigates the calendar to where the entry actually landed. */
    onSaved: (startsAt: number) => void;
  }
  let { entry, initialStart, initialDurationMin, others, onClose, onSaved }: Props = $props();

  // Idempotent, and the page already does it -- but this form now reads
  // oauthStore.providers as well as the identity join, and providers arrive on the
  // subscription this opens. Stating the dependency here keeps the metadata section
  // from silently reading every destination as stream-key-only.
  destinationIdentityStore.start();

  const DRAFT_ID = "__draft__";

  /** What one destination will have applied to it at go-live. An untouched
   * destination stays exactly this: the runner reads an all-empty bag as "nothing
   * to apply", which is the correct behaviour and must not be defeated by a
   * helpful default. Nothing here is ever seeded from the entry's own title. */
  const EMPTY_META = { title: "", category: "", categoryId: "", tags: [] as string[] };
  type DestMeta = typeof EMPTY_META;

  // Read once, untracked: the form is a draft of the entry, not a live view of it,
  // and the caller remounts this component per open. Without untrack the compiler
  // reads these as props captured by accident rather than on purpose.
  const initial = untrack(() => ({
    title: entry?.title ?? "Untitled stream",
    startsAt: entry?.startsAt ?? initialStart,
    durationMin: entry?.durationMin ?? initialDurationMin,
    autoStart: entry?.autoStart ?? false,
    // Keyed by profile id, not label: two profiles may share a label, and a
    // selection keyed by label would silently merge them.
    dests: entry
      ? entry.destinations.map((d) => d.profileId)
      : destinationIdentityStore.all
          .filter((d) => d.canvasUuid !== null)
          .slice(0, 1)
          .map((d) => d.profileUuid),
    meta: new Map<string, DestMeta>(
      (entry?.destinations ?? []).map((d) => [
        d.profileId,
        { title: d.title, category: d.category, categoryId: d.categoryId, tags: [...d.tags] },
      ]),
    ),
  }));

  let title = $state(initial.title);
  let date = $state(toDateInput(initial.startsAt));
  let time = $state(toTimeInput(initial.startsAt));
  let duration = $state(formatDuration(initial.durationMin));
  let autoStart = $state(initial.autoStart);
  let dests = $state<Set<string>>(new Set(initial.dests));
  // Survives a destination being deselected and reselected: losing what was typed
  // to a mis-click is a worse answer than holding a bag nothing reads. Only the
  // selected ids are written on save.
  let meta = $state<Map<string, DestMeta>>(initial.meta);
  let openMeta = $state<string | null>(null);
  let saving = $state(false);
  let error = $state<string | null>(null);

  // The destinations this entry named when it was opened. Held for the whole edit so a
  // carried-over row stays in the list after it is switched off: vanishing on click
  // reads as a delete, and undoing a mis-click would then be impossible.
  const carried = new Set(initial.dests);

  // Only destinations with an ENABLED output binding are offered. Go-live narrows the
  // enabled set and never widens it (ScheduledSetup::Apply), so a destination named while
  // switched off is simply left switched off -- picking one plans a broadcast that will
  // not reach it, and canArm refuses it at go-live rather than routing it. `canvasUuid` is
  // non-null exactly when an enabled binding exists.
  //
  // Two unions, neither of which softens that rule -- both exist so that what is offered
  // can never be narrower than what a save writes:
  //
  //   `dests`   -- everything currently selected. `draftDestinations()` writes this set
  //                raw, so a selected row must stay on screen even if its binding is
  //                switched off from another window mid-edit; otherwise it would be saved
  //                while invisible and impossible to deselect.
  //   `carried` -- what the entry named when it was opened, kept past a deselection so
  //                switching one off stays undoable, and so filtering alone cannot drop a
  //                destination the user configured.
  //
  // Both kinds are marked, and toggle both ways like any other row.
  const options = $derived(
    destinationIdentityStore.all.filter(
      (d) => d.canvasUuid !== null || dests.has(d.profileUuid) || carried.has(d.profileUuid),
    ),
  );
  // Everything in `dests` that still names a live profile -- by the union above this is
  // the whole of what draftDestinations() writes, never a subset of it.
  const selected = $derived(options.filter((d) => dests.has(d.profileUuid)));
  /** Offered in spite of the enabled filter: selected now, or named by the saved entry. */
  const unavailable = $derived(options.filter((d) => d.canvasUuid === null));

  // Per-destination refusals as the host last computed them. Read off the saved entry
  // and never the draft: whether a destination can route is the host's verdict, and one
  // added since the last save has not been looked at yet.
  const blockReasonByProfile = $derived(
    new Map((entry?.destinations ?? []).map((d) => [d.profileId, d.blockReason])),
  );
  const blockedSelected = $derived(
    selected
      .map((d) => ({ d, reason: blockReasonByProfile.get(d.profileUuid) ?? "" }))
      .filter((b) => b.reason !== ""),
  );

  // The provider capability descriptor behind a destination, or null for a
  // stream-key profile. It decides WHICH of the three columns a platform can even
  // receive -- a control for a field the provider never declared would collect a
  // value with nowhere to land.
  function providerOf(d: DestinationIdentity) {
    const key = platformKey(d.platform);
    return oauthStore.providers.find((p) => platformKey(p.id) === key) ?? null;
  }
  function fieldOf(d: DestinationIdentity, key: string): OAuthProviderField | null {
    return providerOf(d)?.fields.find((f) => f.key === key) ?? null;
  }

  function metaOf(profileId: string): DestMeta {
    return meta.get(profileId) ?? EMPTY_META;
  }
  function patchMeta(profileId: string, patch: Partial<DestMeta>): void {
    const next = new Map(meta);
    next.set(profileId, { ...metaOf(profileId), ...patch });
    meta = next;
  }

  // The category control syncs its visible text off the identity of the value it is
  // given, so a fresh {id,name} literal per render would reset the box whenever the
  // metadata it reads is reassigned -- clobbering a category being typed into it from
  // a keystroke in the per-destination title beside it. Held per profile so the
  // reference only changes when the category itself does.
  const catByProfile = new Map<string, { id: string; name: string }>();
  function categoryOf(profileId: string): { id: string; name: string } | null {
    const m = metaOf(profileId);
    if (m.categoryId === "") {
      return null;
    }
    const held = catByProfile.get(profileId);
    if (held && held.id === m.categoryId && held.name === m.category) {
      return held;
    }
    const next = { id: m.categoryId, name: m.category };
    catByProfile.set(profileId, next);
    return next;
  }

  let presetPickerOpen = $state(false);
  // Both halves of what the last load did: how far it reached, and what a planned entry
  // has no room for. The second half is the one that must never be silent -- a sheet
  // whose description simply vanished would read as applied.
  let presetNote = $state("");

  // Loads a saved sheet into the selected rows, resolved through each destination's own
  // provider: which fields a preset states for a platform is the provider's declaration,
  // so a sheet holding a Twitch category does not put that id on a YouTube row.
  //
  // Written through patchMeta, never by assigning `meta`, so the category control's
  // identity cache keeps its held {id,name} for rows this does not change. User-triggered
  // by construction -- the form is initialised once, untracked, and nothing else writes it.
  function applyPreset(preset: StreamInfoPreset): void {
    const dropped = new Set<string>();
    const malformed = new Set<string>();
    let reached = 0;
    for (const d of selected) {
      const provider = providerOf(d);
      if (!provider) {
        continue;
      }
      const patched = schedulePatchFor(preset, provider);
      for (const label of patched.dropped) {
        dropped.add(label);
      }
      for (const label of patched.malformed) {
        malformed.add(label);
      }
      // An empty patch is not a destination reached: a sheet whose every value was
      // unusable would otherwise be counted as applied to a row it never touched.
      if (Object.keys(patched.patch).length > 0) {
        patchMeta(d.profileUuid, patched.patch);
        reached += 1;
      }
    }
    const parts = [
      `Loaded "${presetLabel(preset)}" into ${reached} ${reached === 1 ? "destination" : "destinations"}.`,
    ];
    // A planned entry stores a title, a category and tags per destination and nothing
    // else, so anything else the sheet carries has nowhere to go here. Said in words
    // rather than dropped, and named from what the sheet actually held.
    if (dropped.size > 0) {
      parts.push(
        `A scheduled entry stores only a title, a category and tags, so ${joinNames([...dropped])} ${
          dropped.size === 1 ? "is" : "are"
        } not applied to it — set that in Stream Information when you go live.`,
      );
    }
    if (malformed.size > 0) {
      parts.push(
        `${joinNames([...malformed])} could not be read from this preset and ${
          malformed.size === 1 ? "was" : "were"
        } left as ${malformed.size === 1 ? "it is" : "they are"}.`,
      );
    }
    presetNote = parts.join(" ");
  }

  function isEmptyMeta(m: DestMeta): boolean {
    return m.title === "" && m.categoryId === "" && m.category === "" && m.tags.length === 0;
  }

  /** The collapsed row's one line. Says plainly when nothing will be applied, since
   * that is a real and common state rather than an unfinished one. */
  function metaSummary(m: DestMeta): string {
    if (isEmptyMeta(m)) {
      return "Nothing to apply";
    }
    return [m.title, m.category, m.tags.length > 0 ? `${m.tags.length} tags` : ""]
      .filter((part) => part !== "")
      .join(" · ");
  }
  const startsAt = $derived(fromDateTimeInput(date, time));
  const durationMin = $derived(parseDuration(duration));

  /** The selected destinations as the row shape the bridge takes. One builder for
   * the conflict probe and the save, so what is checked is what is written. */
  function draftDestinations(): ScheduleDestinationInfo[] {
    return [...dests].map((profileId) => {
      const m = metaOf(profileId);
      return {
        profileId,
        title: m.title,
        // Both, never one: providers key on the id (twitch_provider's game_id,
        // youtube_provider's categoryId) and the name is only ever prefill and
        // display. A name standing in for an id is an apply that silently does
        // nothing.
        category: m.category,
        categoryId: m.categoryId,
        tags: [...m.tags],
        // Host-computed and ignored on write, so it goes out empty rather than echoing
        // back a verdict that was reached before this edit.
        blockReason: "",
      };
    });
  }

  // The conflict is checked against the draft as it is typed, not at go-live: one
  // stream profile is one RTMP key, so two overlapping entries sharing one can
  // never both run, and the cheap moment to learn that is now.
  const draftConflict = $derived.by(() => {
    const draft: ScheduleEntryInfo = {
      id: DRAFT_ID,
      startsAt,
      title: title.trim() || "This entry",
      durationMin,
      announce: false,
      autoStart,
      state: "planned",
      countdownCanceled: false,
      startRequested: false,
      blockReason: "",
      destinations: draftDestinations(),
    };
    return destinationConflicts([...others, draft], Date.now()).get(DRAFT_ID) ?? null;
  });

  const conflictNames = $derived(
    draftConflict
      ? draftConflict.profileIds.map(
          (id) => destinationIdentityStore.forProfile(id)?.displayName ?? id,
        )
      : [],
  );

  function toggleDest(profileId: string): void {
    const next = new Set(dests);
    if (next.has(profileId)) {
      next.delete(profileId);
      if (openMeta === profileId) {
        openMeta = null;
      }
    } else {
      next.add(profileId);
    }
    dests = next;
  }

  // The modal stays open on failure with the reason on it: closing would discard
  // what the user typed and leave them guessing why the entry never appeared.
  async function save(): Promise<void> {
    if (saving) {
      return;
    }
    // fromDateTimeInput reports NaN for a cleared date or time, and a NaN startsAt
    // written through would be a silently undated entry.
    if (Number.isNaN(startsAt)) {
      error = "Enter a valid date and time.";
      return;
    }
    saving = true;
    error = null;
    const input = {
      startsAt,
      title: title.trim() || "Untitled stream",
      durationMin,
      announce: false,
      autoStart,
      destinations: draftDestinations(),
    };
    try {
      // No refresh: the host pushes schedule.changed on create and update, and
      // the store already re-lists on it.
      if (entry) {
        await scheduleStore.update(entry.id, input);
      } else {
        await scheduleStore.create(input);
      }
      onSaved(startsAt);
      onClose();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      saving = false;
    }
  }

  async function remove(): Promise<void> {
    if (!entry || saving) {
      return;
    }
    saving = true;
    error = null;
    try {
      await scheduleStore.remove(entry.id);
      onClose();
    } catch (e) {
      error = (e as Error).message;
    } finally {
      saving = false;
    }
  }
</script>

<Modal title={entry ? "Edit Stream" : "Schedule a Stream"} {onClose} width={540}>
  <div class="form">
    <div class="field">
      <div class="f-label">TITLE</div>
      <input class="f-input" bind:value={title} spellcheck="false" />
    </div>

    <div class="field-row">
      <div class="field flex1">
        <div class="f-label">DATE</div>
        <input class="f-input" type="date" bind:value={date} />
      </div>
      <div class="field f-time">
        <div class="f-label">TIME</div>
        <input class="f-input" type="time" bind:value={time} />
      </div>
      <div class="field f-dur">
        <div class="f-label">DURATION</div>
        <input class="f-input" bind:value={duration} spellcheck="false" />
      </div>
    </div>

    <div class="field">
      <div class="f-label">DESTINATIONS</div>
      {#if destinationIdentityStore.all.length === 0}
        <EmptyState
          compact
          title="No destinations yet"
          sub="Add one on the Destinations page and it will appear here."
        >
          {#snippet icon()}
            <Icon name="destinations" size={22} />
          {/snippet}
        </EmptyState>
      {:else if options.length === 0}
        <!-- Reachable and not an error: with every binding switched off there is
             nothing a schedule may name, and saying so beats an empty box. -->
        <EmptyState
          compact
          title="No destination is enabled"
          sub="Only a destination with an enabled output binding can be scheduled. Enable one on the Destinations page and it will appear here."
        >
          {#snippet icon()}
            <Icon name="destinations" size={22} />
          {/snippet}
        </EmptyState>
      {:else}
        <div class="chips">
          {#each options as d (d.profileUuid)}
            {@const on = dests.has(d.profileUuid)}
            {@const off = d.canvasUuid === null}
            <button
              type="button"
              class="chip"
              class:on
              aria-pressed={on}
              style:--chip={PLATFORM_COLORS[platformKey(d.platform)] || "var(--color-accent)"}
              onclick={() => toggleDest(d.profileUuid)}
            >
              <PlatformMark platform={d.platform} size={12} />
              <span class="cname">{d.displayName}</span>
              <!-- The state word takes the canvas chip's slot, which is empty for
                   exactly these rows -- and it is a word, so the tone is not the only
                   thing carrying it. -->
              {#if off}
                <span class="ccanvas off">{unarmedLabel(d)}</span>
              {:else if d.canvasName}
                <span class="ccanvas">{d.canvasName}</span>
              {/if}
            </button>
          {/each}
        </div>
        {#if unavailable.length > 0}
          <p class="note">
            Marked destinations have no enabled output binding. They stay on the entry and
            saving keeps them, but going live will not switch one on — they cannot be reached
            until one is enabled on the Destinations page.
          </p>
        {/if}
        {#if blockedSelected.length > 0}
          <div class="blocked">
            <Icon name="warn" size={12} />
            <div class="blist">
              {#each blockedSelected as b (b.d.profileUuid)}
                <span>{b.d.displayName} — {b.reason}</span>
              {/each}
            </div>
          </div>
        {/if}
      {/if}
    </div>

    {#if selected.length > 0}
      <div class="field">
        <div class="f-head">
          <div class="f-label">PER-DESTINATION METADATA</div>
          <button type="button" class="preset-btn" onclick={() => (presetPickerOpen = true)}>
            Saved info
          </button>
        </div>
        <div class="meta-list">
          {#each selected as d (d.profileUuid)}
            {@const m = metaOf(d.profileUuid)}
            {@const expanded = openMeta === d.profileUuid}
            {@const panelId = `meta-${d.profileUuid}`}
            {@const provider = providerOf(d)}
            {@const titleField = fieldOf(d, "title")}
            {@const categoryField = fieldOf(d, "category")}
            {@const tagsField = fieldOf(d, "tags")}
            <div class="meta-row" class:expanded>
              <button
                class="meta-head"
                type="button"
                aria-expanded={expanded}
                aria-controls={panelId}
                onclick={() => (openMeta = expanded ? null : d.profileUuid)}
              >
                <PlatformMark platform={d.platform} size={13} />
                <span class="meta-name">{d.displayName}</span>
                <span class="meta-sum" class:none={isEmptyMeta(m)}>{metaSummary(m)}</span>
                <span class="meta-caret" class:open={expanded}>
                  <Icon name="caret-down" size={12} />
                </span>
              </button>

              {#if expanded}
                <div class="meta-body" id={panelId}>
                  {#if !provider}
                    <p class="note">
                      A stream-key destination has no metadata API, so nothing is applied to it
                      at go-live.
                    </p>
                  {:else}
                    {#if titleField}
                      <div class="sub">
                        <div class="f-label">{titleField.label.toUpperCase()}</div>
                        <GoLiveFieldInput
                          field={titleField}
                          value={m.title}
                          providerId={provider.id}
                          accountId={d.accountId}
                          placeholder="Leave empty to change nothing"
                          onChange={(v) =>
                            patchMeta(d.profileUuid, { title: typeof v === "string" ? v : "" })}
                        />
                      </div>
                    {/if}
                    {#if categoryField}
                      <div class="sub">
                        <div class="f-label">{categoryField.label.toUpperCase()}</div>
                        <GoLiveCategoryInput
                          providerId={provider.id}
                          accountId={d.accountId}
                          placeholder={categoryField.placeholder ?? ""}
                          browsable={categoryField.browsable ?? false}
                          value={categoryOf(d.profileUuid)}
                          onChange={(v) =>
                            patchMeta(d.profileUuid, {
                              categoryId: v?.id ?? "",
                              category: v?.name ?? "",
                            })}
                        />
                      </div>
                    {/if}
                    {#if tagsField}
                      <div class="sub">
                        <div class="f-label">{tagsField.label.toUpperCase()}</div>
                        <!-- No onReset, because a scheduled row has no layer under it to
                             fall back to and no unset state to return to: an empty list here
                             means "leave the channel's own tags alone" (ScheduledSetup.cpp
                             omits the key for it), not the clear the same empty list means in
                             the Go Live dialog. So "Remove all" empties the row rather than
                             instructing a clear, and Reset is not offered at all. -->
                        <GoLiveTagsInput
                          field={tagsField}
                          providerId={provider.id}
                          values={m.tags}
                          onChange={(next) => patchMeta(d.profileUuid, { tags: next })}
                        />
                      </div>
                    {/if}
                    {#if !titleField && !categoryField && !tagsField}
                      <p class="note">
                        {provider.displayName} declares none of these fields, so there is nothing
                        to set here.
                      </p>
                    {/if}
                  {/if}
                </div>
              {/if}
            </div>
          {/each}
        </div>
        <!-- Announced, not merely rendered: loading a sheet rewrites several rows at once
             and this sentence is the only statement of what happened, including what was
             left out. Kept in the DOM while empty so the live region exists before its
             text changes. -->
        <p class="note presetnote" role="status">{presetNote}</p>
        <p class="note">
          Applied to each destination when the entry goes live. Anything left empty is left
          alone — the channel keeps whatever it already says.
        </p>
      </div>
    {/if}

    <div class="field">
      <div class="f-label">AUTO-START</div>
      <ToggleSwitch
        checked={autoStart}
        onchange={(v) => (autoStart = v)}
        label="Go live automatically at this time"
      />
      <p class="note">
        Braidcast arms the entry five minutes ahead and counts down for the last minute.
        The countdown can be canceled from the entry on the calendar.
      </p>
    </div>

    {#if draftConflict}
      <p class="conflict">
        <Icon name="warn" size={12} />
        Overlaps {draftConflict.others.join(", ")} on {conflictNames.join(", ")}. One stream
        profile cannot carry two concurrent outputs, so only one of these can go live.
      </p>
    {/if}

    {#if error}
      <p class="error">{error}</p>
    {/if}
  </div>

  {#snippet footer()}
    {#if entry}
      <button class="ghost danger" onclick={remove} disabled={saving}>Delete</button>
    {/if}
    <button class="ghost" onclick={onClose} disabled={saving}>Cancel</button>
    <button class="accent" onclick={save} disabled={saving}>
      {saving ? "Saving…" : entry ? "Save Changes" : "Schedule Stream"}
    </button>
  {/snippet}
</Modal>

{#if presetPickerOpen}
  <PresetPicker
    title="Saved Stream Info"
    onPick={applyPreset}
    onClose={() => (presetPickerOpen = false)}
  />
{/if}

<style>
  .form {
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  .field-row {
    display: flex;
    gap: 12px;
  }
  .flex1 {
    flex: 1;
  }
  .f-time {
    flex: 0 0 120px;
  }
  .f-dur {
    flex: 0 0 100px;
  }
  .f-label {
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    color: var(--color-muted);
    margin-bottom: 6px;
  }
  /* Section label with its one action on the right; the label keeps its own baseline
     spacing, so the gap under the row comes from the label as everywhere else. */
  .f-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
  }
  .preset-btn {
    height: 24px;
    padding: 0 9px;
    margin-bottom: 6px;
    flex: 0 0 auto;
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-dim);
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
  }
  .preset-btn:hover {
    border-color: var(--color-accent);
    color: var(--color-accent);
  }
  .preset-btn:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
  .f-input {
    width: 100%;
    background: var(--color-base);
    border: var(--border-weight) solid var(--color-border);
    color: var(--color-text);
    font-family: var(--font-ui);
    font-size: 13px;
    padding: 9px 11px;
    outline: none;
  }
  .f-input:focus {
    border-color: var(--color-accent);
  }

  /* Multi-select, so this cannot be DestinationChips: that control renders exactly
     one selection (all / a platform / a destination) and two other surfaces depend
     on that meaning. The chip look is deliberately its twin. */
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }
  .chip {
    display: flex;
    align-items: center;
    gap: 5px;
    max-width: 100%;
    min-width: 0;
    height: auto;
    padding: 5px 9px;
    font-family: var(--font-ui);
    font-size: 11px;
    color: var(--color-dim);
    background: transparent;
    border: var(--border-weight) solid var(--color-border);
  }
  .chip.on {
    border-color: var(--chip, var(--color-accent));
    color: var(--color-text);
    background: color-mix(in srgb, var(--chip, var(--color-accent)) 14%, transparent);
  }
  .chip:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: 1px;
  }
  .cname {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .ccanvas {
    flex: 0 0 auto;
    font-family: var(--font-mono);
    font-size: 9px;
    letter-spacing: 0.08em;
    color: var(--color-muted);
  }
  .ccanvas.off {
    color: var(--color-live);
  }

  /* Accordion rather than every destination expanded at once: three open panels of
     three fields is taller than the modal, and the collapsed line already says
     whether a destination has anything to apply. */
  .meta-list {
    display: flex;
    flex-direction: column;
    border: var(--border-weight) solid var(--color-border);
  }
  .meta-row {
    border-bottom: var(--border-weight) solid var(--color-border);
  }
  .meta-row:last-child {
    border-bottom: 0;
  }
  .meta-head {
    display: flex;
    align-items: center;
    gap: 8px;
    width: 100%;
    height: auto;
    padding: 8px 10px;
    background: transparent;
    border: 0;
    text-align: left;
  }
  .meta-head:hover {
    background: color-mix(in srgb, var(--color-text) 4%, transparent);
    border: 0;
  }
  .meta-head:focus-visible {
    outline: 2px solid var(--color-accent);
    outline-offset: -2px;
  }
  .meta-name {
    flex: 0 0 auto;
    max-width: 40%;
    font-size: 12px;
    color: var(--color-text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .meta-sum {
    flex: 1;
    min-width: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-dim);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .meta-sum.none {
    color: var(--color-muted);
  }
  .meta-caret {
    flex: 0 0 auto;
    display: inline-flex;
    color: var(--color-muted);
    transition: transform 0.12s ease;
  }
  .meta-caret.open {
    transform: rotate(180deg);
  }
  .meta-body {
    display: flex;
    flex-direction: column;
    gap: 12px;
    padding: 4px 10px 12px;
    background: color-mix(in srgb, var(--color-base) 55%, transparent);
  }
  .sub .f-label {
    margin-bottom: 5px;
  }

  .presetnote:empty {
    display: none;
  }
  .note {
    margin: 8px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-muted);
  }
  .conflict,
  .blocked {
    display: flex;
    align-items: flex-start;
    gap: 6px;
    margin: 0;
    padding: 9px 11px;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-live);
    border: var(--border-weight) solid color-mix(in srgb, var(--color-live) 45%, transparent);
  }
  .blocked {
    margin-top: 8px;
  }
  .blist {
    display: flex;
    flex-direction: column;
    gap: 4px;
    min-width: 0;
  }
  .error {
    margin: 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-live);
  }
  .ghost.danger {
    color: var(--color-live);
  }
</style>
