<script lang="ts">
  // The value control for a `media` field: an image (PNG, JPG, GIF, APNG, WebP) or a WebM
  // video shown on the alert card. The stored value is "" or "assets/<file>". The thumbnail
  // is a still -- the first frame of an animation, the first decoded frame of a video -- so
  // the inspector never loops motion beside the stage that is already playing it.
  import type { OverlayAsset } from "$lib/api/bridge";
  import Button from "$lib/ui/Button.svelte";
  import { MEDIA_ACCEPT } from "$lib/overlays/fieldTypes";
  import { assetFileOf, assetPreviewUrl, formatBytes } from "$lib/overlays/scopes/scopedAssets";

  let {
    value,
    name,
    assets,
    widgetUrl,
    uploading,
    ariaDescribedBy,
    onUpload,
    onChange,
  }: {
    value: string;
    name: string;
    assets: OverlayAsset[];
    widgetUrl: string;
    uploading: boolean;
    ariaDescribedBy?: string;
    onUpload: (file: File) => void;
    onChange: (next: string) => void;
  } = $props();

  const VIDEO = /\.webm$/i;
  const FORMAT: Record<string, string> = { png: "PNG", apng: "APNG", jpg: "JPEG", jpeg: "JPEG", gif: "GIF", webp: "WebP", webm: "WebM video" };

  const file = $derived(assetFileOf(value));
  const url = $derived(file ? assetPreviewUrl(widgetUrl, file) : "");
  const isVideo = $derived(!!file && VIDEO.test(file));
  const record = $derived(file ? assets.find((a) => a.file === file) : undefined);
  const format = $derived(file ? (FORMAT[file.slice(file.lastIndexOf(".") + 1).toLowerCase()] ?? "File") : "");

  let fileInput = $state<HTMLInputElement | null>(null);
  let canvas = $state<HTMLCanvasElement | null>(null);
  let size = $state<{ w: number; h: number } | null>(null);
  let broken = $state(false);

  // Drawn once per value: an <img> of a GIF keeps animating, a canvas painted from it holds
  // the frame it was painted with.
  $effect(() => {
    size = null;
    broken = false;
    if (!url || isVideo || !canvas) {
      return;
    }
    const target = canvas;
    const img = new Image();
    img.onload = () => {
      size = { w: img.naturalWidth, h: img.naturalHeight };
      const scale = Math.min(target.width / img.naturalWidth, target.height / img.naturalHeight, 1);
      const w = img.naturalWidth * scale;
      const h = img.naturalHeight * scale;
      const g = target.getContext("2d");
      g?.clearRect(0, 0, target.width, target.height);
      g?.drawImage(img, (target.width - w) / 2, (target.height - h) / 2, w, h);
    };
    img.onerror = () => (broken = true);
    img.src = url;
    return () => {
      img.onload = null;
      img.onerror = null;
    };
  });

  const line = $derived.by(() => {
    if (!file) {
      return "No media. The alert shows text only.";
    }
    if (broken) {
      return "This file can't be shown. The alert will show text only.";
    }
    const parts = [format];
    if (size) {
      parts.push(`${size.w}×${size.h}`);
    }
    if (record?.bytes) {
      parts.push(formatBytes(record.bytes));
    }
    return parts.join(" · ");
  });
</script>

<div class="mp">
  <div class="mp__thumb" aria-hidden="true">
    {#if file && isVideo}
      <video
        src={url}
        muted
        preload="auto"
        playsinline
        onloadeddata={(e) => (size = { w: e.currentTarget.videoWidth, h: e.currentTarget.videoHeight })}
        onerror={() => (broken = true)}
      ></video>
    {:else if file}
      <canvas bind:this={canvas} width="96" height="64"></canvas>
    {:else}
      <span class="mp__none">None</span>
    {/if}
  </div>
  <div class="mp__side">
    <p class="mp__line">{line}</p>
    <div class="mp__actions">
      <input
        bind:this={fileInput}
        class="mp__file"
        type="file"
        accept={MEDIA_ACCEPT}
        tabindex="-1"
        aria-hidden="true"
        onchange={(e) => {
          const f = e.currentTarget.files?.[0];
          if (f) {
            onUpload(f);
          }
          e.currentTarget.value = "";
        }}
      />
      <Button
        size="sm"
        variant="surface"
        disabled={uploading}
        aria-label="{file ? 'Replace' : 'Upload'} {name}"
        aria-describedby={ariaDescribedBy}
        onclick={() => fileInput?.click()}
      >
        {uploading ? "Uploading…" : file ? "Replace" : "Upload"}
      </Button>
      {#if file}
        <Button size="sm" variant="bare" aria-label="Clear {name}" onclick={() => onChange("")}>Clear</Button>
      {/if}
    </div>
  </div>
</div>

<style>
  .mp {
    display: flex;
    align-items: center;
    gap: 10px;
    width: 100%;
    min-width: 0;
  }
  /* A checkerboard, so a transparent GIF or an alpha WebM reads as transparent rather than
     as whatever colour sat behind it. */
  .mp__thumb {
    flex: 0 0 96px;
    height: 64px;
    display: flex;
    align-items: center;
    justify-content: center;
    border: var(--border-weight) solid var(--color-border);
    background-color: var(--color-base);
    background-image: conic-gradient(
      var(--color-surface-2) 25%,
      transparent 0 50%,
      var(--color-surface-2) 0 75%,
      transparent 0
    );
    background-size: 12px 12px;
    overflow: hidden;
  }
  .mp__thumb video {
    max-width: 100%;
    max-height: 100%;
  }
  .mp__none {
    font-family: var(--font-mono);
    font-size: 10px;
    color: var(--color-muted);
  }
  .mp__side {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .mp__line {
    margin: 0;
    font-size: 11px;
    color: var(--color-dim);
  }
  .mp__actions {
    display: flex;
    gap: 6px;
  }
  .mp__file {
    display: none;
  }
</style>
