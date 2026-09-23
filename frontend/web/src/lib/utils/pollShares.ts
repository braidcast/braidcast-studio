import type { LivePollOption } from "$lib/api/bridge";

// Share/leader math for a poll's options, shared by PollResults and the results
// dialog's winner hero so both compute "who's leading" the same way -- a second copy
// of this arithmetic is how a hero and its own breakdown would eventually disagree
// about who won.

/** Votes on the options that reported a count. Null when none did. */
export function tallySum(options: LivePollOption[]): number | null {
  let total: number | null = null;
  for (const o of options) {
    if (o.tally !== null) {
      total = (total ?? 0) + o.tally;
    }
  }
  return total;
}

/** Each option's share of the votes (0..1): the platform's live ratio when it sent one,
 * else the option's count over the counted total. Null when neither is known. */
export function sharesOf(options: LivePollOption[]): (number | null)[] {
  const sum = tallySum(options);
  return options.map((o) => {
    if (o.ratio !== null) {
      return o.ratio;
    }
    return o.tally !== null && sum !== null && sum > 0 ? o.tally / sum : null;
  });
}

export const pct = (share: number): number => Math.round(share * 100);

/** Whether a poll has ever had a result reported at all, straight off the raw fields --
 * not off `sharesOf`, which folds a known-zero tally and an unknown one into the same
 * null. A closed poll under a private broadcast merges tallies without ever setting
 * `ratio` (PollRegistry::Close, poll_registry.cpp), so `[0, 0]` with null ratios is a
 * real, known zero-vote result, not "nothing reported yet" -- `sharesOf` would call
 * both cases null, which is right for "who's leading" but wrong for telling a genuine
 * zero apart from no data at all. */
export function hasAnyResult(options: LivePollOption[], totalVotes: number | null): boolean {
  return totalVotes !== null || options.some((o) => o.tally !== null || o.ratio !== null);
}

export interface PollLeaders {
  /** Every option index tied for the highest share. Empty when nobody leads yet --
   * no results have come in, or every reported share is zero. */
  indices: number[];
  /** The winning share (0..1), or null when `indices` is empty. */
  share: number | null;
}

/** The option(s) tied for the highest share, from `sharesOf`'s output. A null share
 * counts as 0 (not yet leading), and the tie is exact equality on the unrounded
 * share, so two options that both display 50% but differ underneath are not tied. */
export function pollLeaders(shares: (number | null)[]): PollLeaders {
  const top = Math.max(0, ...shares.map((s) => s ?? 0));
  if (top <= 0) {
    return { indices: [], share: null };
  }
  const indices: number[] = [];
  shares.forEach((s, j) => {
    if (s === top) {
      indices.push(j);
    }
  });
  return { indices, share: top };
}
