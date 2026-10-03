<script lang="ts">
  // The inspector's head while a variation is selected: its name and the condition that
  // makes it play. The condition is edited as a sentence -- "plays when [amount] [is at
  // least] [1000]" -- over the fields its event declares, so it can only name something the
  // event carries. A condition the resolver cannot evaluate says why here, in the same words
  // the rail's warning uses. A money amount carries its currency beside it: with no exchange
  // rate, "at least 20" means 20 of one currency, so a variation per currency is the model.
  import Button from "$lib/ui/Button.svelte";
  import Icon from "$lib/ui/Icon.svelte";
  import { PLATFORM_ORDER, platformName } from "$lib/theme/platformColors";
  import {
    CONDITION_FIELDS,
    conditionCurrency,
    conditionLabel,
    conditionProblem,
    isMoneyScope,
    tierNumber,
    type AlertEventScope,
    type AlertVariation,
    type VariationCondition,
  } from "../../../overlay/alertScopes";
  import { startingCondition } from "./scopeEdit";

  let {
    variation,
    event,
    onMeta,
    onDelete,
  }: {
    variation: AlertVariation;
    event: AlertEventScope;
    onMeta: (patch: { when?: VariationCondition; label?: string }) => void;
    onDelete: () => void;
  } = $props();

  const OP_WORDS: Record<string, string> = { ">=": "is at least", "==": "is exactly" };
  /** Offered in the currency box; any other ISO 4217 code can be typed. */
  const COMMON_CURRENCIES = ["USD", "EUR", "GBP", "INR", "JPY", "CAD", "AUD", "BRL", "MXN", "KRW"];

  const uid = $props.id();
  const when = $derived(variation.when);
  const spec = $derived(Object.hasOwn(CONDITION_FIELDS, when.field) ? CONDITION_FIELDS[when.field] : null);
  const problem = $derived(conditionProblem(when, event));
  const money = $derived(isMoneyScope(event) && when.field === "amount");
  const currency = $derived(conditionCurrency(when));

  function setField(field: string): void {
    const next = startingCondition(field, event);
    if (next) {
      onMeta({ when: next });
    }
  }

  function setNumber(raw: string): void {
    const n = Number(raw);
    // An emptied box stores nothing usable rather than a 0 the user never typed; the problem
    // line then says the condition needs a number.
    const value = raw.trim() === "" || !Number.isFinite(n) ? null : money ? Math.round(n * 100) : n;
    onMeta({ when: { ...when, value } });
  }

  /** Stored upper-cased as typed, so a half-typed code shows the problem line until it is whole. */
  function setCurrency(raw: string): void {
    onMeta({ when: { ...when, currency: raw.trim().toUpperCase() } });
  }

  /** "2" and "Tier 2" store as the number; anything else (a membership level name) as text. */
  function setTier(raw: string): void {
    const n = tierNumber(raw.trim()) ?? (/^\d+$/.test(raw.trim()) ? Number(raw.trim()) : null);
    onMeta({ when: { ...when, value: n ?? raw } });
  }

  const shownNumber = $derived(
    typeof when.value === "number" ? String(money ? when.value / 100 : when.value) : "",
  );
</script>

<div class="vh">
  <div class="vh__row">
    <label class="vh__label" for="{uid}-name">Name</label>
    <input
      id="{uid}-name"
      class="vh__name"
      type="text"
      value={variation.label ?? ""}
      placeholder={conditionLabel(variation, event)}
      oninput={(e) => onMeta({ label: e.currentTarget.value })}
    />
    <Button size="sm" tone="live" onclick={onDelete}>Delete variation</Button>
  </div>

  <fieldset class="vh__cond">
    <legend class="vh__legend">Plays instead of the {event.label} alert when</legend>
    <select
      class="cv-select vh__sel"
      aria-label="Condition"
      value={when.field}
      onchange={(e) => setField(e.currentTarget.value)}
    >
      {#each event.conditions as c (c)}
        {#if Object.hasOwn(CONDITION_FIELDS, c)}
          <option value={c}>{CONDITION_FIELDS[c].label}</option>
        {/if}
      {/each}
    </select>
    {#if spec && spec.ops.length > 1}
      <select
        class="cv-select vh__sel"
        aria-label="Comparison"
        value={when.op}
        onchange={(e) => onMeta({ when: { ...when, op: e.currentTarget.value } })}
      >
        {#each spec.ops as op (op)}
          <option value={op}>{OP_WORDS[op]}</option>
        {/each}
      </select>
    {/if}
    {#if spec?.valueKind === "number"}
      <input
        class="vh__val"
        type="number"
        min="0"
        step={money ? "0.01" : "1"}
        aria-label={money ? `Amount, in ${currency ?? "the chosen currency"}` : spec.label}
        value={shownNumber}
        oninput={(e) => setNumber(e.currentTarget.value)}
      />
      {#if money}
        <input
          class="vh__cur"
          type="text"
          maxlength="3"
          list="{uid}-currencies"
          aria-label="Currency"
          placeholder="USD"
          value={typeof when.currency === "string" ? when.currency : ""}
          oninput={(e) => setCurrency(e.currentTarget.value)}
        />
        <datalist id="{uid}-currencies">
          {#each COMMON_CURRENCIES as c (c)}
            <option value={c}></option>
          {/each}
        </datalist>
      {/if}
    {:else if spec?.valueKind === "tier"}
      <input
        class="vh__val"
        type="text"
        aria-label="Tier"
        placeholder="2, or a membership level"
        value={when.value == null ? "" : String(when.value)}
        oninput={(e) => setTier(e.currentTarget.value)}
      />
    {:else if spec?.valueKind === "platform"}
      <select
        class="cv-select vh__sel"
        aria-label={spec.label}
        value={typeof when.value === "string" ? when.value : ""}
        onchange={(e) => onMeta({ when: { ...when, op: "==", value: e.currentTarget.value } })}
      >
        {#each PLATFORM_ORDER as p (p)}
          <option value={p}>is {platformName(p)}</option>
        {/each}
      </select>
    {:else if spec?.valueKind === "boolean"}
      <select
        class="cv-select vh__sel"
        aria-label={spec.label}
        value={when.value === true ? "yes" : "no"}
        onchange={(e) => onMeta({ when: { ...when, op: "==", value: e.currentTarget.value === "yes" } })}
      >
        <option value="yes">it is a gift</option>
        <option value="no">it is not a gift</option>
      </select>
    {/if}
  </fieldset>

  {#if money}
    <p class="vh__note">
      Plays only for {event.label} paid in {currency ?? "this currency"}. No exchange rate is applied, so add one
      variation per currency.
    </p>
  {/if}
  {#if problem}
    <p class="vh__problem"><Icon name="warn" size={12} /> This variation never plays: {problem}</p>
  {/if}
</div>

<style>
  .vh {
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding: 10px;
    border: var(--border-weight) solid var(--color-border);
    background: var(--color-surface);
  }
  .vh__row {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .vh__label,
  .vh__legend {
    font-size: 11px;
    color: var(--color-dim);
  }
  .vh__name {
    flex: 1;
    min-width: 0;
  }
  .vh__cond {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 6px;
    margin: 0;
    padding: 0;
    border: 0;
  }
  .vh__legend {
    float: left;
    margin-right: 4px;
    padding: 0;
  }
  .vh__sel {
    width: auto;
    max-width: none;
    height: var(--control-height);
  }
  .vh__val {
    width: 110px;
  }
  .vh__cur {
    width: 64px;
    text-transform: uppercase;
  }
  .vh__note {
    margin: 0;
    font-size: 11px;
    color: var(--color-muted);
  }
  .vh__problem {
    margin: 0;
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    color: var(--color-warn);
  }
</style>
