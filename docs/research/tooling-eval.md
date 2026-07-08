# Tooling evaluation — research grounding + large-codebase navigation

Scope: does each tool improve (a) research grounding with more/better sources, or
(b) context burned navigating a large C++/Svelte codebase? Verified against the
GitHub API on 2026-07-04. Star/commit counts are from the live API, not README claims.

## Verdicts

| Tool | Does what | Install type | Verdict |
| --- | --- | --- | --- |
| [last30days-skill](https://github.com/mvanhorn/last30days-skill) | Multi-platform research (Reddit/HN/X/YT/TikTok/Polymarket/GitHub), ranked by real engagement, synthesized | Claude Code **skill** (also MCP / agent-skill) | **ADOPT** |
| [agent-reach](https://github.com/Panniantong/agent-reach) | Capability layer: unified internet access to 15+ platforms, auto-fallback routing, `doctor` health check | CLI + `SKILL.md` registration | **TRY** |
| [headroom](https://github.com/headroomlabs-ai/headroom) | Compresses tool output / logs / RAG / history before the LLM (claims 60–95% fewer tokens) | Library / proxy / MCP / agent-wrap | **TRY** |
| [pm-skills](https://github.com/phuryn/pm-skills) | 68+ product-management skill workflows (discovery/strategy/GTM) | Claude plugin | **SKIP** |
| [Serena](https://github.com/oraios/serena) | LSP-backed semantic code nav: find-symbol, find-references, precise symbol edit; 40+ langs | MCP server | **TRY** (complement codegraph) |
| [ast-grep-mcp](https://github.com/ast-grep/ast-grep-mcp) | Structural (AST-pattern) code search — "all async fns without error handling" | MCP server (experimental) | **TRY** (refactors only) |
| [reddit-mcp-server](https://github.com/eliasbiondo/reddit-mcp-server) | Zero-config no-auth Reddit search/browse via scraping | MCP server (`uvx`) | **SKIP** (stale/unproven) |
| Sourcegraph / `src` | Enterprise code search + graph | Server/CLI | **SKIP** (overkill solo) |

## Rationale

**last30days-skill** — 48.8k★, created 2026-01-23, last push 2026-07-04 (today), MIT.
Directly upgrades research grounding: it hits the community sources our stack is weakest on
(Reddit, HN, X, YT) and ranks by upvotes/engagement rather than SEO. Reddit + HN + GitHub +
Polymarket work free with no key; X/YT/TikTok need optional keys. Installs as a first-class
Claude Code skill (low friction) and is the cleanest replacement for the dead Reddit MCP +
manual `old.reddit` curl. Strongest single addition for goal (a).

**agent-reach** — 50.2k★, created 2026-02-24, last push 2026-07-03. A "capability layer"
that manages/routes upstream tools (OpenCLI, yt-dlp, Exa) with primary+fallback per platform
and a `doctor` status probe. Its auto-fallback is the direct answer to `old.reddit` fragility:
when one access path 403s it switches without intervention. Overlaps last30days on Reddit/YT;
pick last30days for *synthesis*, add agent-reach only if you want a resilient raw-access
substrate too. TRY, don't stack both blindly.

**headroom** — 56.4k★, very active. Targets goal (b) but as infrastructure, not navigation:
it compresses tool outputs before they reach the model. Real savings, but it wraps/proxies the
agent and overlaps the existing RTK token-killer proxy — two compressors in one pipeline is a
debugging hazard. Pilot in isolation and measure against RTK before committing. TRY.

**pm-skills** — 22.4k★, but pure product-management workflow content. Off-domain for a solo
dev optimizing research grounding + code navigation. SKIP.

**Serena** — 26k★, created 2025-03-23, actively maintained, MIT. The one credible codegraph
*complement*. It runs real language servers (clangd for C++, LSP for TS/Svelte), so its
references/definitions are ground truth — it does **not** hallucinate same-named overloads the
way symbol-name matching can, and it edits at symbol granularity. Trade-off: clangd on the full
OBS/libobs tree is heavy to spin up and index. Recommendation: keep codegraph as the sub-ms
first hop ("where/how does X work"), reach for Serena when you need *exact* cross-file reference
truth or a precise symbol edit on the C++ side. Complement, not replacement.

**ast-grep-mcp** — 426★, experimental. Structural pattern search, not a reference graph — great
for "find every call shaped like this" refactor sweeps (the data-list/registry cleanups this
repo favors), useless for "what calls this / what breaks." Complements codegraph; never replaces
it. TRY for refactors.

**reddit-mcp-server** — 143★, but created **and** last pushed 2026-03-11 (single-day repo, no
maintenance since). It claims no-auth scraping, but that is the exact surface Reddit's lockdown
breaks, and there is no track record it survives. Prefer last30days-skill / agent-reach for
Reddit. SKIP.

### Bottom line
- Goal (a) research grounding + Reddit replacement → **ADOPT last30days-skill**; optionally TRY agent-reach as a fallback substrate.
- Goal (b) navigation → **keep codegraph** as the first hop; **TRY Serena** for LSP-precise C++ reference/edit truth; ast-grep-mcp only for structural refactors.
- headroom = separate token-savings experiment, not a nav tool; don't run alongside RTK unmeasured.

## Proposed changes to global CLAUDE.md (Research preferences)

Concrete edits to the `# Research preferences` section:

1. **Add a source-tier checklist** (run top-down, stop when grounded):
   ```
   Source tiers (prefer higher; cite tier used):
   T1 context7 (library/API docs) · T2 official docs / issue tracker / source
   T3 last30days-skill (engagement-ranked Reddit+HN+X+YT+GitHub synthesis)
   T4 WebSearch (general web) · T5 WebFetch a known URL
   Community sentiment / "does X actually work" → ALWAYS include T3.
   ```

2. **Replace the dead-Reddit-MCP workaround** paragraph. New rule:
   > Reddit / community sentiment: use the **last30days-skill** (Claude Code skill,
   > Reddit tier is free/no-key) as the default. Fall back to **agent-reach** (auto-fallback
   > routing) if a specific thread is needed. The `old.reddit.com` curl scrape is now the
   > last resort, not the primary. The bespoke reddit MCPs (reddit-mcp-server et al.) are
   > unproven against the API lockdown — do not rely on them.

3. **Cite-with-URL-and-date rule** (new bullet under Research preferences):
   > Every external claim carries its source **URL + access date**, and a confidence tag
   > (verified / strong-inference / guessing). Stats (stars/commits/recency) must be
   > API-verified, never taken from a README or a summarizer — WebFetch's small model
   > inflates/paraphrases numbers. Verify with `curl --ssl-no-revoke api.github.com/repos/<r>`.

4. **Strengthen the codegraph trigger** (edit the existing Code-intelligence rule):
   > codegraph_explore is the first hop for any where/how/trace question — but it is a
   > symbol-name graph and lags writes ~1s, so for **exact reference truth or a precise
   > symbol edit in C++**, escalate to **Serena** (clangd LSP, no same-name-overload
   > hallucination). For **structural "all code shaped like X" sweeps**, use **ast-grep**.
   > codegraph stays the cheap default; Serena/ast-grep are the precision escalations.
