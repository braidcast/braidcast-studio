<script lang="ts">
  // A canvas's permanent number in a box shaped like the canvas. Replaces the canvas
  // NAME wherever a row is too narrow to carry one -- the events feed and the chat
  // feed stamp every line with this -- so attribution costs a fixed few pixels
  // instead of a name that pushes the message itself off the row.
  //
  // The box aspect encodes orientation because that is the thing being told apart
  // most of the time: a 16:9 and a 9:16 of the SAME channel are two destinations a
  // name like "Main"/"Vertical" distinguishes only if you remember which is which.
  // Shape is never the only channel, though -- role="img" + aria-label carries the
  // real canvas name, and every caller also sets a title, so the mapping is
  // recoverable by hover and by screen reader, not just by memory.
  //
  // Sized from the number rather than fixed: numbers are never reused, so a install
  // that has created and deleted canvases will reach two digits, and a fixed box
  // would clip it.

  interface Props {
    /** CanvasInfo.number. 0 means the host has not numbered it yet; the box renders
     * a placeholder rather than "0", which is not a number any canvas holds. */
    number: number;
    /** Canvas name, for the accessible label and the tooltip. */
    name: string;
    /** Encode size, for orientation. Equal values (or absent) read as square. */
    width?: number;
    height?: number;
    /** Box height in px; the width follows from the orientation. */
    size?: number;
    /** Tooltip override; defaults to the name plus the number. */
    title?: string;
  }
  let { number, name, width = 0, height = 0, size = 16, title }: Props = $props();

  type Orientation = "landscape" | "portrait" | "square";
  const orientation = $derived<Orientation>(
    width > 0 && height > 0 ? (height > width ? "portrait" : width > height ? "landscape" : "square") : "square",
  );

  const label = $derived(number > 0 ? String(number) : "?");
  const displayName = $derived(name.trim() || "Unnamed canvas");
  // Spoken and hovered form. The number alone would be the one thing a reader cannot
  // resolve without the bar in front of them, so the name leads.
  const described = $derived(
    number > 0 ? `${displayName} — canvas ${number}, ${orientation}` : `${displayName} — not yet numbered`,
  );
</script>

<span
  class="mark {orientation}"
  role="img"
  aria-label={described}
  title={title ?? described}
  style="--mark-size: {size}px"
>
  {label}
</span>

<style>
  .mark {
    display: inline-grid;
    place-items: center;
    flex: none;
    box-sizing: border-box;
    height: var(--mark-size);
    /* Thick enough to read as a deliberate frame at 14-16px rather than as a hairline
       the row's other borders swallow. */
    border: 2px solid currentColor;
    border-radius: 2px;
    /* Tabular so a column of marks keeps its digits on one vertical axis, and bold so
       the numeral survives being this small. */
    font-family: var(--font-mono);
    font-variant-numeric: tabular-nums;
    font-weight: 700;
    font-size: calc(var(--mark-size) * 0.58);
    line-height: 1;
    /* Horizontal breathing room only: the height is the orientation signal and must
       not grow with the digit count. */
    padding: 0 calc(var(--mark-size) * 0.14);
    color: inherit;
  }

  /* The three aspects. min-width carries the shape for a single digit; a second digit
     widens the box past it, which is fine -- by then the height difference is doing
     the work, and a clipped number would not be. */
  .landscape {
    height: calc(var(--mark-size) * 0.74);
    min-width: calc(var(--mark-size) * 1.2);
  }
  .portrait {
    min-width: calc(var(--mark-size) * 0.74);
  }
  .square {
    height: calc(var(--mark-size) * 0.88);
    min-width: calc(var(--mark-size) * 0.88);
  }
</style>
