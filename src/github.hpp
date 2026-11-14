#pragma once
// GitHub via `gh api graphql`: the search path for search views and the alias
// path for list views. Both return the same Pr fields.

#include <string>
#include <vector>

#include "model.hpp"

namespace anton {

struct Fetch {
  std::vector<Pr> prs;
  std::string viewer;  // login of the gh user
  std::string error;   // first gh stderr line; prs may still be partial
};

// GitHub search syntax, passed through. Pages of 100 up to `max`.
Fetch search_prs(const std::string& query, int max = 500);

// Exact PRs by URL, full fields. Unknown/inaccessible ones are skipped.
// additions/deletions make this slow (~0.2s per PR), so it runs 10 PRs per
// request, 6 requests at a time.
Fetch fetch_prs(const std::vector<std::string>& urls);

// Just the state of each PR (OPEN/CLOSED/MERGED), 100 per request: ~1s for a
// thousand. Lets list views skip full fetches of PRs that are long merged.
// Returned as partial Prs.
Fetch fetch_states(const std::vector<std::string>& urls);

// A PR's description (markdown, "" if it has none), via `gh pr view`. On
// failure returns "" and sets error to gh's first stderr line.
std::string fetch_body(const std::string& url, std::string& error);

}  // namespace anton
