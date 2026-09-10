<script lang="ts" module>
  import type { Snippet } from "svelte";
  import type { HTMLButtonAttributes } from "svelte/elements";

  // The resting box, low to high, the same three IconButton draws plus two the
  // text-button population needs and an icon-only control never does. Hover paints an
  // edge or a wash, so the variant decides what the button shows before it is pointed
  // at, not how it reacts.
  //   bare    - no edge at rest (inline row actions)
  //   outline - edge at rest
  //   surface - edge at rest over a filled surface
  //   dashed  - dashed edge at rest, the house mark for an "add another one" cell
  //   filled  - the accent block; see FilledOnly below for why it takes no tone
  export type ButtonVariant = "bare" | "outline" | "surface" | "dashed" | "filled";

  // Resting ink, and for the bordered variants the resting edge with it: every call
  // site that reddened a Retry or ambered a warning-strip action moved both together.
  //
  // `live` at rest inherits a known token-level gap: --color-live is not a 4.5:1 ink
  // on --color-surface in light mode (3.49:1), and no preset rescues it (4.25:1 at
  // worst in dark, Slate). Four destructive controls now take it at rest rather than
  // only on hover -- lib/pages/OverlaysPage.svelte:664,
  // lib/settings/StreamsTab.svelte:739, lib/settings/BrowserDocksTab.svelte:121,
  // lib/settings/HotkeysTab.svelte:222 -- where the resting ink used to be
  // --color-dim (6.91:1 light) or --color-text (16.55:1 light), both compliant. The
  // shortfall is the token's and predates this component; it is recorded here rather
  // than tuned, because closing it means moving --color-live.
  export type ButtonTone = "default" | "accent" | "live" | "warn";

  // Which typeface the label is set in.
  //   ui    - the UI face, as written (sentence case, no tracking)
  //   label - the UI face as chrome: tracked, and cased by --label-case
  //   mono  - the mono micro-label: tracked, cased, one step smaller
  // `label` and `mono` both take --label-case rather than a literal `uppercase`
  // because it is a user-editable Appearance token (lib/theme/tokens.ts:74) and it is
  // what `button.accent` (app.css:202-209) already reads. `ui` is the face to pick
  // when the label interpolates a name the user typed or a destination name; the
  // house precedent for holding --label-case off that kind of text is `.dock-label`
  // (app.css:476-489, reasoned in the comment above it).
  export type ButtonFace = "ui" | "label" | "mono";

  // The scale. The hand-picked heights and paddings this replaces collapse to these
  // three rungs (counted in braidcast-notes/frontend-component-audit.md, findings
  // section 2, at :153-205), so a call site names a rung and never a
  // measurement -- the numbers exist once, here, and reach the CSS as custom
  // properties rather than being restated in a per-rung rule.
  //   xs - inline actions inside a row or a field label
  //   sm - strip and card actions
  //   md - the form-control rung; the only one that tracks --control-height, so it
  //        is the one that still fits its row when density changes
  export type ButtonSize = "xs" | "sm" | "md";

  interface SizeSpec {
    height: string;
    padX: string;
    /** Label size for the UI face. */
    font: string;
    /** Mono micro-label face: uppercase with tracking reads larger at equal px. */
    monoFont: string;
  }

  const BUTTON_SIZES: Record<ButtonSize, SizeSpec> = {
    xs: { height: "24px", padX: "9px", font: "11px", monoFont: "9.5px" },
    sm: { height: "28px", padX: "11px", font: "11px", monoFont: "10px" },
    md: { height: "var(--control-height)", padX: "12px", font: "12px", monoFont: "10px" },
  };

  // --color-text rather than the --color-dim IconButton rests at: a glyph owes
  // SC 1.4.11's 3:1 and dim clears that, but a label is text and owes 4.5:1. Over
  // the four preset palettes plus the shared light palette, x five accents, against
  // --color-base/-rail/-surface/-surface-2 and each of those under a selected row's
  // accent wash, dim bottoms out at 3.56:1 and text at 6.30:1 (both Slate, amber,
  // selected row over --color-surface-2; Slate is the 22% wash because it ships
  // selectionStyle `fill` (lib/theme/presets.ts:174, app.css:461-463) where the
  // other three use 12% (app.css:457-460)). Switching mode rewrites the nine
  // neutrals wholesale (lib/theme/themeStore.svelte.ts:65-68), so light mode is one
  // palette rather than four.
  const TONE_INK: Record<ButtonTone, string> = {
    default: "var(--color-text)",
    accent: "var(--color-accent)",
    live: "var(--color-live)",
    warn: "var(--color-warn)",
  };

  const TONE_EDGE: Record<ButtonTone, string> = {
    default: "var(--color-border)",
    accent: "var(--color-accent)",
    live: "var(--color-live)",
    warn: "var(--color-warn)",
  };

  interface ButtonBase extends Omit<HTMLButtonAttributes, "class" | "style" | "children"> {
    /** The label. A text button is named by what it renders, so it is required. */
    children: Snippet;
    size?: ButtonSize;
    face?: ButtonFace;
    /** Fill the inline axis of whatever lays this out, instead of sizing to the
     * label. Owned here because the alternative -- a parent rule reaching in with
     * `:global(button) { flex: 1 }` -- loses on specificity from an unscoped
     * stylesheet, silently. */
    grow?: boolean;
  }

  // A tone paints ink and edge in one of the four semantic colors; a fill needs the
  // matching ink to draw *on* that color, and only the accent has one
  // (--color-accent-ink, app.css:72; --color-warn-ink at app.css:84 exists but is
  // scoped to warn's own surfaces, and there is no live equivalent). So `filled` is
  // the accent block or it is nothing, and the type says so rather than accepting a
  // `tone` it would drop.
  type Toned = { variant?: Exclude<ButtonVariant, "filled">; tone?: ButtonTone };
  type FilledOnly = { variant: "filled"; tone?: never };

  export type ButtonProps = ButtonBase & (Toned | FilledOnly);
</script>

<script lang="ts">
  let {
    children,
    size = "md",
    variant = "outline",
    tone = "default",
    face = "ui",
    grow = false,
    type = "button",
    ...rest
  }: ButtonProps = $props();

  const spec = $derived(BUTTON_SIZES[size]);
</script>

<button
  {...rest}
  {type}
  class="btnx btnx-{variant} btnx-face-{face}"
  class:btnx-grow={grow}
  class:btnx-toned={tone !== "default"}
  style:--btnx-h={spec.height}
  style:--btnx-pad={spec.padX}
  style:--btnx-font={face === "mono" ? spec.monoFont : spec.font}
  style:--btnx-ink={TONE_INK[tone]}
  style:--btnx-edge={TONE_EDGE[tone]}
>
  {@render children()}
</button>

<style>
  /* The global `button` rule (app.css:183-191) sets `padding: 0 12px`, a fixed
     `height`, a surface background and a border. All four are declared here, so a
     rung's numbers describe the whole box rather than layering over whichever of the
     four a call site remembered to cancel; the rule blocks that did so are counted
     in braidcast-notes/frontend-component-audit.md, findings section 2 (:153-205).
     Its `cursor: pointer` (app.css:185) is deliberately left inherited, which is
     what lets `button:disabled` (app.css:197-200) still land its `cursor: default`,
     and its `opacity` -- the app's one disabled idiom, shared with IconButton and
     every raw button in the tree. */
  .btnx {
    flex: 0 0 auto;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    padding: 0 var(--btnx-pad);
    height: var(--btnx-h);
    background: transparent;
    /* Every variant reserves the same border box and varies only its color, so the
       content box does not move when a variant paints a visible edge. */
    border: var(--border-weight) solid transparent;
    color: var(--btnx-ink);
    font-family: var(--font-ui);
    font-size: var(--btnx-font);
    line-height: 1.2;
    white-space: nowrap;
    /* Under the global `box-sizing: border-box` (app.css:112-115) a declared height
       is the outer box, so the line box plus both borders is the floor below which
       the label no longer fits inside it. Derived from --border-weight rather than a
       constant: it is a user-editable Appearance token with a 2px setting
       (lib/settings/AppearanceTab.svelte:103-106). */
    min-height: calc(var(--btnx-font) * 1.2 + var(--border-weight) * 2);
    transition:
      color 0.12s ease,
      border-color 0.12s ease,
      background 0.12s ease;
  }

  /* An icon beside the label would be a flex item, and a flex item shrinks by
     default -- the same collapse IconButton's glyph had to be pinned against. */
  .btnx > :global(svg) {
    flex: 0 0 auto;
  }

  /* Both axes because the parents that ask for this are a mix: a flex strip wants
     the button to take an equal share, a block panel wants it to span the padding
     box. In a flex row the `flex` basis governs and `width` is inert; in a block
     parent the reverse. In a flex COLUMN the basis is the height, so this stretches
     the button down the column rather than across it -- OverlaysPage.svelte:851-855
     is such a parent, and its add affordance is the one of the four that must not
     take this prop.

     Must stay below `.btnx`: both compile to (0,2,0), so source order is the only
     thing deciding which `flex` wins. */
  .btnx-grow {
    flex: 1 1 0;
    width: 100%;
  }

  .btnx-face-label {
    text-transform: var(--label-case);
    letter-spacing: var(--letter-spacing);
  }

  .btnx-face-mono {
    font-family: var(--font-mono);
    text-transform: var(--label-case);
    letter-spacing: var(--letter-spacing);
  }

  .btnx-outline {
    border-color: var(--btnx-edge);
  }

  .btnx-surface {
    border-color: var(--btnx-edge);
    background: var(--color-surface);
  }

  .btnx-dashed {
    border-color: var(--btnx-edge);
    border-style: dashed;
  }

  /* The accent block. The border box the shared rule reserves stays transparent and
     the fill paints under it (background-clip is border-box), so the block is the
     same outer size as every other variant. */
  .btnx-filled {
    background: var(--color-accent);
    color: var(--color-accent-ink);
    font-weight: 600;
  }

  /* Untoned: hover moves the edge and never the ink. --color-text rather than the
     --color-accent most of the migrated call sites hovered to: an edge is outside
     the label, so hovering does not change what the label is measured against,
     whereas recoloring the label to the accent would put it at 1.61:1 in light mode
     (worst cell: light, amber, over --color-rail). */
  .btnx:not(.btnx-toned, .btnx-filled):hover:not(:disabled) {
    border-color: var(--color-text);
  }

  /* Toned: the edge already carries the tone, so moving it to --color-text would
     read as the button having stopped being a retry or a warning. The wash is what
     each of the three rules it replaces already used, at this same 14%, and it
     leaves ink and edge alone. It is the one hover here that moves what the label
     is measured against, so its cost was swept: over the four preset palettes plus
     the shared light palette, x five accents, against --color-base/-rail/-surface/
     -surface-2, the washed label is at worst 0.08 below the same label on the
     unwashed ground (warn, light mode, over --color-rail: 1.39:1 washed against
     1.47:1 plain -- an ink already below 3:1 in light mode before any wash). */
  .btnx-toned:hover:not(:disabled) {
    background: color-mix(in srgb, var(--btnx-ink) 14%, transparent);
  }

  /* The fill is already the strongest thing in its row, so its hover darkens the
     block rather than adding anything to it. */
  .btnx-filled:hover:not(:disabled) {
    background: color-mix(in srgb, var(--color-accent) 88%, var(--color-text));
  }

  /* Accent by default, matching IconButton. */
  .btnx:focus-visible {
    outline: calc(var(--border-weight) * 2) solid var(--btnx-ring, var(--color-accent));
    outline-offset: 1px;
  }

  /* When the button's own fill or edge already carries a color, an accent ring one
     pixel outside it differs from no ring by a hairline gap. Those two take the ring
     in --color-text instead. It is drawn on whatever ground the button sits on, and
     across the three these reach -- --color-surface, --color-surface-2, and the warn
     wash at GoLiveModal.svelte:2502 -- the worst is 8.55:1 (Industrial, on the
     wash), many times the 3:1 a non-text indicator owes. It also cannot be mistaken
     for the button's own edge, which is the point. */
  .btnx-filled,
  .btnx-toned {
    --btnx-ring: var(--color-text);
  }

  @media (prefers-reduced-motion: reduce) {
    .btnx {
      transition: none;
    }
  }
</style>
