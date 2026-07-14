<script lang="ts">
  import type { CanvasForm } from "$lib/canvas/canvasForm";
  import type { EncoderType } from "$lib/api/bridge";
  import UseDefaultStrip from "$lib/canvas/UseDefaultStrip.svelte";
  import PropertyForm from "$lib/properties/PropertyForm.svelte";

  interface Props {
    form: CanvasForm;
    canvasUuid: string;
    videoEncoders: EncoderType[];
    isLive: boolean;
    isDefault: boolean;
    commit: () => void;
    commitNow: () => Promise<void>;
  }
  let { form, canvasUuid, videoEncoders, isLive, isDefault, commit, commitNow }: Props = $props();

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
      disabled={isLive}
      onchange={(v) => {
        form.videoUseDefault = v;
        commit();
      }}
    />
  {/if}

  {#if form.videoUseDefault && !isDefault}
    <p class="cv-inherit-note">This canvas inherits the Default canvas's video encoder.</p>
  {:else}
    <div class="cv-field">
      <div class="cv-field__l">
        Video Encoder
        <span class="cv-field__sub">Fields below are supplied by the selected encoder.</span>
      </div>
      <select class="cv-select" value={form.videoEnc} disabled={isLive} onchange={onEncoder}>
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
  {/if}
</div>
