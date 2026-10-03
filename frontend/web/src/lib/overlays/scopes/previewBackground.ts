// What the preview frame shows through a widget's transparent areas. "base" is the app's
// own background (every widget's preview today); the other three are the scoped editor's
// stage backdrops, for judging an alert against the kinds of scene it will sit on.

export type PreviewBackground = "base" | "checker" | "dark" | "light";

export const STAGE_BACKGROUNDS: { label: string; value: PreviewBackground }[] = [
  { label: "Checker", value: "checker" },
  { label: "Dark", value: "dark" },
  { label: "Light", value: "light" },
];
