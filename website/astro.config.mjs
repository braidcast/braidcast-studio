// @ts-check
import { defineConfig } from 'astro/config';

// Static output, zero client JS by default. Keep it lightweight for high Lighthouse scores.
export default defineConfig({
  output: 'static',
});
