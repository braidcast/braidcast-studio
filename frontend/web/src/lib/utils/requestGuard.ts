// Ordering guard for one piece of state that overlapping requests write to. Each
// request claims a generation before it is sent and asks its claim before it writes
// its response back, so a slow earlier reply -- resolved or rejected -- can never
// overwrite what a later one established.
//
// One instance per piece of state, since two unrelated writes must not cancel each
// other: a fader move says nothing about whether a mute succeeded. Pass an `id` when
// the state is per-row (a source uuid) so rows stay independent within one instance;
// the default id addresses the whole thing.
export class RequestGuard {
  #generations = new Map<string, number>();

  /** Claim the newest generation. Ask the returned predicate wherever the response
   * would write; it answers false once anything newer has claimed the same id. */
  claim(id = ""): () => boolean {
    const mine = (this.#generations.get(id) ?? 0) + 1;
    this.#generations.set(id, mine);
    return () => this.#generations.get(id) === mine;
  }

  /** Note which claims are outstanding now; calling the returned function discards
   * exactly those and leaves anything claimed after this moment alone. A read that
   * outranks the writes it predates takes its mark when it is issued and applies it
   * when its answer lands, so no write is discarded on the strength of an answer that
   * never arrived, and one issued after the read still gets to apply its own. */
  mark(): () => void {
    const outstanding = new Map(this.#generations);
    return () => {
      for (const [id, generation] of outstanding) {
        if (this.#generations.get(id) === generation) {
          this.#generations.set(id, generation + 1);
        }
      }
    };
  }

  /** Discard every claim in flight: authoritative state has arrived and outranks
   * whatever they were going to write. */
  supersede(): void {
    this.mark()();
  }
}
