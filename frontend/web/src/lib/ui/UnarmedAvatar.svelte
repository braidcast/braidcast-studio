<script lang="ts">
  // The greyed channel avatar a chip falls back to when a destination has no armed
  // canvas -- shared by DestinationChips (the Events/Multichat filter strip) and
  // EntryModal (the schedule destination picker). Grey carries "not armed" /
  // "disabled" rather than a visible word, which truncated in a chip this narrow; the
  // word itself lives only in the caller's aria-label/title.
  import Avatar from "$lib/ui/Avatar.svelte";

  interface Props {
    url?: string;
    name?: string;
    size?: number;
    /** DestinationIdentity.boundButDisabled -- a binding exists but is switched off.
     * Draws the corner ring that tells it apart from plain "not armed" (no binding at
     * all): both are greyed identically otherwise, and the two ask for opposite next
     * actions (enable the one you have vs. go create one). */
    disabled?: boolean;
    /** True while the surrounding chip is itself selected/toggled on, so picking this
     * destination still reads as the foreground action over the grey state -- mirrors
     * how the chip's own text brightens on selection. */
    active?: boolean;
  }
  let { url = "", name = "", size = 14, disabled = false, active = false }: Props = $props();
</script>

<span class="unarmed-avatar">
  <span class="unarmed-avatar-img" style:opacity={active ? 0.85 : 0.6}>
    <Avatar {url} {name} {size} />
  </span>
  {#if disabled}
    <!-- aria-hidden: decorative only, `disabled` already reaches the caller's
         aria-label/title. -->
    <svg class="offmark" width="8" height="8" viewBox="0 0 8 8" aria-hidden="true">
      <circle cx="4" cy="4" r="3.25" fill="none" stroke="currentColor" stroke-width="1" />
      <line x1="1.5" y1="6.5" x2="6.5" y2="1.5" stroke="currentColor" stroke-width="1" />
    </svg>
  {/if}
</span>

<style>
  .unarmed-avatar {
    position: relative;
    display: inline-flex;
    flex: none;
  }
  /* Grayscale lives on the image wrapper alone, never on `.unarmed-avatar` itself:
     .offmark is this span's SIBLING, not its descendant, so the ring is unaffected by
     either the filter or the opacity below it. Kept separate on purpose: blended
     through the avatar's 0.6 opacity, --color-dim on --color-surface-2 falls to
     2.58:1 (Industrial), under SC 1.4.11's 3:1 floor for non-text UI; undimmed, the
     pairing measures 4.81-8.31:1 across every preset x mode. */
  .unarmed-avatar-img {
    display: inline-flex;
    filter: grayscale(1);
  }
  .offmark {
    position: absolute;
    right: -2px;
    bottom: -2px;
    border-radius: 50%;
    background: var(--color-surface-2);
    color: var(--color-dim);
  }
</style>
