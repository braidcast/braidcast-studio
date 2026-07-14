import type { CopiedFilter, Transform } from "$lib/api/bridge";

// Editor clipboard for copy/paste of sources, filter chains, and transforms.
// The source clipboard stores the identity needed to paste a REFERENCE of the
// same source: `ref` is the source NAME, which is what sources.addExisting
// consumes via its `name` param (it resolves by obs_get_source_by_name).
class ClipboardStore {
  source = $state<{ ref: string; name: string } | null>(null);
  filters = $state<CopiedFilter[] | null>(null);
  transform = $state<Transform | null>(null);
}

export const clipboard = new ClipboardStore();
