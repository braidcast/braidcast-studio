import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import { fileURLToPath } from "node:url";

// base './' makes the built index.html reference assets relatively (./assets/*),
// which the C++ app:// scheme handler resolves as app://app/assets/* off the
// bundle root. The bundle is fully offline: no CDN, no dev server.
export default defineConfig({
  base: "./",
  plugins: [svelte()],
  resolve: {
    alias: {
      $lib: fileURLToPath(new URL("./src/lib", import.meta.url)),
    },
  },
  // Dev server for live UI work (HMR): run `bun run dev`, then launch the host with
  // FE_DEV_URL=http://localhost:5173 to point CEF at this instead of the app:// bundle.
  // Fixed port so FE_DEV_URL stays stable across restarts.
  server: {
    port: 5173,
    strictPort: true,
  },
  build: {
    outDir: "dist",
    emptyOutDir: true,
    // Keep assets as files the scheme handler serves (no forced data: URLs).
    target: "es2022",
    // The bundle loads from local disk via the app:// scheme, so chunk size has no
    // network cost and code-splitting buys nothing -- the shell needs it all at
    // startup. The Overlays editor pulls in CodeMirror 6 (~+630 kB raw), putting the
    // main chunk near ~1.17 MB; raise the warning above that so it still flags a real
    // regression (e.g. an accidental heavy dep) instead of crying wolf every build.
    chunkSizeWarningLimit: 1400,
  },
});
