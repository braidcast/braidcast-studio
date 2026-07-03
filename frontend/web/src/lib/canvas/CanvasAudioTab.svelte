<script lang="ts">
  import type { CanvasForm } from "./canvasForm";
  import type { EncoderType } from "../bridge";
  import UseDefaultStrip from "./UseDefaultStrip.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";

  interface Props {
    form: CanvasForm;
    canvasUuid: string;
    audioEncoders: EncoderType[];
    isLive: boolean;
    isDefault: boolean;
    commit: () => void;
    commitNow: () => Promise<void>;
  }
  let { form, canvasUuid, audioEncoders, isLive, isDefault, commit, commitNow }: Props = $props();

  async function onEncoder(e: Event): Promise<void> {
    form.audioEnc = (e.currentTarget as HTMLSelectElement).value;
    await commitNow();
  }
</script>

<div class="cv-body">
  {#if !isDefault}
    <UseDefaultStrip
      checked={form.audioUseDefault}
      label="Use Default audio encoder"
      disabled={isLive}
      onchange={(v) => {
        form.audioUseDefault = v;
        commit();
      }}
    />
  {/if}

  {#if form.audioUseDefault && !isDefault}
    <p class="cv-inherit-note">This canvas inherits the Default canvas's audio encoder.</p>
  {:else}
    <div class="cv-field">
      <div class="cv-field__l">Audio Encoder</div>
      <select class="cv-select" value={form.audioEnc} disabled={isLive} onchange={onEncoder}>
        {#each audioEncoders as e (e.id)}
          <option value={e.id}>{e.name}</option>
        {/each}
      </select>
    </div>
    {#if form.audioEnc}
      <div class="cv-subhead">Encoder Properties</div>
      {#key form.audioEnc}
        <PropertyForm kind="encoder" ref={`${canvasUuid}:audio`} />
      {/key}
    {/if}

    <div class="cv-field">
      <div class="cv-field__l">Tracks</div>
      <div class="cv-tracks">
        <span class="cv-track on">Track 1 · Active</span>
        <span class="cv-track ghost">Tracks 2 – 6 · Planned</span>
      </div>
      <div class="cv-field__h">
        This canvas streams a single audio track. Per-track configuration (Track 1–6, independent bitrate and
        routing) is planned.
      </div>
    </div>
  {/if}
</div>
