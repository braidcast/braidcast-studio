<script lang="ts">
  import type { CanvasForm } from "$lib/canvas/canvasForm";
  import { groupLocked } from "$lib/canvas/canvasForm";
  import type { EncoderType } from "$lib/api/bridge";
  import UseDefaultStrip from "$lib/canvas/UseDefaultStrip.svelte";
  import PropertyForm from "$lib/properties/PropertyForm.svelte";
  import ToggleSwitch from "$lib/ui/ToggleSwitch.svelte";

  interface Props {
    form: CanvasForm;
    canvasUuid: string;
    videoEncoders: EncoderType[];
    videoBitrate: number;
    isLive: boolean;
    isDefault: boolean;
    commit: () => void;
    commitNow: () => Promise<void>;
  }
  let {
    form,
    canvasUuid,
    videoEncoders,
    videoBitrate,
    isLive,
    isDefault,
    commit,
    commitNow,
  }: Props = $props();

  const locked = $derived(groupLocked(form.videoUseDefault, isDefault, isLive));

  /** 0 when the target bitrate is unknown (the encoder slot inherits the Default, or
   * carries no bitrate key) -- the kbps figure is then omitted rather than shown as 0. */
  const floorKbps = $derived(
    videoBitrate > 0 ? Math.round((videoBitrate * form.dynamicBitrateFloorPct) / 100) : 0,
  );

  async function onEncoder(e: Event): Promise<void> {
    form.videoEnc = (e.currentTarget as HTMLSelectElement).value;
    await commitNow();
  }
</script>

<div class="cv-body">
  {#if !isDefault}
    <UseDefaultStrip
      checked={form.videoUseDefault}
      label="Use Default video encoder"
      inheritNote="The video encoder comes from the Default canvas."
      disabled={isLive}
      onchange={(v) => {
        form.videoUseDefault = v;
        commit();
      }}
    />
  {/if}

  <fieldset class="cv-lockgroup" disabled={locked}>
    <div class="cv-field">
      <div class="cv-field__l">
        Video Encoder
        <span class="cv-field__sub">Fields below are supplied by the selected encoder.</span>
      </div>
      <select class="cv-select" value={form.videoEnc} onchange={onEncoder}>
        {#each videoEncoders as e (e.id)}
          <option value={e.id}>{e.name}</option>
        {/each}
      </select>
    </div>
    {#if form.videoEnc}
      <div class="cv-subhead">Encoder Properties</div>
      {#key form.videoEnc}
        <PropertyForm kind="encoder" ref={`${canvasUuid}:video`} />
      {/key}
    {/if}
  </fieldset>

  <!-- Outside the lock group on purpose: adaptive bitrate reaches the output's settings,
       never the canvas mix or its encoders, so it stays editable while live. -->
  <div class="cv-subhead">Adaptive Bitrate</div>
  <div class="cv-field">
    <div class="cv-field__l">
      Reduce bitrate on congestion
      <span class="cv-field__sub">
        Lower this canvas's bitrate instead of dropping frames when the connection stalls.
        Applies to every output bound to this canvas, from its next start.
      </span>
    </div>
    <ToggleSwitch
      size="sm"
      checked={form.dynamicBitrate}
      onchange={(v) => {
        form.dynamicBitrate = v;
        commit();
      }}
    />
  </div>

  {#if form.dynamicBitrate}
    <div class="cv-field">
      <div class="cv-field__l">
        Never drop below
        <span class="cv-field__sub">
          Share of this canvas's video bitrate. At 0 the rate follows measured throughput
          with no lower bound, so one stalled second can gut quality for minutes.
        </span>
      </div>
      <div class="cv-num">
        <input
          type="number"
          min="0"
          max="100"
          bind:value={form.dynamicBitrateFloorPct}
          aria-label="Minimum bitrate as a percentage of this canvas's target"
          onchange={commit}
        />
        <span class="cv-num__u">{floorKbps ? `% = ${floorKbps.toLocaleString()} kbps` : "%"}</span>
      </div>
    </div>
  {/if}
</div>
