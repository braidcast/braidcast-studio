// The live polls opened in destination chats (polls.*), kept host-side in the poll
// registry. polls.changed carries the WHOLE list after every change, so an event is a
// complete replacement rather than a delta and no mutation here writes state itself.
//
// A popped-out Chat dock runs its own copy of this singleton in another browser, so it
// must be correct from its own initial polls.list plus the events that follow -- nothing
// is handed across from the main window.

import { obs } from "$lib/api/bridge";
import type { LivePoll } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

export interface PollCreateParams {
  accountId: string;
  /** "" (or omitted) addresses the account's channel-wide chat. */
  profileUuid?: string;
  question: string;
  options: string[];
}

class PollStore {
  polls = $state<LivePoll[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  // Bumped by every list issued AND every event applied. A polls.list that resolves after
  // a polls.changed landed describes an older registry than the event did, so it must not
  // overwrite it; last-issued-or-applied wins, never last-resolved.
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.pollsChanged, ({ polls }) => {
      this.#seq++;
      this.polls = polls;
      this.error = null;
      this.loaded = true;
    });
    void this.refresh();
  }

  async refresh(): Promise<void> {
    const seq = ++this.#seq;
    try {
      const { polls } = await obs.call("polls.list");
      if (seq !== this.#seq) {
        return;
      }
      this.polls = polls;
      this.error = null;
    } catch (e) {
      if (seq !== this.#seq) {
        return;
      }
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
    }
  }

  /** Opens a poll in one destination's chat. Rejects with the host's readable reason. */
  async create(params: PollCreateParams): Promise<LivePoll> {
    return (await obs.call("polls.create", params)).poll;
  }

  /** Closes a running poll. A failure is also recorded on the poll itself (its `error`). */
  async end(id: string): Promise<LivePoll> {
    return (await obs.call("polls.end", { id })).poll;
  }

  async dismiss(id: string): Promise<void> {
    await obs.call("polls.dismiss", { id });
  }
}

export const pollStore = new PollStore();
