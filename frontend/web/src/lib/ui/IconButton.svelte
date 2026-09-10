<script lang="ts" module>
  import type { Snippet } from "svelte";
  import type { HTMLButtonAttributes } from "svelte/elements";
  import type { IconName } from "./Icon.svelte";

  // The resting box, low to high. Hover paints an edge on all three, so the variant
  // decides what the button shows before it is pointed at, not how it reacts.
  //   bare    - no edge at rest (dock toolbars, row toggles, close buttons)
  //   outline - edge at rest
  //   surface - edge at rest over a filled surface
  export type IconButtonVariant = "bare" | "outline" | "surface";

  // Resting glyph color: `accent` for an engaged toggle, `live` for a state the app
  // paints in the live color (monitoring routed to output, a destructive action at
  // rest). Hover preserves a tone rather than overriding it.
  export type IconButtonTone = "default" | "accent" | "live";

  // Named boxes for the shapes that repeat across docks and dialogs, so changing a
  // toolbar's button size is one edit here instead of one per call site. Spread at
  // the call site: <IconButton {...ICONBTN_TOOLBAR} icon="search" … />.
  export const ICONBTN_TOOLBAR = { size: 25, height: 22 } as const;
  export const ICONBTN_ROW = { size: 20, height: 16, iconSize: 12 } as const;
  export const ICONBTN_UTILITY = { size: 28, iconSize: 15, variant: "surface" } as const;
  export const ICONBTN_FIELD = {
    size: 28,
    height: "var(--control-height)",
    iconSize: 12,
    variant: "surface",
  } as const;

  interface IconButtonBase extends Omit<HTMLButtonAttributes, "aria-label" | "title" | "class" | "children"> {
    /** Glyph from the shared set. Omit and pass `children` for a one-off inline SVG. */
    icon?: IconName;
    children?: Snippet;
    /** Outer box; a number is px, a string is a raw CSS length. Square unless `height` overrides. */
    size?: number | string;
    height?: number | string;
    /** Glyph box in px. */
    iconSize?: number;
    variant?: IconButtonVariant;
    tone?: IconButtonTone;
    /** Destructive action: hover paints the edge live, and an untoned glyph reddens with it. */
    danger?: boolean;
  }

  // The glyph is the whole control, so one of the two naming attributes is required
  // rather than optional -- a call site cannot leave the button unnamed.
  export type IconButtonProps =
    | (IconButtonBase & { "aria-label": string; title?: string })
    | (IconButtonBase & { title: string; "aria-label"?: string });
</script>

<script lang="ts">
  import Icon from "./Icon.svelte";

  let {
    icon,
    children,
    size = 26,
    height,
    iconSize = 13,
    variant = "bare",
    tone = "default",
    danger = false,
    type = "button",
    ...rest
  }: IconButtonProps = $props();

  const len = (v: number | string) => (typeof v === "number" ? `${v}px` : v);
</script>

<button
  {...rest}
  {type}
  class="iconbtn iconbtn-{variant} iconbtn-{tone}"
  class:iconbtn-danger={danger}
  style:width={len(size)}
  style:height={len(height ?? size)}
  style:--iconbtn-glyph="{iconSize}px"
>
  {#if icon}
    <Icon name={icon} size={iconSize} />
  {:else}
    {@render children?.()}
  {/if}
</button>

<style>
  /* The global `button` rule (app.css:183-191) sets `padding: 0 12px`, a fixed
     `height`, a surface background and a border. Under the global
     `box-sizing: border-box` (app.css:112-115) a width narrower than that padding
     leaves a zero-width content box, and the glyph -- a flex item, so shrinkable --
     collapses into it while the border box still paints. Cancelling all four here is
     what makes a declared size mean a usable content box. Its `cursor: pointer`
     (app.css:185) is deliberately not among them: leaving it inherited is what lets
     `button:disabled` (app.css:197-200) still land its `cursor: default`. */
  .iconbtn {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0;
    background: transparent;
    /* Every variant reserves the same border box and varies only its color, so the
       content box does not move when a variant paints a visible edge. */
    border: var(--border-weight) solid transparent;
    /* --color-dim, not --color-muted: the glyph carries no text label, so it is the
       whole control and owes SC 1.4.11's 3:1. Over the four presets x five accents x
       both modes, against --color-surface, --color-rail and a selected dock row,
       muted bottoms out at 2.21:1 and dim at 3.96:1 -- both on Slate's selected row,
       which is a 22% accent wash because Slate ships selectionStyle `fill`
       (lib/theme/presets.ts:174, app.css:461-463) rather than the 12% of the other
       three (app.css:459). Both tokens are redefined per preset, so app.css:68 is
       only the pre-hydration default. */
    color: var(--color-dim);
    /* Declared size is the outer box, so the glyph plus both borders is the floor
       below which the content box can no longer hold the glyph. Derived from
       --border-weight rather than a constant: it is a user-editable Appearance token
       with a 2px setting (lib/settings/AppearanceTab.svelte:103-106). */
    min-width: calc(var(--iconbtn-glyph) + var(--border-weight) * 2);
    min-height: calc(var(--iconbtn-glyph) + var(--border-weight) * 2);
    transition:
      color 0.12s ease,
      border-color 0.12s ease;
  }

  /* The glyph is a flex item and shrinks by default; pinning it is the other half of
     never rendering it at 0px. */
  .iconbtn > :global(svg) {
    flex: 0 0 auto;
  }

  .iconbtn-outline {
    border-color: var(--color-border);
  }

  .iconbtn-surface {
    border-color: var(--color-border);
    background: var(--color-surface);
  }

  .iconbtn-accent {
    color: var(--color-accent);
  }

  .iconbtn-live {
    color: var(--color-live);
  }

  /* Hover moves the edge and never the ground. A translucent ground wash pulls the
     ground toward mid luminance, and --color-live and --color-accent sit there, so
     they are the tones it costs contrast; the direction reverses between modes
     because --color-text is near-white in one and near-black in the other, which is
     why no single percentage holds. An edge is outside the glyph, so hovering does
     not change what the glyph is measured against; only the two rules below move a
     glyph's own color, and then deliberately. --color-text measures 7.00:1 at worst
     over the sweep described above. */
  .iconbtn:hover:not(:disabled) {
    border-color: var(--color-text);
  }

  /* The destructive mark rides the edge, so it holds whatever the glyph's tone is.
     --color-live undiluted: it is already the weakest edge in that sweep at 2.65:1
     (Slate's selected row, light mode), and mixing it with transparent composites it
     toward the ground from there. */
  .iconbtn.iconbtn-danger:hover:not(:disabled) {
    border-color: var(--color-live);
  }

  /* Only an untoned glyph moves on hover; a toned one keeps its tone so hovering an
     engaged toggle does not read as switching it off. Exactly one tone class is
     emitted, so a toned button carries no `iconbtn-default` and neither this rule
     nor the danger one below can match it. */
  .iconbtn.iconbtn-default:hover:not(:disabled) {
    color: var(--color-text);
  }

  .iconbtn.iconbtn-default.iconbtn-danger:hover:not(:disabled) {
    color: var(--color-live);
  }

  /* No text label, so the ring is the only thing telling a keyboard user which
     control they are about to press. */
  .iconbtn:focus-visible {
    outline: calc(var(--border-weight) * 2) solid var(--iconbtn-ring, var(--color-accent));
    outline-offset: 1px;
  }

  /* The criterion here is agreement across a row, not what the button paints at
     rest: CanvasDestinationsTab.svelte:206-226 puts a live-toned Button and a
     `danger` IconButton side by side, and a ring that changed color between them
     would read as two different kinds of focus. `danger` earns its place on that
     alone -- unlike the other two it carries no color at rest (dim glyph, border
     edge, live only on hover), and five call sites take it. The list therefore does
     not match Button's: Button has no `danger` and spells destructive as `live`, so
     the two are different predicates rather than one list transcribed twice. */
  .iconbtn-accent,
  .iconbtn-live,
  .iconbtn-danger {
    --iconbtn-ring: var(--color-text);
  }

  @media (prefers-reduced-motion: reduce) {
    .iconbtn {
      transition: none;
    }
  }
</style>
