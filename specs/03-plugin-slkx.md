# anton-slkx — the Slack plugin

A C++ executable built from this repo (`plugins/slkx/`) that turns Slack
activity into anton plugin lines (`02-plugins.md`). Two rules, each enabled
by its own flag; with neither flag it exits with a usage error.

| flag | rule | emits |
| --- | --- | --- |
| `--review-channel C` | every PR posted to C is on the `code-review` list; `:eyes:`/`:zoiando:` on it means "being reviewed" | `lists`, `tags` |
| `--deploy-channel C` | a PR posted to C is in the deploy queue; p2r2's reactions refine it | `status`: `⚙ queued` / `💬 Ready?` |

Approved / changes requested / commented come from GitHub in anton itself:
`:+1:`, `:speech_balloon:`, `:wrench:` are used inconsistently and are ignored.

## Where the websocket goes

anton never touches Slack. `slkx watch` already owns the websocket (and its
reconnects) and prints raw RTM events as JSONL. This plugin is the layer in
between: it reads slkx's raw events and prints anton's `pr` lines.

```
Slack RTM ──ws──▶ slkx watch ──raw JSONL──▶ anton-slkx ──pr JSONL──▶ anton
                  slkx read  ──backfill──▶      (fold: ts → PRs, reactions)
```

slkx is a runtime dependency of the plugin only: it is found on `PATH`
(`--slkx PATH` to override) and never linked. anton builds and runs without
either.

## Flags

| flag | default | |
| --- | --- | --- |
| `--review-channel C` | off | channel whose posts form the `code-review` list |
| `--review-emoji a,b` | `eyes,zoiando` | reactions that mean "reviewing" |
| `--deploy-channel C` | off | channel whose posts mean "queued" |
| `--deploy-clear-emoji a,b` | `p2r2-rejected-pr` | reactions that take a PR back out of the queue |
| `--deploy-state emoji=glyph:text` | see "Deploy channel" | repeatable, **in pipeline order**; replaces the defaults |
| `--backfill N` | `1000` | messages read per channel at startup. Slack's max is 1000: a bigger value is capped, a non-number uses 1000, both with a warning on stderr |
| `--slkx PATH` | `slkx` | the slkx binary |

`C` is anything slkx accepts: `'#code-review'`, a name, or an id. The plugin
resolves names to ids once at startup via `slkx channels --json`, because RTM
events carry ids only.

## Rules in detail

**Live first.** The list is driven by the websocket: `slkx watch` forwards
every RTM event the Slack web client gets — new messages, edits, deletes,
reactions. A PR posted while the plugin runs appears in anton within a second;
a deleted message removes it the same way. History (`slkx read`) is only used
once at startup, to know what was posted *before* the plugin started, because
a socket only carries what happens after it connects.

**Messages.** Any author's message counts, not just yours: in a review tool
the interesting PRs are other people's. PR URLs are found with
`https://github\.com/[\w.-]+/[\w.-]+/pull/\d+` over `text` (Slack wraps them as
`<url|label>`; the regex ignores the wrapping). Bot posts (`bot_message`, e.g. a GitHub app linking PRs) count too; joins,
topic changes and other subtypes don't. A message may name several PRs;
everything below applies to each. Thread replies count only if they are
broadcast to the channel (`subtype: thread_broadcast`), since slkx's channel
read does not return other replies.

**Review channel.**
- PR posted → on list `code-review`, with `order` = the ts of its newest post
  there, so the view matches the channel: newest on top, a repost moves it up.
- Reviewers = users with a review emoji on **any** message naming that PR
  (union — the same PR is often reposted). Tag per reviewer, sorted:
  `{"glyph": "👀", "text": "<handle>", "hint": "reviewing, via Slack"}`.
  Your own reaction renders as `you`.
- A deleted message, or an edit that removes the URL, takes that message out.
  The PR leaves the list when **no** message naming it remains — if it was
  posted twice and one post is deleted, it stays.

**Deploy channel.** p2r2 *adds* a reaction at each step and leaves the
earlier ones, so a message's state is the **furthest** step present — the same
rule slkx-alerts uses. The steps, in order, and what each shows:

| p2r2 reaction (in order) | STATE in anton |
| --- | --- |
| *(posted, no reaction yet)* | `⚙ queued` |
| `p2r2-picked-up-your-pr` | `⚙ queued` |
| `p2r2-validated-your-pr` | `⚙ queued` |
| `p2r2-waiting-for-dev` | `💬 Ready?` — p2r2 is asking you; act |
| `p2r2-pr-queued-for-merge` | `⚙ queued` |
| `p2r2-pr-merging` and later | `⚙ queued` (GitHub's `merged` takes over once merged) |

So `Ready?` appears exactly while waiting-for-dev is the furthest step, and
goes back to `queued` as soon as p2r2 moves on. The defaults are the
`--deploy-state` list:

```
--deploy-state p2r2-picked-up-your-pr=⚙:queued
--deploy-state p2r2-validated-your-pr=⚙:queued
--deploy-state p2r2-waiting-for-dev=💬:Ready?
--deploy-state p2r2-pr-queued-for-merge=⚙:queued
--deploy-state p2r2-pr-merging=⚙:queued
```

- A clear emoji (`p2r2-rejected-pr`) on that message → no status from it.
- Step reactions count from anyone, not only p2r2 (ceiling: a human adding
  `p2r2-waiting-for-dev` by hand would show `Ready?`; filter by the bot's user
  id if that ever happens).
- Several deploy messages for one PR → the most recent message decides.
- Once merged, GitHub's `merged` outranks the custom status in anton, so the
  plugin never needs to track merges.
- People without this queue system just omit `--deploy-channel`; nothing
  about deploys exists in anton itself.

## Algorithm

State (in `fold.hpp`, pure — events in, changed PR URLs out):
- `msgs: (channel, ts) → [pr urls]`
- `reacts: (channel, ts) → set of (emoji, user)`
- `names: user id → handle`, from `user_names` in slkx's cache
  (`slkx channels --json` prints the whole cache: `channels`, `ids`,
  `user_names`); `me` from `slkx auth status --json` → `user_id`

0. **Watch first.** `slkx watch` starts before the backfill and its events
   wait until the backfill is folded, so nothing that happens in between is
   lost. Replaying an event the history already shows is a no-op (the fold is
   set-based). `slkx watch` stays in the plugin's process group, so anton's
   stop signal reaches it.
1. **Backfill.** For each enabled channel, `slkx read --channel C --limit N --json`.
   Fold every message and its `reactions` into state. Emit one `pr` line per
   PR seen, then `{"health": "ok"}`.
2. **Live.** Spawn `slkx watch`, fold each line:

   | event | effect |
   | --- | --- |
   | `message`, no subtype, `thread_broadcast` or `bot_message`, channel enabled, has PR URLs | add `msgs[(ch, ts)]` |
   | `message`/`message_changed` | replace `msgs[(ch, message.ts)]` with the new URLs (none if it is a plain thread reply) |
   | `message`/`message_deleted` | drop `msgs` and `reacts` for `(ch, deleted_ts)` |
   | `reaction_added` / `reaction_removed` on a known `(item.channel, item.ts)` | add/remove `(reaction, user)` |
   | anything else | ignore |

   After each event, emit a `pr` line for every PR whose derived output changed.
3. **Derive** a PR's line from all messages naming it: `lists` if any review
   message; `tags` from review-emoji reactions on review messages; `status`
   from the most recent deploy message without a clear emoji, using the
   furthest `--deploy-state` step present. Full state each time, per the
   protocol.
4. **Odd input** (an event of unexpected shape, bad UTF-8) is skipped, never
   fatal. If startup fails (`slkx read` errors, unknown channel), the plugin
   stops its `slkx watch`, reports `health: error`, and exits.
5. **slkx watch exits** → `{"health": "error", "text": "<last stderr line>"}`
   and exit non-zero. anton drops this plugin's state and restarts it, which
   backfills again. One restart policy, in anton; none in the plugin.

Known ceiling, startup only: a PR posted *before the plugin started* and
older than the last `--backfill` (1000) messages is not on the list, and live
reactions on that old message are ignored. Everything posted while it runs is
caught live, without limit. 1000 messages of `#code-review` is weeks; closed
PRs leave the view through `is:open` anyway. If that ever bites, the fix is
paging history in slkx (`read --cursor`), not anything in anton.

## Build

Part of anton's CMake project (`-DANTON_SLKX_PLUGIN=OFF` to skip). Reuses
`src/proc.hpp` for spawning slkx and reading its lines, and nlohmann/json.

```
plugins/slkx/main.cpp   flags, channel resolve, backfill, watch loop, emit
plugins/slkx/fold.hpp   the pure fold + derive
tests/fold_check.cpp    feeds recorded slkx JSON through the fold, asserts
                        the emitted lines (post, react, unreact, edit, delete,
                        repost-union, deploy steps incl. Ready? → queued,
                        deploy + reject)
```
