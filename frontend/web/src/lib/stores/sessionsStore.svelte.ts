import { obs, type SessionDetail, type SessionInfo } from "$lib/api/bridge";
import { EV } from "$lib/utils/eventNames";

// History is queried state, not a live stream: the list changes when a
// broadcast ends or a row is deleted, which is what sessions.changed reports.
// So this follows canvasStore's start-and-refresh shape rather than statsStore's
// ref-counted push model.
class SessionsStore {
  sessions = $state<SessionInfo[]>([]);
  loaded = $state(false);
  error = $state<string | null>(null);
  #started = false;
  #seq = 0;

  start(): void {
    if (this.#started) {
      return;
    }
    this.#started = true;
    obs.on(EV.sessionsChanged, () => void this.refresh());
    void this.refresh();
  }

  async refresh(): Promise<void> {
    // Drops a stale response that lands after a newer one: sessions.changed can
    // fire twice in quick succession (a session closes, then a delete), and the
    // first reply must not overwrite the second.
    const seq = ++this.#seq;
    try {
      const sessions = await obs.call("sessions.list", { limit: 200, offset: 0 });
      if (seq !== this.#seq) {
        return;
      }
      this.sessions = sessions;
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

  async get(id: string): Promise<SessionDetail> {
    return obs.call("sessions.get", { id });
  }

  async remove(id: string): Promise<void> {
    await obs.call("sessions.delete", { id });
  }
}

export const sessionsStore = new SessionsStore();
