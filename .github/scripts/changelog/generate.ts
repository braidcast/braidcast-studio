#!/usr/bin/env bun
// Renders the GitHub release body for a `v*` tag and prints it to stdout.
//
//   bun run generate.ts [--from v0.0.1] [--to v0.0.2]
//
// `--to` defaults to the tag at HEAD, `--from` to the previous `v*` tag reachable
// from it. Sections, titles and prefix routing all live in changelog.config.ts.
//
// changelogen does the parsing, contributor resolution and compare-link rendering.
// Two adjustments sit on top of it:
//
//  1. Its commit-type regex is /(?<type>[a-z]+)(...)?: /i -- letters only, and
//     unanchored. Left alone, `obs-ffmpeg: x` parses as type `ffmpeg`, and
//     `win-capture:` / `mac-capture:` both collapse onto `capture`. Rewriting each
//     module prefix to its section key before parsing avoids that entirely, and
//     folds the `CI:` / `ci:` case split back together at the same time.
//  2. It emits `### <title>` under a `## <version>` heading. A GitHub release body
//     sits below the release title, so the version heading goes and everything is
//     promoted one level.
//  3. It drops any commit its regex can't parse, which would silently omit real
//     unprefixed work. Merges are filtered structurally by parent count instead,
//     and everything left over is routed to the fallback section.
//
// Sections render in `for (const type in config.types)` order, but c12 merges our
// config *under* changelogen's built-in types, which puts `feat`/`fix`/... first and
// loses the declared order. Reindexing against changelog.config.ts restores it and
// drops the built-ins we never route to.

import { execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import {
  generateMarkDown,
  getGitDiff,
  loadChangelogConfig,
  parseCommits,
  resolveRepoConfig,
  type ChangelogConfig,
  type RepoConfig,
} from "changelogen";
import declared from "../../../changelog.config";

type Config = ChangelogConfig & {
  moduleMap?: Record<string, string>;
  scopedSections?: string[];
};

const PLUGIN_SECTION = "plugins";
const FALLBACK_SECTION = "misc";

// GitHub rejects a release whose body exceeds 125,000 characters, and takes the whole
// draft down with it. Trimmed notes beat a release that fails to publish, so stay well
// under the cap. Emoji count as several UTF-16 units in `.length`, which errs high --
// the safe direction.
const GITHUB_BODY_LIMIT = 125_000;
const MAX_BODY = 100_000;

// `<prefix>: <subject>` per CONTRIBUTING.md; a trailing `!` marks a breaking change.
const SUBJECT_RE = /^(?<prefix>[^\s:()!]+)(?<breaking>!)?:[ \t]+(?<rest>\S.*)$/;
const COMPARE_RE = /^\[compare changes\]\((?<url>[^)]+)\)$/m;

function git(cwd: string, ...args: string[]): string {
  return execFileSync("git", args, { cwd, encoding: "utf8" }).trim();
}

function tryGit(cwd: string, ...args: string[]): string | undefined {
  try {
    return git(cwd, ...args) || undefined;
  } catch {
    return undefined;
  }
}

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i === -1 ? undefined : process.argv[i + 1];
}

export function sectionFor(prefix: string, config: Config, root: string): string {
  const key = prefix.toLowerCase();
  const mapped = config.moduleMap?.[key];
  if (mapped) {
    return mapped;
  }
  // Module prefixes name a plugin directory 1:1, so read that list off disk
  // rather than duplicating it into the config.
  return existsSync(join(root, "plugins", key)) ? PLUGIN_SECTION : FALLBACK_SECTION;
}

// Merge commits are excluded by parent count, never by prefix-absence: the two are
// unrelated, and conflating them silently drops real unprefixed work.
export function mergeHashes(root: string, from: string, to: string): Set<string> {
  const out = tryGit(root, "log", "--merges", "--pretty=%h", `${from}...${to}`);
  return new Set(out ? out.split("\n") : []);
}

export function retype(message: string, config: Config, root: string): string {
  const groups = message.match(SUBJECT_RE)?.groups;
  if (!groups) {
    // No module prefix. changelogen's regex would drop this commit outright, so
    // file it under the fallback section instead: omitting real work from a
    // release changelog is a silent failure, and CONTRIBUTING.md mandating the
    // prefix is exactly why nobody would notice it had happened.
    return `${FALLBACK_SECTION}: ${message}`;
  }
  const { prefix, breaking, rest } = groups;
  const section = sectionFor(prefix!, config, root);
  // Aggregating sections keep the module visible as a changelogen scope.
  const scope =
    config.scopedSections?.includes(section) && prefix!.toLowerCase() !== section
      ? `(${prefix})`
      : "";
  return `${section}${scope}${breaking ?? ""}: ${rest}`;
}

function polish(markdown: string): { body: string; compare?: string } {
  const compare = markdown.match(COMPARE_RE)?.groups?.url;
  const body = markdown
    .replace(/^## .*$/m, "")
    .replace(COMPARE_RE, "")
    .replace(/^### /gm, "## ")
    .replace(/^#### /gm, "### ")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
  return { body, compare };
}

// Trims at a line boundary so the same range always yields the same body, and keeps
// the compare link reachable in the note -- the trimmed entries are still one click away.
export function fit(body: string, compare?: string): string {
  const footer = compare ? `\n\n**Full Changelog**: ${compare}` : "";
  if (body.length + footer.length <= MAX_BODY) {
    return body + footer;
  }

  const note =
    "\n\n> **Notes truncated** — this release has more changes than a GitHub release body can hold." +
    (compare ? `\n> See the [full changelog](${compare}) for the complete list.` : "");
  const budget = MAX_BODY - note.length - footer.length;

  const kept: string[] = [];
  let used = 0;
  for (const line of body.split("\n")) {
    if (used + line.length + 1 > budget) {
      break;
    }
    kept.push(line);
    used += line.length + 1;
  }
  return kept.join("\n").trimEnd() + note + footer;
}

// First release: nothing to diff against. The fork carries all of upstream's history,
// so rendering the range would emit ~16k entries and blow the body limit -- and
// refusing outright would cut no draft at all. Point at the tree instead.
export function initialRelease(repo: RepoConfig | undefined, to: string): string {
  const body = `## 🎉 Initial Release\n\n- First tagged release of Braidcast.`;
  return repo?.domain && repo.repo
    ? `${body}\n\n**Full Changelog**: https://${repo.domain}/${repo.repo}/commits/${to}`
    : body;
}

export async function resolveConfig(root: string, from: string, to: string): Promise<Config> {
  const config = (await loadChangelogConfig(root, { from, to, output: false })) as Config;
  config.types = Object.fromEntries(
    Object.keys(declared.types).map((key) => [key, config.types[key]!]),
  );
  return config;
}

async function main(): Promise<void> {
  const root = git(process.cwd(), "rev-parse", "--show-toplevel");
  // HEAD can carry several tags (this repo also holds upstream's bare `32.1.0`),
  // so narrow to the `v*` release tags before picking one.
  const to =
    arg("to") ?? tryGit(root, "tag", "--points-at", "HEAD", "--list", "v*")?.split("\n")[0] ?? "HEAD";
  const from =
    arg("from") ?? tryGit(root, "describe", "--tags", "--abbrev=0", "--match", "v*", `${to}^`);

  if (!from) {
    console.log(initialRelease(await resolveRepoConfig(root), to));
    return;
  }

  const config = await resolveConfig(root, from, to);
  const merges = mergeHashes(root, from, to);
  const raw = (await getGitDiff(from, to, root)).filter(
    (commit) => !merges.has(commit.shortHash),
  );
  const commits = parseCommits(
    raw.map((commit) => ({ ...commit, message: retype(commit.message, config, root) })),
    config,
  );

  const { body, compare } = polish(await generateMarkDown(commits, config));
  console.log(fit(body, compare));
}

if (import.meta.main) {
  await main();
}
