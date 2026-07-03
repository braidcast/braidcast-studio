<script lang="ts">
  import type { PropertyDescriptor } from "../bridge";
  import { controlFor } from "./controls";

  interface Props {
    prop: PropertyDescriptor;
    value: unknown;
    onChange: (name: string, value: unknown) => void;
    onButton: (name: string) => void;
    lookup: (name: string) => unknown;
  }
  let { prop, value, onChange, onButton, lookup }: Props = $props();

  const Control = $derived(controlFor(prop.type));

  // Info text renders its own full-width note.
  const isInfo = $derived(
    prop.type === "text" && (prop as { text_type?: string }).text_type === "info",
  );
  // Group/button/info own their full-width chrome (legend, note, button label).
  const isFull = $derived(prop.type === "group" || prop.type === "button" || isInfo);
  // Booleans render as a label-left / toggle-right row (the mock's tglField).
  const isRow = $derived(prop.type === "bool");

  const label = $derived(prop.label ?? prop.name);
  const hint = $derived(prop.long_description ?? "");
</script>

{#if prop.visible}
  {#if isFull}
    <div class="cv-field cv-field--full">
      <Control {prop} {value} {onChange} {onButton} {lookup} />
    </div>
  {:else if isRow}
    <div class="cv-field cv-field--row">
      <div class="cv-field__l">
        {label}
        {#if hint}<span class="cv-field__sub">{hint}</span>{/if}
      </div>
      <div class="cv-field__c cv-field__c--end">
        <Control {prop} {value} {onChange} {onButton} {lookup} />
      </div>
    </div>
  {:else}
    <div class="cv-field">
      <div class="cv-field__l">{label}</div>
      <div class="cv-field__c">
        <Control {prop} {value} {onChange} {onButton} {lookup} />
      </div>
      {#if hint}<div class="cv-field__h">{hint}</div>{/if}
    </div>
  {/if}
{/if}
