<script lang="ts">
  import type { OAuthProviderField } from "$lib/api/bridge";
  // The id→display-name map the chat/event surfaces already key by providerId, rather
  // than a second table: its labels are the providers' own displayName() strings.
  import { PLATFORM_LABELS, platformKey } from "$lib/theme/platformColors";
  import TagsInput from "$lib/ui/TagsInput.svelte";

  // One provider descriptor, spread across the tag control's limit props. The limits are
  // enforced at the moment a tag arrives, because the provider that enforces them refuses
  // the WHOLE metadata push over one bad tag and that refusal blocks the go-live — so the
  // first sign of a rule broken while typing must not be a stream that never started.
  //
  // Spread key by key rather than handed over as one object: the prop names ARE the
  // descriptor's own key names, so a provider that renames one fails to compile here
  // instead of quietly enforcing nothing.
  //
  // Adding a limit, though, is an edit here and there is no way around it — the mapping is
  // what turns a descriptor into four separate props, which is the shape this control is
  // meant to have. A new limit is named in TagLimits, listed below, and enforced in
  // admitTags; miss this file and the control simply never receives it.
  interface Props {
    field: OAuthProviderField;
    values: string[] | undefined;
    onChange: (next: string[]) => void;
    onReset?: () => void;
    inheritedValues?: string[];
    /** Whose rules `field` states. Names the platform in every sentence about a limit. */
    providerId?: string;
    accent?: boolean;
    disabled?: boolean;
  }
  let {
    field,
    values,
    onChange,
    onReset,
    inheritedValues,
    providerId = "",
    accent = false,
    disabled = false,
  }: Props = $props();
</script>

<TagsInput
  {values}
  {onChange}
  {onReset}
  {inheritedValues}
  {accent}
  {disabled}
  maxTags={field.maxTags}
  maxTagChars={field.maxTagChars}
  maxTotalChars={field.maxTotalChars}
  tagCharset={field.tagCharset}
  platform={PLATFORM_LABELS[platformKey(providerId)] ?? ""}
/>
