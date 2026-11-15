#!/usr/bin/env python3
"""Made-up data for anton's screenshot. Three roles:

  demo.py gh ...    stands in for `gh` (auth status, api graphql, pr view
                    --web and --json body, a PR's files and their contents)
  demo.py plugin    stands in for anton-slkx: the code-review list, 👀 tags,
                    deploy statuses
  demo.py temoc     stands in for anton-temoc: one PR's spec results, as a
                    tag, a note and marks on the spec's rows

Run it through ./run.sh. Nothing here talks to GitHub or Slack.
"""
import json, re, sys, time

ME = "mona-lisa"
NOW = time.time()
H = 3600

# number, repo, author, +, -, created (h ago), state, reviewers (👀), posted to
# #code-review (h ago, None = not posted), searches it shows up in
PRS = [
    (4812, "core", "rtanaka",    385,   0,   3, "review",    ["paz"],          0.2, ""),
    (4827, "core", "lmartin",    255,  32, 0.5, "approved",  [],               0.5, ""),
    (4826, "core", "lmartin",    260,  20, 0.5, "approved",  [],               0.5, ""),
    (4825, "core", "lmartin",    331,  61, 0.5, "ready",     [],               0.5, ""),
    (4809, "core", "rtanaka",      0,   4,   2, "review",    ["mona", "yuri"], 0.9, "rr"),
    (4790, "web",  "okafor-c",   490,  56,  26, "merged:1",  [],               1.1, ""),
    (4805, "core", "rtanaka",    578,   2,   3, "conflict",  ["chidi"],        1.4, "mn"),
    (4822, "core", "okafor-c",   236,   3,   1, "merged:1",  [],               1.5, ""),
    (4821, "core", "okafor-c",   411,  54,   1, "checks",    [],               1.6, ""),
    (4820, "core", "okafor-c",   107,  10,   1, "merged:1",  [],               1.7, ""),
    (4819, "web",  "lmartin",     12,   1,   1, "merged:1",  [],               1.8, ""),
    (4801, "core", "shah-a",     433, 106,   2, "queued",    ["dana"],         2.0, ""),
    (4811, "core", "dkowalski",   50,  17,   2, "merged:2",  [],               2.1, ""),
    (4808, "core", "rtanaka",      0,   4,   2, "you_ok",    [],               2.2, "rr"),
    (4815, "core", "dkowalski",  227,  12,   2, "merged:2",  [],               2.3, ""),
    (4814, "web",  "dkowalski",  483,  92,   2, "changes",   ["ines"],         2.4, ""),
    (4816, "core", "jvolkov",     72,   7,   2, "merged:2",  [],               2.5, ""),
    (4813, "core", "ifernandes", 502,  49,   2, "merged:2",  [],               2.6, ""),
    (4803, "web",  "dkowalski",  216,  57,   3, "merged:3",  [],               2.8, ""),
    (4802, "core", "ifernandes",1349,  21,   3, "review",    ["lucas"],        2.9, ""),
    (4806, "core", "rtanaka",      3,  44,   2, "merged:2",  [],               3.0, ""),
    (4810, "core", "rtanaka",     97,  30,   2, "merged:2",  [],               3.1, ""),
    (4817, "core", ME,             3,   3,   2, "merged:2",  [],               3.2, ""),
    (4800, "core", "shah-a",     294, 117,   3, "draft",     [],               3.3, ""),
    (4799, "core", "dkowalski",  651,  50,   3, "merged:3",  [],               3.4, ""),
    # Yours, not posted to #code-review yet.
    (4830, "core", ME,           142,  18,  20, "approved",  [],               None, ""),
    (4831, "web",  ME,            38,   2,  50, "review",    [],               None, "as"),
    (4795, "core", ME,          2210, 840, 170, "draft",     [],               None, ""),
    (4796, "infra", ME,           64,  12,  75, "queued",    [],               None, "mn"),
    (4828, "web",  ME,             9,   9,   4, "checks",    [],               None, ""),
]

TITLES = {
    4812: "[CORE-2210] Onboarding: skip the team step for single-seat plans",
    4827: "feat(billing): prorate seat changes mid-cycle",
    4826: "feat(billing): a downgrade waits for the end of the period",
    4825: "feat(billing): invoices list each seat change on its own line",
    4809: "Table values: no success toast when deleting a value",
    4790: "fix(editor): paste from Google Docs keeps nested lists",
    4805: "Search: the query box keeps the cursor where you left it",
    4822: "test(api): contract tests for the public v2 endpoints",
    4821: "feat(api): idempotency keys on POST /payments",
    4820: "feat(api): a webhook's payload is the same object the API returns",
    4819: "docs(web): the settings guide lists every permission",
    4801: "perf(export): CSV export streams rows instead of buffering them",
    4811: "fix(webhooks): retry 429s after the server's Retry-After",
    4808: "Automations list: drop toasts whose result is already on screen",
    4815: "feat(audit): filter the audit log by actor and action",
    4814: "Put the change request page in the approval flow",
    4816: "Ignore unsupported attachments (.bin/.zip) on upload",
    4813: "server: an assistant's tool changes stay in the draft",
    4803: "Speed up ui and form tests, fix flaky date pickers",
    4802: "[CORE-2187] feat(sso): SCIM group sync",
    4806: "Reports: the date range picker remembers your last choice",
    4810: "fix(auth): refresh tokens survive five minutes of clock skew",
    4817: "docs(api): document pagination cursors",
    4800: "refactor(jobs): one queue per tenant",
    4799: "feat(notifications): a weekly digest email per workspace",
    4830: "feat(search): typo tolerance for people's names",
    4831: "fix(mobile): the keyboard no longer covers the send button",
    4795: "[CORE-1990] Dashboard v2: widgets, layouts and sharing",
    4796: "chore(ci): cache the dependency layer between builds",
    4828: "fix(ui): dark mode contrast on disabled buttons",
}


# GitHub labels, for the filter bar's label chips.
LABELS = {
    4812: ["onboarding"], 4802: ["sso", "needs-design"], 4828: ["bug", "ui"], 4830: ["search"],
    4831: ["bug", "mobile"], 4805: ["bug"], 4801: ["performance"], 4821: ["payments", "api"],
    4814: ["needs-design"], 4795: ["needs-design", "dashboard"], 4796: ["ci"], 4809: ["bug"],
}


# What an LLM reviewer plugin would attach (`n` opens it).
NOTES = {
    4812: """## Review: skip the team step for single-seat plans

**Verdict:** fine to merge after one fix. The change is small and well tested, and the onboarding flow reads better without the team step for people who will never invite anyone; the only real problem is how trial plans report their seat count, which slips past the new check.

### Problems
1. `onboarding/steps.ts:48` checks `plan.seats === 1`, but trial plans report
   `seats: null` until checkout, so trial users with one seat still see the
   team step. Use `(plan.seats ?? 1) <= 1`.
2. The new `skipTeamStep` flag is never reset when someone upgrades mid-flow;
   they land on billing with no team.

### Nits
- `StepList` re-renders on every keystroke now that `steps` is built inline:

```ts
const steps = useMemo(() => buildSteps(plan), [plan]);
```

- The test names say "skips" but assert the step count; name the behavior.
""",
}


# PR descriptions (`⏎` reads one). Shaped like real ones: a template's
# comments and <details>, a task list, a table, code.
BODIES = {
    4812: """<!-- Describe the change and link the ticket. Delete what doesn't apply. -->

## What

Single-seat plans skip the **Invite your team** step of onboarding. Nobody on a
one-seat plan can invite anyone, so the step only ever showed an upgrade wall.

Closes [CORE-2210](https://linear.app/acme/issue/CORE-2210).

## How it behaves

| Plan        | Seats | Team step | Notes                                   |
|-------------|------:|:---------:|-----------------------------------------|
| Solo        |     1 |  skipped  | the step's checklist item is hidden too |
| Solo (trial)|  null |  skipped  | trials report no seats until checkout   |
| Team        |     5 |   shown   |                                         |
| Enterprise  |   250 |   shown   | invites go through SCIM when it's on    |

## Checklist

- [x] Unit tests for `buildSteps()`
- [x] Tried it on a fresh Solo workspace
- [ ] Copy reviewed by design
- [ ] Analytics event renamed to `onboarding_team_step_skipped`

```ts
export function buildSteps(plan: Plan): Step[] {
  return ALL_STEPS.filter((s) => s.id !== "team" || (plan.seats ?? 1) > 1);
}
```

<details>
<summary>Screenshots</summary>

![onboarding without the team step](https://example.com/onboarding.png)

Before: 5 steps. After: 4 steps, and the progress bar says so.

</details>

> **Note**
> The flag `skipTeamStep` stays in the payload for one release so old
> clients don't break.
""",
    4802: """<!-- PR template: what, why, how to test -->

SCIM group sync: groups pushed by the identity provider become teams,
and membership follows them. Part of the SSO epic ([CORE-2187](https://linear.app/acme/issue/CORE-2187)).

### Endpoints

| Method | Path                         | Status | What it does                                                    | Rate limit |
|--------|------------------------------|:------:|-----------------------------------------------------------------|-----------:|
| GET    | `/scim/v2/Groups`            | 200    | lists groups, filtered by `displayName eq "..."`                 | 100/min    |
| POST   | `/scim/v2/Groups`            | 201    | creates the team, members included                              | 20/min     |
| PATCH  | `/scim/v2/Groups/{id}`       | 200    | add/remove members; `replace` on `displayName` renames the team | 100/min    |
| DELETE | `/scim/v2/Groups/{id}`       | 204    | archives the team (never deletes its projects)                  | 20/min     |

### How to test

1. Start the IdP mock: `make idp-mock`
2. Point a workspace at it:
   ```sh
   acme-admin sso set --workspace demo --scim-url http://localhost:9099/scim/v2
   ```
3. Push a group from the mock and watch the team appear.
   - Renames show up within a second.
   - Removing the last member archives nothing: empty teams stay.

<details>
<summary>Why not sync on login instead?</summary>

Login-time sync only learns about people who log in. Offboarding has to
remove access for people who *don't*, so it has to be pushed.

</details>

- [x] Contract tests against the SCIM 2.0 spec (RFC 7644)
- [ ] Load test with 10k-member groups
""",
}


# The markdown files a PR changes (`⏎`, then Tab), at its head commit. 4812
# changes a temoc spec and a doc that share a name.
HEAD = "3f9c2a1e5b7d4c6a8e0f1b2c3d4e5f60718293a4"
FILES = {
    4812: {
        "specs/onboarding.md": """# Onboarding steps

Which onboarding steps a new workspace sees, by plan.

[//]: # "import Onboarding.Fixtures"
[//]: # "decisionTable StepsFixture"

| plan       | seats | # why                    | team step? | steps? |
| ---------- | ----- | ------------------------ | ---------- | ------ |
| Solo       | 1     | nobody to invite         | skipped    | 4      |
| Solo trial | null  | trials have no seats yet | skipped    | 4      |
| Team       | 5     |                          | shown      | 5      |
| Enterprise | 250   | invites go through SCIM  | shown      | 5      |

An upgrade mid-flow brings the team step back. Not built yet:

[//]: # "decisionTable UpgradeFixture -- snooze until 2026-12-15"

| from | to   | at step | team step? |
| ---- | ---- | ------- | ---------- |
| Solo | Team | billing | shown      |
| Solo | Team | profile | shown      |

The checklist hides what the steps skip:

[//]: # "decisionTable ChecklistFixture#items"

| plan | items?                                |
| ---- | ------------------------------------- |
| Solo | profile, billing, first project       |
| Team | profile, billing, team, first project |

## Invites

[//]: # "decisionTable InvitesFixture"

| plan | seats | invite? |
| ---- | ----- | ------- |
| Team | 5     | sent    |
| Solo | 1     | hidden  |
""",
        "specs/billing.md": """# Billing step

[//]: # "decisionTable PriceFixture"

| plan | seats | price? |
| ---- | ----- | ------ |
| Solo | 1     | 9      |
| Team | 5     | 45     |
""",
        "docs/onboarding.md": """# Onboarding

A new workspace walks through four or five steps: **profile**, **billing**,
**team** (plans with more than one seat), **first project**, and a tour.

Each step can be skipped and comes back in the checklist.
""",
    },
}


def spec_line(num, path, start):
    """The 1-based line of FILES[num][path] that starts with `start`."""
    for i, l in enumerate(FILES[num][path].split("\n"), 1):
        if l.startswith(start):
            return i
    raise KeyError(start)


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def node(pr):
    num, repo, author, add, dele, age, state, _, _, _ = pr
    merged = state.startswith("merged")
    n = {
        "number": num, "title": TITLES[num],
        "url": f"https://github.com/acme/{repo}/pull/{num}",
        "isDraft": state == "draft",
        "state": "MERGED" if merged else "OPEN",
        "createdAt": iso(NOW - age * H), "updatedAt": iso(NOW - age * H / 2),
        "closedAt": iso(NOW - float(state.split(":")[1]) * H) if merged else None,
        "additions": add, "deletions": dele,
        "reviewDecision": {"approved": "APPROVED", "queued": "APPROVED", "ready": "APPROVED",
                           "changes": "CHANGES_REQUESTED"}.get(state, "REVIEW_REQUIRED"),
        "mergeable": "CONFLICTING" if state == "conflict" else "MERGEABLE",
        "changedFiles": max(1, (add + dele) // 60),
        "labels": {"nodes": [{"name": l} for l in LABELS.get(num, [])]},
        "repository": {"nameWithOwner": f"acme/{repo}", "owner": {"login": "acme"}},
        "author": {"login": author},
        "latestReviews": {"nodes": [{"author": {"login": ME}, "state": "APPROVED",
                                     "submittedAt": iso(NOW - H)}] if state == "you_ok" else []},
        "commits": {"nodes": [{"commit": {"statusCheckRollup": {
            "state": "FAILURE" if state == "checks" else "SUCCESS"}}}]},
    }
    return n


def gh(args):
    if args[:2] == ["pr", "view"] and "--json" in args:  # the reader: a PR's body
        num = int(args[2].rstrip("/").split("/")[-1])
        print(json.dumps({"body": BODIES.get(num, ""), "headRefOid": HEAD}))
        return 0
    if args[:2] == ["api", "--paginate"]:  # repos/acme/<repo>/pulls/<n>/files, filenames
        for path in FILES.get(int(args[2].split("/")[4]), {}):
            print(path)
        return 0
    if args[:2] == ["api", "-H"]:  # repos/acme/<repo>/contents/<path>?ref=<sha>, raw
        m = re.match(r"repos/acme/[^/]+/contents/(.*)\?ref=", args[3])
        for files in FILES.values():
            if m and m.group(1) in files:
                sys.stdout.write(files[m.group(1)])
                return 0
        print("gh: Not Found (HTTP 404)", file=sys.stderr)
        return 1
    if args[:2] == ["auth", "status"] or args[:2] == ["pr", "view"]:
        return 0
    if args[:2] != ["api", "graphql"]:
        print(f"demo gh: {' '.join(args)} not faked", file=sys.stderr)
        return 1
    fields = dict(a.split("=", 1) for a in args[2:] if "=" in a)
    query = fields.get("query", "")
    data: dict = {"viewer": {"login": ME}}
    if "q" in fields:  # a search view
        q = fields["q"]
        def hit(pr):
            if pr[6].startswith("merged") and "is:open" in q:
                return False
            if "is:merged" in q.split() and not pr[6].startswith("merged"):
                return False
            for tok in q.split():  # label:x, -label:x, author:login, repo:acme/x
                neg, tok = tok.startswith("-"), tok.lstrip("-")
                key, _, val = tok.partition(":")
                val = val.strip('"')
                if key == "label" and (val in LABELS.get(pr[0], [])) == neg:
                    return False
                if key == "author" and val != "@me" and (pr[2] == val) == neg:
                    return False
                if key == "repo" and (f"acme/{pr[1]}" == val) == neg:
                    return False
            if "author:@me" in q: return pr[2] == ME
            if "review-requested:@me" in q: return "rr" in pr[9]
            if "assignee:@me" in q: return "as" in pr[9]
            if "mentions:@me" in q: return "mn" in pr[9]
            return True
        nodes = [node(p) for p in PRS if hit(p)]
        data["search"] = {"pageInfo": {"hasNextPage": False, "endCursor": None}, "nodes": nodes}
    else:  # PRs by URL: p<i>: repository(owner: "acme", name: "core") { pullRequest(number: N)
        by_num = {p[0]: p for p in PRS}
        for alias, num in re.findall(r'(p\d+): repository\([^)]*\) \{ pullRequest\(number: (\d+)\)', query):
            p = by_num.get(int(num))
            data[alias] = {"pullRequest": node(p) if p else None}
    print(json.dumps({"data": data}))
    return 0


def plugin():
    def out(j):
        print(json.dumps(j, ensure_ascii=False), flush=True)
    for p in PRS:
        num, repo, _, _, _, _, state, reviewers, posted, _ = p
        line: dict = {"pr": f"https://github.com/acme/{repo}/pull/{num}"}
        if posted is not None:
            line["lists"] = ["code-review"]
            line["order"] = NOW - posted * H
        if reviewers:
            line["tags"] = [{"glyph": "👀", "text": "you" if r == "mona" else r} for r in reviewers]
        if state == "queued":
            line["status"] = {"glyph": "⚙", "text": "queued"}
        if state == "ready":
            line["status"] = {"glyph": "💬", "text": "Ready?"}
        if num in NOTES:
            line["note"] = NOTES[num]
            line.setdefault("tags", []).append({"glyph": "🤖", "text": "haiku"})
        if len(line) > 1:
            out(line)
    out({"health": "ok"})
    out({"actions": [{"id": "review", "key": "v", "label": "toggle 👀 reviewing in Slack", "button": "Toggle 👀"}]})
    for _ in sys.stdin:  # stay up like the real plugin; ignore `v`
        pass
    time.sleep(10 ** 9)


def temoc():
    """What anton-temoc says about 4812: in onboarding.md a failed row, a
    snoozed table, an error, a fixture that doesn't exist and the rest
    passing; billing.md never ran (its hooks failed: a banner). Stays up and
    offers `T`, like the real one: a run (the PR, a file, a table or a row)
    clears what it's about to run and fills it in a row at a time."""
    url = "https://github.com/acme/core/pull/4812"
    spec, bill = "specs/onboarding.md", "specs/billing.md"
    at = lambda f, start: spec_line(4812, f, start)
    words = {"passed": "✓", "failed": "✗", "error": "!", "snoozed": "~", "skipped": "-", "partial": "·", "running": "⋯"}
    worse = ["passed", "skipped", "snoozed", "partial", "failed", "error", "running"]
    level = lambda s: {"passed": "ok", "failed": "error", "error": "error", "snoozed": "warn"}.get(s, "dim")

    def table(f, directive, rows, own=None):
        return {"line": at(f, directive), "rows": [at(f, r) for r in rows], "own": own}
    tables = {
        spec: [table(spec, '[//]: # "decisionTable StepsFixture',
                     ["| Solo       |", "| Solo trial |", "| Team       |", "| Enterprise |"]),
               table(spec, '[//]: # "decisionTable UpgradeFixture', ["| Solo | Team | billing", "| Solo | Team | profile"]),
               table(spec, '[//]: # "decisionTable ChecklistFixture', ["| Solo | profile", "| Team | profile"]),
               table(spec, '[//]: # "decisionTable InvitesFixture', ["| Team | 5 ", "| Solo | 1 "],
                     own="make got exception NO_CLASS InvitesFixture")],
        bill: [table(bill, '[//]: # "decisionTable PriceFixture', ["| Solo | 1 ", "| Team | 5 "])],
    }
    banner = {bill: "make got exception COULD_NOT_INVOKE_CONSTRUCTOR BillingHooks[0] (NO_CLASS BillingHooks)"}
    final = {at(spec, "| Solo trial |"): ("failed", ["teamStep expected skipped, got shown"]),
             at(spec, "| Solo | Team | billing"): ("snoozed", ["teamStep expected shown, got skipped (snoozed until 2026-12-15)"]),
             at(spec, "| Team | profile"): ("error", ["items expected profile, billing, team, first project, "
                                                       "got exception NullPointerException: plan.seats is null"])}
    for t in tables[spec][3]["rows"] + tables[bill][0]["rows"]:
        final[t] = ("skipped", [])
    # what's known: per file, rows by line, tables' own failures shown, banner, running table
    state = {f: {"rows": {}, "own": set(), "banner": None, "running": None} for f in tables}
    command = ["temoc -c Config.toml --output jsonl --"]

    def table_status(f, t):
        st = state[f]
        if st["running"] == t["line"]:
            return "running"
        got = [st["rows"].get(r, ("partial",))[0] for r in t["rows"]] + (["error"] if t["line"] in st["own"] else [])
        if all(r not in st["rows"] for r in t["rows"]) and t["line"] not in st["own"]:
            return None
        return max(got, key=worse.index)

    def counted(s, rows, f):
        bad = lambda r: state[f]["rows"].get(r, ("",))[0] in ("failed", "error")
        done = lambda r: state[f]["rows"].get(r, ("running",))[0] != "running"
        k = sum(1 for r in rows if (bad(r) if s in ("failed", "error") else done(r) if s in ("running", "partial")
                                    else state[f]["rows"].get(r, ("",))[0] == s))
        return s if k == 0 and s in ("failed", "error") else f"{s} · {k} of {len(rows)}"

    def line_out():
        marks, rows, counts = [], [], {}
        for f, ts in tables.items():
            st = state[f]
            if st["banner"]:
                marks.append({"file": f, "line": 0, "level": "error", "glyph": "!", "text": "temoc failed: " + st["banner"], "banner": True})
            ss = [s for s in (table_status(f, t) for t in ts) if s]
            if st["banner"] or ss:
                s = "running" if st["running"] else "error" if st["banner"] else max(ss + ["partial"] * (len(ts) - len(ss)), key=worse.index)
                marks.append({"file": f, "line": 0, "level": level(s), "glyph": words[s],
                              "text": counted(s, [r for t in ts for r in t["rows"]], f)})
            for t in ts:
                s = table_status(f, t)
                if s:
                    text = counted(s, t["rows"], f) + ("\n" + t["own"] if t["line"] in st["own"] else "")
                    marks.append({"file": f, "line": t["line"], "level": level(s), "glyph": words[s], "text": text})
            for r, (s, notes) in sorted(st["rows"].items()):
                rows.append({"file": f, "line": r, "level": level(s), "glyph": words[s], "text": "\n".join(notes)})
                if s != "running":
                    counts[s] = counts.get(s, 0) + 1
        running = any(st["running"] or st.get("busy") for st in state.values())
        bad, ok, zz = counts.get("failed", 0) + counts.get("error", 0), counts.get("passed", 0), counts.get("snoozed", 0)
        tally = " ".join(x for x in [f"{bad} ✗" if bad else "", f"{ok} ✓", f"💤 {zz}" if zz else ""] if x)
        what = command[-1].split(" -- ")[-1]
        tag = ({"glyph": "🧪", "text": "temoc " + what.split("/")[-1] + "…", "hint": "temoc: running " + what} if running
               else {"glyph": "🧪", "text": "failed", "hint": "temoc: " + bill + ": " + banner[bill]} if state[bill]["banner"]
               else {"glyph": "🧪", "text": tally, "hint": "temoc at 3f9c2a1"})
        note = (f"temoc at `3f9c2a1`: {bad} did not pass, {ok} passed\n\n**{spec}**\n" +
                "".join(f"- `{spec}:{r}` {n}\n" for r, (s, ns) in sorted(state[spec]["rows"].items())
                        if s in ("failed", "error") for n in ns))
        print(json.dumps({"pr": url, "tags": [tag], "note": note,
                          "marks": {"ref": HEAD, "command": command[-1], "lines": marks + rows}}, ensure_ascii=False), flush=True)

    def run(file="", line=0, delay=0.6):
        files = [file] if file else list(tables)
        targets = []  # (file, table, rows)
        for f in files:
            for t in tables[f]:
                if not line or line == t["line"]:
                    targets.append((f, t, t["rows"]))
                elif line in t["rows"]:
                    targets.append((f, t, [line]))
        command.append("temoc -c Config.toml --output jsonl -- " + (f"{file}:{line}" if line else file or " ".join(files)))
        for f, t, rows in targets:  # clear what's about to run
            for r in rows:
                state[f]["rows"].pop(r, None)
            state[f]["own"].discard(t["line"])
            if not line:
                state[f]["banner"] = None
            state[f]["busy"] = True
        if delay:
            line_out()
        for f, t, rows in targets:
            if f in banner:  # its hooks fail: nothing in it runs
                state[f]["banner"] = banner[f]
                for r in rows:
                    state[f]["rows"][r] = final[r]
                continue
            state[f]["running"] = t["line"]
            for r in rows:
                state[f]["rows"][r] = ("running", [])
                if delay:
                    line_out()
                    time.sleep(delay)
                state[f]["rows"][r] = final.get(r, ("passed", []))
            if t["own"]:
                state[f]["own"].add(t["line"])
            state[f]["running"] = None
        for st in state.values():
            st["busy"] = False
        line_out()

    run(delay=0)  # at start: as if a run of the PR had just ended
    print(json.dumps({"health": "ok"}), flush=True)
    print(json.dumps({"actions": [{"id": "run", "key": "T", "label": "run the PR's temoc specs",
                                   "button": "Run specs", "line": True}]}, ensure_ascii=False), flush=True)
    for l in sys.stdin:
        try:
            a = json.loads(l)
        except ValueError:
            continue
        if a.get("action") == "run" and a.get("pr") == url:
            run(a.get("file", ""), a.get("line", 0) if a.get("file") else 0)


if __name__ == "__main__":
    role = sys.argv[1] if len(sys.argv) > 1 else ""
    if role == "gh":
        sys.exit(gh(sys.argv[2:]))
    if role == "plugin":
        plugin()
    if role == "temoc":
        sys.exit(temoc())
    sys.exit("usage: demo.py gh ... | demo.py plugin | demo.py temoc")
