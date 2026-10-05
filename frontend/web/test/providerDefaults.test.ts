import { describe, expect, test } from "bun:test";
import type { OAuthProviderField } from "$lib/api/bridge";
import { ALL_LAYER, inheritLayers, providerDefaultsBlocks } from "$lib/dialogs/golive/fieldValue";

const field = (key: string, over: Partial<OAuthProviderField> = {}): OAuthProviderField =>
  ({ key, label: key, type: "text", ...over }) as OAuthProviderField;
const youtube = {
  id: "youtube",
  fields: [
    field("title", { scope: "all" }),
    field("category", { scope: "provider", type: "category" }),
    field("privacy", { scope: "provider", type: "enum" }),
    field("latency", { scope: "provider", tier: "advanced" }),
    field("notes"),
  ],
};
const twitch = { id: "twitch", fields: [field("title", { scope: "all" }), field("tags", { scope: "provider" })] };
const chan = (accountId: string, provider: typeof youtube | null) => ({ accountId, provider });

describe("Go Live's per-platform defaults blocks", () => {
  test("two YouTube channels get a YouTube block of their provider-scoped simple fields", () => {
    const blocks = providerDefaultsBlocks([
      chan("youtube:a", youtube),
      chan("twitch:t", twitch),
      chan("youtube:b", youtube),
    ]);
    expect(blocks).toHaveLength(1);
    const [b] = blocks;
    expect(b.provider.id).toBe("youtube");
    expect(b.channelCount).toBe(2);
    expect(b.accountId).toBe("youtube:a");
    expect(b.fields.map((f) => f.key)).toEqual(["category", "privacy"]);
    // The block edits exactly the bucket each channel card inherits from.
    expect(b.bucket).toBe(inheritLayers(youtube.fields[1], "youtube")[0]);
    expect(b.bucket).not.toBe(ALL_LAYER);
  });

  test("one channel of a platform gets no block, and an unresolved provider is skipped", () => {
    expect(providerDefaultsBlocks([chan("youtube:a", youtube), chan("twitch:t", twitch)])).toEqual([]);
    expect(providerDefaultsBlocks([chan("x:1", null), chan("x:2", null)])).toEqual([]);
  });

  test("a platform with nothing provider-scoped gets no empty block", () => {
    const plain = { id: "kick", fields: [field("title", { scope: "all" })] };
    expect(providerDefaultsBlocks([chan("kick:a", plain as never), chan("kick:b", plain as never)])).toEqual([]);
  });
});
