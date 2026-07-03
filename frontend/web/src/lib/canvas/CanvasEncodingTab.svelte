<script lang="ts">
  import type { CanvasForm } from "./canvasForm";
  import type { EncoderType } from "../bridge";
  import UseDefaultStrip from "./UseDefaultStrip.svelte";
  import PropertyForm from "../properties/PropertyForm.svelte";

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

<div class="body">
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
    <p class="inherit-note">This canvas inherits the Default canvas's video encoder.</p>
  {:else}
    <div class="field">
      <span class="flabel">Video Encoder</span>
      <select value={form.videoEnc} disabled={isLive} onchange={onEncoder}>
        {#each videoEncoders as e (e.id)}
          <option value={e.id}>{e.name}</option>
        {/each}
      </select>
    </div>
    {#if form.videoEnc}
      <div class="subhead">Encoder Properties</div>
      {#key form.videoEnc}
        <PropertyForm kind="encoder" ref={`${canvasUuid}:video`} />
      {/key}
    {/if}
  {/if}
</div>

<style>
  .body {
    display: block;
  }
  .field {
    margin-bottom: 12px;
  }
  .flabel {
    display: block;
    font-size: 12px;
    color: var(--color-text);
    margin-bottom: 6px;
  }
  select {
    background: var(--color-base);
    border: 1px solid var(--color-border);
    padding: 7px 10px;
    color: var(--color-text);
    font: inherit;
    width: 100%;
    max-width: 420px;
  }
  select:focus {
    outline: none;
    border-color: var(--color-accent);
  }
  select:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .subhead {
    margin: 16px 0 12px;
    font-family: var(--font-mono);
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--color-dim);
    padding-bottom: 8px;
    border-bottom: var(--border-weight) solid var(--color-border-2);
  }
  .inherit-note {
    margin: 0;
    font-size: 12px;
    color: var(--color-muted);
  }
</style>
