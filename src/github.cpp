#include "github.hpp"

#include <future>
#include <mutex>

#include "proc.hpp"

namespace anton {

using nlohmann::json;

static const char* kFields = R"(
fragment F on PullRequest {
  number title url isDraft state createdAt updatedAt closedAt additions deletions changedFiles reviewDecision mergeable
  repository { nameWithOwner owner { login } }
  author { login }
  labels(first: 20) { nodes { name } }
  latestReviews(first: 20) { nodes { author { login } state submittedAt } }
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
  pr.changed_files = num("changedFiles");
  pr.review_decision = sget(n, "reviewDecision");
  pr.created = parse_time(sget(n, "createdAt"));
  pr.updated = parse_time(sget(n, "updatedAt"));
  pr.closed = parse_time(sget(n, "closedAt"));
  pr.mergeable = sget(n, "mergeable");
  pr.repo = sget(at(n, "/repository"), "nameWithOwner");
  pr.owner = sget(at(n, "/repository/owner"), "login");
  pr.author = sget(at(n, "/author"), "login");
  json reviews = at(n, "/latestReviews/nodes"), commits = at(n, "/commits/nodes");
  if (reviews.is_array())
    for (auto& r : reviews)
      if (sget(at(r, "/author"), "login") == viewer) {
        pr.my_review = sget(r, "state");
        pr.my_review_at = sget(r, "submittedAt");
      }
  if (json labels = at(n, "/labels/nodes"); labels.is_array())
    for (auto& l : labels)
      if (std::string name = sget(l, "name"); !name.empty()) pr.labels.push_back(name);
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
    std::string body = aliases(urls, a, b, "state closedAt");
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
      pr.closed = parse_time(sget(n, "closedAt"));
      pr.partial = true;
      f.prs.push_back(pr);
    }
  });
  return f;
}

static std::string gh_error(const Result& r) {
  std::string e = r.timed_out ? "gh timed out" : first_line(r.err.empty() ? r.out : r.err);
  return e.empty() ? "gh: unreadable response" : e;
}

std::string fetch_body(const std::string& url, std::string& head, std::string& error) {
  Result r = run({"gh", "pr", "view", url, "--json", "body,headRefOid"}, "", true, 30);
  json j = json::parse(r.out, nullptr, false);
  if (r.code == 0 && j.is_object()) {
    head = sget(j, "headRefOid");
    return sget(j, "body");
  }
  error = gh_error(r);
  return "";
}

// "docs/a b.md" -> "docs/a%20b.md": a path as one segment of the contents URL.
static std::string url_path(const std::string& path) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : path)
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  return out;
}

std::vector<File> fetch_md_files(const std::string& url, const std::string& head, std::string& error) {
  auto ref = parse_ref(url);
  if (!ref) {
    error = "not a PR URL: " + url;
    return {};
  }
  std::string repo = "repos/" + ref->owner + "/" + ref->name;
  Result r = run({"gh", "api", "--paginate", repo + "/pulls/" + std::to_string(ref->number) + "/files", "--jq",
                  ".[] | select(.status != \"removed\") | .filename"},
                 "", true, 60);
  if (r.code != 0) {
    error = gh_error(r);
    return {};
  }
  std::vector<File> files;
  size_t at = 0;
  for (size_t nl; (nl = r.out.find('\n', at)) != std::string::npos; at = nl + 1) {
    std::string path = r.out.substr(at, nl - at), low = lower(path);
    if (low.ends_with(".md") || low.ends_with(".markdown")) files.push_back({path, "", ""});
  }
  // ponytail: every file is fetched up front, 6 at a time, because tab order
  // (specs first) needs their text; a PR with hundreds of .md files waits for
  // all of them. Fetch per tab and sort on what's in if that ever matters.
  parallel_chunks(files.size(), 1, [&](size_t a, size_t) {
    Result c = run({"gh", "api", "-H", "Accept: application/vnd.github.raw",
                    repo + "/contents/" + url_path(files[a].path) + "?ref=" + head},
                   "", true, 60);
    if (c.code == 0) files[a].text = c.out;
    else files[a].error = gh_error(c);
  });
  return files;
}

}  // namespace anton
