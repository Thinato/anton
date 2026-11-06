#include "github.hpp"

#include <future>
#include <mutex>

#include "proc.hpp"

namespace anton {

using nlohmann::json;

static const char* kFields = R"(
fragment F on PullRequest {
  number title url isDraft state createdAt updatedAt additions deletions reviewDecision
  repository { nameWithOwner owner { login } }
  author { login }
  latestReviews(first: 20) { nodes { author { login } state } }
  commits(last: 1) { nodes { commit { statusCheckRollup { state } } } }
})";

static std::string first_line(const std::string& s) {
  auto nl = s.find('\n');
  return nl == std::string::npos ? s : s.substr(0, nl);
}

// gh exits non-zero when the response has "errors" even if "data" is partial
// (e.g. one aliased PR was deleted), so data wins over the exit code.
static json graphql(const std::vector<std::string>& args, std::string& error) {
  std::vector<std::string> argv = {"gh", "api", "graphql"};
  argv.insert(argv.end(), args.begin(), args.end());
  Result r = run(argv);
  json j = json::parse(r.out, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("data") || !j["data"].is_object()) {
    error = r.code == -1 ? r.err : first_line(r.err.empty() ? r.out : r.err);
    if (error.empty()) error = "gh: unreadable response";
    return json::object();
  }
  // ponytail: partial errors (a PR in a repo you can't see) are dropped with
  // that PR; surface them if someone needs to know why a row is missing.
  return j["data"];
}

static std::string sget(const json& j, const char* k) {
  if (!j.is_object()) return "";
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

// j at a JSON pointer, or null for any missing/mistyped step. GitHub returns
// null for many nested objects (deleted authors, commits without checks).
static json at(const json& j, const char* ptr) {
  try {
    return j.at(json::json_pointer(ptr));
  } catch (const json::exception&) {
    return json();
  }
}

static Pr to_pr(const json& n, const std::string& viewer) {
  Pr pr;
  pr.url = normalize_url(sget(n, "url"));
  auto num = [&](const char* k) { return n.contains(k) && n[k].is_number() ? n[k].get<long>() : 0L; };
  pr.number = static_cast<int>(num("number"));
  pr.title = sget(n, "title");
  pr.state = sget(n, "state");
  pr.draft = n.contains("isDraft") && n["isDraft"].is_boolean() && n["isDraft"].get<bool>();
  pr.additions = num("additions");
  pr.deletions = num("deletions");
  pr.review_decision = sget(n, "reviewDecision");
  pr.created = parse_time(sget(n, "createdAt"));
  pr.updated = parse_time(sget(n, "updatedAt"));
  pr.repo = sget(at(n, "/repository"), "nameWithOwner");
  pr.owner = sget(at(n, "/repository/owner"), "login");
  pr.author = sget(at(n, "/author"), "login");
  json reviews = at(n, "/latestReviews/nodes"), commits = at(n, "/commits/nodes");
  if (reviews.is_array())
    for (auto& r : reviews)
      if (sget(at(r, "/author"), "login") == viewer) pr.my_review = sget(r, "state");
  if (commits.is_array())
    for (auto& c : commits) pr.checks = sget(at(c, "/commit/statusCheckRollup"), "state");
  return pr;
}

Fetch search_prs(const std::string& query, int max) {
  Fetch f;
  const std::string q =
      std::string(kFields) +
      "\nquery($q: String!, $after: String) { viewer { login }"
      " search(query: $q, type: ISSUE, first: 100, after: $after) {"
      " pageInfo { hasNextPage endCursor } nodes { ...F } } }";
  std::string after;
  while (static_cast<int>(f.prs.size()) < max) {
    std::vector<std::string> args = {"-f", "query=" + q, "-f", "q=" + query};
    if (!after.empty()) args.insert(args.end(), {"-f", "after=" + after});
    json d = graphql(args, f.error);
    if (d.empty()) break;
    f.viewer = sget(at(d, "/viewer"), "login");
    json nodes = at(d, "/search/nodes");
    if (nodes.is_array())
      for (auto& n : nodes)
        if (n.is_object() && n.contains("number")) f.prs.push_back(to_pr(n, f.viewer));
    if (at(d, "/search/pageInfo/hasNextPage") != json(true)) break;
    after = sget(at(d, "/search/pageInfo"), "endCursor");
    if (after.empty()) break;
  }
  return f;
}

// Runs work(begin, end) over [0, n) in chunks, at most 6 at a time.
template <class Work>
static void parallel_chunks(size_t n, size_t chunk, Work work) {
  std::vector<std::future<void>> running;
  for (size_t start = 0; start < n; start += chunk) {
    if (running.size() >= 6) {
      running.front().get();
      running.erase(running.begin());
    }
    running.push_back(std::async(std::launch::async, work, start, std::min(n, start + chunk)));
  }
  for (auto& r : running) r.get();
}

// "p<i>: repository(...) { pullRequest(number: N) { <sel> } }" for urls[a, b).
static std::string aliases(const std::vector<std::string>& urls, size_t a, size_t b,
                           const std::string& sel) {
  std::string body;
  for (size_t i = a; i < b; ++i) {
    auto ref = parse_ref(urls[i]);
    if (!ref) continue;
    body += " p" + std::to_string(i) + ": repository(owner: " + json(ref->owner).dump() +
            ", name: " + json(ref->name).dump() + ") { pullRequest(number: " +
            std::to_string(ref->number) + ") { " + sel + " } }";
  }
  return body;
}

Fetch fetch_prs(const std::vector<std::string>& urls) {
  Fetch f;
  std::mutex mu;
  parallel_chunks(urls.size(), 10, [&](size_t a, size_t b) {
    std::string body = aliases(urls, a, b, "...F");
    if (body.empty()) return;
    std::string err;
    json d = graphql({"-f", "query=" + std::string(kFields) + "\nquery { viewer { login }" + body + " }"}, err);
    std::lock_guard lk(mu);
    if (!err.empty()) f.error = err;
    if (d.empty()) return;
    f.viewer = sget(at(d, "/viewer"), "login");
    for (auto& [key, repo] : d.items()) {
      json n = at(repo, "/pullRequest");
      if (key.size() < 2 || key[0] != 'p' || !n.is_object()) continue;
      Pr pr = to_pr(n, f.viewer);
      // Keyed by the URL we were asked for, not GitHub's canonical one: they
      // differ after a repo rename/transfer, and the caller looks it up by ours.
      pr.url = urls[std::stoul(key.substr(1))];
      f.prs.push_back(pr);
    }
  });
  return f;
}

Fetch fetch_states(const std::vector<std::string>& urls) {
  Fetch f;
  std::mutex mu;
  parallel_chunks(urls.size(), 100, [&](size_t a, size_t b) {
    std::string body = aliases(urls, a, b, "state");
    if (body.empty()) return;
    std::string err;
    json d = graphql({"-f", "query=query {" + body + " }"}, err);
    std::lock_guard lk(mu);
    if (!err.empty()) f.error = err;
    for (auto& [key, repo] : d.items()) {
      if (key.size() < 2 || key[0] != 'p') continue;
      size_t i = std::stoul(key.substr(1));  // "p<i>" -> urls[i]
      json n = at(repo, "/pullRequest");
      auto ref = parse_ref(urls[i]);
      if (!n.is_object() || !ref) continue;
      Pr pr;
      pr.url = urls[i];
      pr.owner = ref->owner;
      pr.repo = ref->owner + "/" + ref->name;
      pr.number = ref->number;
      pr.state = sget(n, "state");
      pr.partial = true;
      f.prs.push_back(pr);
    }
  });
  return f;
}

}  // namespace anton
