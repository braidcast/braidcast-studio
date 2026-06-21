// Reusable JS<->C++ bridge client. Defines `window.obs` on top of the CEF
// message router (window.cefQuery) and a server-push sink (window.__obsEmit).
// Dependency-free so the Svelte bundle (4.1.5+) imports the same runtime.
//
// Contract:
//   window.obs.call(method, params) -> Promise<result>
//   window.obs.on(event, handler)   -> unsubscribe()
//   window.__obsEmit(event, payload)  (invoked by C++ via ExecuteJavaScript)
(function () {
  if (window.obs) {
    return; // idempotent: tolerate double-injection
  }

  // event name -> Set<handler>
  const subscribers = new Map();

  function call(method, params) {
    return new Promise(function (resolve, reject) {
      if (!window.cefQuery) {
        reject(new Error("bridge unavailable (cefQuery missing)"));
        return;
      }
      let request;
      try {
        request = JSON.stringify({ method: method, params: params === undefined ? null : params });
      } catch (e) {
        reject(new Error("failed to encode params: " + e.message));
        return;
      }
      window.cefQuery({
        request: request,
        onSuccess: function (response) {
          // C++ returns the result already JSON-encoded; empty string => null.
          if (response === "" || response === undefined) {
            resolve(null);
            return;
          }
          try {
            resolve(JSON.parse(response));
          } catch (e) {
            // Tolerate a bare (non-JSON) string result.
            resolve(response);
          }
        },
        onFailure: function (code, message) {
          const err = new Error(message || ("bridge error " + code));
          err.code = code;
          reject(err);
        },
      });
    });
  }

  function on(event, handler) {
    let set = subscribers.get(event);
    if (!set) {
      set = new Set();
      subscribers.set(event, set);
    }
    set.add(handler);
    return function unsubscribe() {
      const s = subscribers.get(event);
      if (s) {
        s.delete(handler);
        if (s.size === 0) {
          subscribers.delete(event);
        }
      }
    };
  }

  // Server-push entry point. C++ Bridge::EmitEvent ExecuteJavaScripts a call
  // to this; it fans out to subscribers registered via obs.on().
  function emit(event, payload) {
    const set = subscribers.get(event);
    if (!set) {
      return;
    }
    // Copy so a handler unsubscribing mid-dispatch doesn't invalidate iteration.
    for (const handler of Array.from(set)) {
      try {
        handler(payload);
      } catch (e) {
        console.log("OBSBRIDGE: handler for '" + event + "' threw: " + e.message);
      }
    }
  }

  window.__obsEmit = emit;
  window.obs = { call: call, on: on };
})();
