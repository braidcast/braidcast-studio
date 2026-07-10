// Shared reactive stream-profile list (reusable destination credentials, stored
// globally in streams.json). Consumers (the Streams tab, the Canvases/Schedule
// pages, the Go Live modal, the OAuth connect dialog) used to fetch
// `streamProfile.list` and subscribe `streamProfile.changed` into private state.
// This singleton is the one source of truth; mirrors canvasStore's lifecycle.

import { obs } from "./bridge";
import { EV } from "./eventNames";
import type { StreamProfileInfo } from "./bridge";

class StreamProfileStore {
  profiles = $state<StreamProfileInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);

  #started = false;
  #ready: Promise<void>;
  #resolveReady: () => void = () => {};

  constructor() {
    this.#ready = new Promise((r) => (this.#resolveReady = r));
  }

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.streamProfileChanged, () => void this.refresh());
    void this.refresh();
  }

  whenReady(): Promise<void> {
    this.start();
    return this.#ready;
  }

  async refresh(): Promise<void> {
    try {
      this.profiles = await obs.call("streamProfile.list");
      this.error = null;
    } catch (e) {
      this.error = (e as Error).message;
    } finally {
      this.loaded = true;
      this.#resolveReady();
    }
  }

  byUuid(uuid: string): StreamProfileInfo | undefined {
    return this.profiles.find((p) => p.uuid === uuid);
  }
}

export const streamProfileStore = new StreamProfileStore();
