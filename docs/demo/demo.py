#!/usr/bin/env python3
"""Made-up data for anton's screenshot. Two roles:

  demo.py gh ...    stands in for `gh` (auth status, api graphql, pr view)
  demo.py plugin    stands in for anton-slkx: the code-review list, 👀 tags,
                    deploy statuses

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
        "repository": {"nameWithOwner": f"acme/{repo}", "owner": {"login": "acme"}},
        "author": {"login": author},
        "latestReviews": {"nodes": [{"author": {"login": ME}, "state": "APPROVED",
                                     "submittedAt": iso(NOW - H)}] if state == "you_ok" else []},
        "commits": {"nodes": [{"commit": {"statusCheckRollup": {
            "state": "FAILURE" if state == "checks" else "SUCCESS"}}}]},
    }
    return n


def gh(args):
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
    out({"actions": [{"id": "review", "key": "v", "label": "toggle 👀 reviewing in Slack"}]})
    for _ in sys.stdin:  # stay up like the real plugin; ignore `v`
        pass
    time.sleep(10 ** 9)


if __name__ == "__main__":
    role = sys.argv[1] if len(sys.argv) > 1 else ""
    if role == "gh":
        sys.exit(gh(sys.argv[2:]))
    if role == "plugin":
        plugin()
    sys.exit("usage: demo.py gh ... | demo.py plugin")
