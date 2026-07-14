// Shared opener for the About dialog. Mirrors settingsOpener:
// App owns the single AboutDialog mount gated on `.open`; the Help menu calls
// openAbout() to request it.

export const aboutOpen = $state<{ open: boolean }>({ open: false });

/** Open the About dialog. */
export function openAbout(): void {
  aboutOpen.open = true;
}

export function closeAbout(): void {
  aboutOpen.open = false;
}
