// Test preload (bunfig.toml): compiles `*.svelte.ts` modules through the Svelte compiler
// the way vite-plugin-svelte does for the app, so a test can import a runes class
// ($state/$derived/$effect) and drive it for real instead of re-implementing it. TypeScript
// is stripped first because compileModule accepts JavaScript only.
//
// Client output, not server: the server build turns $effect into a no-op, and effects are
// exactly what these tests need to run. A test hosts such a class inside harness.svelte.ts's
// root and settles it with flushSync.
import { plugin } from "bun";
import { mock } from "bun:test";
import { compileModule } from "svelte/compiler";

const transpiler = new Bun.Transpiler({ loader: "ts" });

// Bare "svelte" resolves by export condition, and Bun's are the server's: flushSync there
// is a no-op and untrack skips the real tracker. The compiled modules run against
// svelte/internal/client, so the public API they and the tests import must be the client
// build too. (A plugin onResolve does not reach bare package imports in bun test.)
const svelteClient = Bun.resolveSync("svelte/package.json", import.meta.dir).replace(
  /package\.json$/,
  "src/index-client.js",
);
const clientApi = await import(svelteClient);
mock.module("svelte", () => clientApi);

plugin({
  name: "svelte-runes",
  setup(build) {
    build.onLoad({ filter: /\.svelte\.ts$/ }, async ({ path }) => {
      const js = transpiler.transformSync(await Bun.file(path).text());
      const out = compileModule(js, { filename: path, generate: "client", dev: false });
      return { contents: out.js.code, loader: "js" };
    });
  },
});
