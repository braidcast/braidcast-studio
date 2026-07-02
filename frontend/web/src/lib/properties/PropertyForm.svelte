<script lang="ts">
  import { obs, type PropertyKind, type PropertyDescriptor } from "../bridge";
  import PropertyRow from "./PropertyRow.svelte";

  interface Props {
    kind: PropertyKind;
    ref: string;
  }
  let { kind, ref }: Props = $props();

  let descriptors = $state<PropertyDescriptor[]>([]);
  let values = $state<Record<string, unknown>>({});
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Resolve a property's live value from the flat settings map; descriptors also
  // carry a `value`, but the settings map is the source of truth across re-fetch.
  function lookup(name: string): unknown {
    return values[name];
  }

  async function fetchProps() {
    error = null;
    try {
      const r = await obs.call("properties.get", { kind, ref });
      descriptors = r.props;
      values = r.values ?? {};
    } catch (e) {
      error = (e as Error).message;
    } finally {
      loaded = true;
    }
  }

  // Re-fetch whenever the target changes (selecting a different source).
  $effect(() => {
    void kind;
    void ref;
    loaded = false;
    void fetchProps();
  });

  // Debounce settings pushes: coalesce rapid edits (sliders, typing) into one
  // properties.set, then adopt the re-fetched schema+values so dynamic
  // visibility/enabled/option changes from modified-callbacks reflect.
  let pending: Record<string, unknown> = {};
  let timer: ReturnType<typeof setTimeout> | null = null;

  async function flush() {
    timer = null;
    const settings = pending;
    pending = {};
    if (Object.keys(settings).length === 0) return;
    try {
      const r = await obs.call("properties.set", { kind, ref, settings });
      descriptors = r.props;
      values = r.values ?? {};
    } catch (e) {
      error = (e as Error).message;
    }
  }

  function onChange(name: string, value: unknown) {
    // Optimistic local update so the bound control stays responsive pre-flush.
    values = { ...values, [name]: value };
    pending[name] = value;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => void flush(), 150);
  }

  async function onButton(name: string) {
    try {
      const r = await obs.call("properties.button", { kind, ref, prop: name });
      descriptors = r.props;
      values = r.values ?? {};
    } catch (e) {
      error = (e as Error).message;
    }
  }
</script>

<div class="prop-form">
  {#if error}
    <p class="error">{error}</p>
  {:else if !loaded}
    <p class="dim">Loading properties…</p>
  {:else if descriptors.length === 0}
    <p class="dim">No properties</p>
  {:else}
    <div class="rows">
      {#each descriptors as prop (prop.name)}
        <PropertyRow {prop} value={lookup(prop.name)} {onChange} {onButton} {lookup} />
      {/each}
    </div>
  {/if}
</div>

<style>
  .prop-form {
    display: flex;
    flex-direction: column;
    gap: 14px;
    min-width: 0;
  }
  .rows {
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .dim {
    color: var(--color-muted);
    margin: 0;
  }
  .error {
    color: var(--color-live);
    margin: 0;
    font-size: 12px;
  }
</style>
