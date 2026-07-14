<script lang="ts">
  // Tiny pointer-based drag handle between two flex panes. Reports incremental
  // movement (px since the last pointermove) along its cross axis via `onDrag`; the
  // host clamps and applies it to a size var. Pointer capture keeps the drag alive
  // even when the cursor leaves the 5px hit area. No deps.
  interface Props {
    orientation: "row" | "column";
    onDrag: (delta: number) => void;
  }
  let { orientation, onDrag }: Props = $props();

  let last = 0;
  let dragging = $state(false);

  function down(e: PointerEvent) {
    e.preventDefault();
    dragging = true;
    last = orientation === "row" ? e.clientX : e.clientY;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  }
  function move(e: PointerEvent) {
    if (!dragging) {
      return;
    }
    const pos = orientation === "row" ? e.clientX : e.clientY;
    onDrag(pos - last);
    last = pos;
  }
  function up(e: PointerEvent) {
    if (!dragging) {
      return;
    }
    dragging = false;
    (e.currentTarget as HTMLElement).releasePointerCapture(e.pointerId);
  }
</script>

<div
  class="splitter {orientation}"
  class:dragging
  role="separator"
  aria-orientation={orientation === "row" ? "vertical" : "horizontal"}
  onpointerdown={down}
  onpointermove={move}
  onpointerup={up}
  onpointercancel={up}
></div>
