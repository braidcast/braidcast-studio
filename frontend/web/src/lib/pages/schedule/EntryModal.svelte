<script lang="ts">
  import { untrack } from "svelte";
  import type { ScheduleEntryInfo } from "$lib/api/bridge";
  import { destinationIdentityStore } from "$lib/stores/destinationIdentityStore.svelte";
  import { scheduleStore } from "$lib/stores/scheduleStore.svelte";
  import { PLATFORM_COLORS, platformKey } from "$lib/theme/platformColors";
  import EmptyState from "$lib/ui/EmptyState.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import Modal from "$lib/ui/Modal.svelte";
  import PlatformMark from "$lib/ui/PlatformMark.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";
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

  const DRAFT_ID = "__draft__";

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
  }));

  let title = $state(initial.title);
  let date = $state(toDateInput(initial.startsAt));
  let time = $state(toTimeInput(initial.startsAt));
  let duration = $state(formatDuration(initial.durationMin));
  let autoStart = $state(initial.autoStart);
  let dests = $state<Set<string>>(new Set(initial.dests));
  let saving = $state(false);
  let error = $state<string | null>(null);

  const options = $derived(destinationIdentityStore.all);
  const startsAt = $derived(fromDateTimeInput(date, time));
  const durationMin = $derived(parseDuration(duration));

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
      blockReason: "",
      destinations: [...dests].map((profileId) => ({
        profileId,
        title: "",
        category: "",
        tags: [] as string[],
      })),
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
    saving = true;
    error = null;
    const cleanTitle = title.trim() || "Untitled stream";
    const input = {
      startsAt,
      title: cleanTitle,
      durationMin,
      announce: false,
      autoStart,
      destinations: [...dests].map((profileId) => ({
        profileId,
        title: cleanTitle,
        category: "",
        tags: [] as string[],
      })),
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
      {#if options.length === 0}
        <EmptyState
          compact
          title="No destinations yet"
          sub="Add one on the Destinations page and it will appear here."
        >
          {#snippet icon()}
            <Icon name="destinations" size={22} />
          {/snippet}
        </EmptyState>
      {:else}
        <div class="chips">
          {#each options as d (d.profileUuid)}
            {@const on = dests.has(d.profileUuid)}
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
              {#if d.canvasName}<span class="ccanvas">{d.canvasName}</span>{/if}
            </button>
          {/each}
        </div>
      {/if}
    </div>

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

  .note {
    margin: 8px 0 0;
    font-family: var(--font-mono);
    font-size: 10px;
    line-height: 1.6;
    color: var(--color-muted);
  }
  .conflict {
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
