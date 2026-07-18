<script lang="ts">
  import { obs, type PropertyKind, type PropertyDescriptor } from "$lib/api/bridge";
  import PropertyRow from "$lib/properties/PropertyRow.svelte";

  interface Props {
    kind: PropertyKind;
    ref: string;
    // Property names to omit from the rendered form (e.g. a control promoted to a
    // parent component). Excluded rows are still fetched and pushed normally; only
    // their row is hidden.
    exclude?: string[];
  }
  let { kind, ref, exclude }: Props = $props();

  let descriptors = $state<PropertyDescriptor[]>([]);
  let values = $state<Record<string, unknown>>({});
  let loaded = $state(false);
  let error = $state<string | null>(null);

  // Settings snapshot taken on the first successful load after the target changes,
  // so a Properties dialog wrapper can revert to it on Cancel/Esc. Never overwritten
  // by a later re-fetch (flush/restoreDefaults) — only fetchProps' target-change path
  // resets it, and only once per target.
  let initialValues: Record<string, unknown> = {};
  let hasSnapshot = false;

  // Descriptors minus any names a parent promoted out of this form (e.g. the
  // service picker). Excluded rows are still fetched/pushed; only hidden here.
  const visibleDescriptors = $derived(
    exclude?.length ? descriptors.filter((d) => !exclude.includes(d.name)) : descriptors,
  );

  // Resolve a property's live value from the flat settings map. The settings map
  // is the source of truth across re-fetch, but it can omit a key (or carry a
  // null) the first time a form loads; fall back to the descriptor's own default
  // so a control never renders blank/zero when a real default exists.
  function lookup(name: string): unknown {
    const v = values[name];
    if (v !== undefined && v !== null) return v;
    const d = descriptors.find((desc) => desc.name === name) as { value?: unknown } | undefined;
    return d?.value ?? v;
  }

  async function fetchProps() {
    error = null;
    try {
      const r = await obs.call("properties.get", { kind, ref });
      descriptors = r.props;
      values = r.values ?? {};
      if (!hasSnapshot) {
        initialValues = { ...values };
        hasSnapshot = true;
      }
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
    hasSnapshot = false;
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

  // Revert to the open-time snapshot. properties.set MERGES settings, so pushing the
  // full snapshot back overwrites every key the form could have touched — nothing can
  // be left orphaned. Cancel any pending debounced edit first so it can't re-apply
  // after the revert lands.
  export async function revert() {
    if (timer) {
      clearTimeout(timer);
      timer = null;
    }
    pending = {};
    try {
      const r = await obs.call("properties.set", { kind, ref, settings: initialValues });
      descriptors = r.props;
      values = r.values ?? {};
    } catch (e) {
      error = (e as Error).message;
    }
  }

  // Reset to the type's registered defaults. Does not touch initialValues, so a
  // subsequent Cancel can still revert past this back to the dialog's open state.
  export async function restoreDefaults() {
    try {
      const r = await obs.call("properties.defaults", { kind, ref });
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
  {:else if visibleDescriptors.length === 0}
    <p class="dim">No properties</p>
  {:else}
    <div class="rows">
      {#each visibleDescriptors as prop (prop.name)}
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
