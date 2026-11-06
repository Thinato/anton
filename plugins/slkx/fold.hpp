#pragma once
// anton-slkx's pure core: Slack messages and RTM events in, anton plugin lines
// out. No I/O, so tests/fold_check.cpp drives it with recorded JSON.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "model.hpp"  // normalize_url

namespace anton::slkx {

using nlohmann::json;

struct Step {
  std::string emoji, glyph, text;
};

// One reaction to add or remove on a message.
struct React {
  std::string ch, ts, emoji;
  bool remove = false;
};

struct FoldConfig {
  std::string review_ch, deploy_ch;  // channel ids; "" = rule off
  std::set<std::string> review_emoji = {"eyes", "zoiando"};
  std::string review_add = "eyes";  // what `v` puts on: the first --review-emoji
  std::set<std::string> clear_emoji = {"p2r2-rejected-pr"};
  Step posted = {"", "⚙", "queued"};  // a deploy post with no step reaction yet
  std::vector<Step> steps = {         // in pipeline order: the furthest present wins
      {"p2r2-picked-up-your-pr", "⚙", "queued"},
      {"p2r2-validated-your-pr", "⚙", "queued"},
      {"p2r2-waiting-for-dev", "💬", "Ready?"},
      {"p2r2-pr-queued-for-merge", "⚙", "queued"},
      {"p2r2-pr-merging", "⚙", "queued"},
  };
  std::string approve_emoji = "+1";          // replaces your review emoji once you approve
  std::string me;                            // your user id: renders as "you"
  std::map<std::string, std::string> names;  // user id -> handle
};

inline std::vector<std::string> pr_urls(const std::string& text) {
  static const std::regex re(R"(https://github\.com/[\w.-]+/[\w.-]+/pull/\d+)");
  std::vector<std::string> out;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
    std::string u = normalize_url(it->str());
    if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
  }
  return out;
}

class Fold {
 public:
  explicit Fold(FoldConfig c) : c_(std::move(c)) {}

  // A message as `slkx read --json` returns it (reactions included). Returns
  // the PR URLs it names.
  std::set<std::string> add_message(const std::string& ch, const json& m) {
    if (!watched(ch) || !m.is_object()) return {};
    std::string sub = s(m, "subtype"), ts = s(m, "ts");
    // A person's post, a bot's post (GitHub apps link PRs too), or a reply
    // sent to the channel. Not edits/deletes (event() handles them), joins,
    // topic changes...
    if (!sub.empty() && sub != "thread_broadcast" && sub != "bot_message") return {};
    if (is_plain_reply(m)) return {};
    auto urls = pr_urls(s(m, "text"));
    if (urls.empty()) return {};
    Msg& msg = msgs_[{ch, ts}];
    msg.urls = urls;
    auto rs = m.find("reactions");
    if (rs != m.end() && rs->is_array())
      for (auto& r : *rs) {
        auto us = r.is_object() ? r.find("users") : r.end();
        if (us == r.end() || !us->is_array()) continue;
        for (auto& u : *us)
          if (u.is_string()) msg.reacts.insert({s(r, "name"), u.get<std::string>()});
      }
    return {urls.begin(), urls.end()};
  }

  // One `slkx watch` line. Returns the PR URLs whose output may have changed.
  std::set<std::string> event(const json& ev) {
    if (!ev.is_object()) return {};
    std::string type = s(ev, "type");
    if (type == "message") {
      std::string ch = s(ev, "channel"), sub = s(ev, "subtype");
      if (sub == "message_changed") {
        json m = obj(ev, "message");
        auto key = std::make_pair(ch, s(m, "ts"));
        std::set<std::string> touched = urls_of(key);
        auto urls = is_plain_reply(m) ? std::vector<std::string>{} : pr_urls(s(m, "text"));
        if (!watched(ch)) return {};
        if (urls.empty()) {
          msgs_.erase(key);
        } else {
          msgs_[key].urls = urls;  // reactions stay
          touched.insert(urls.begin(), urls.end());
        }
        return touched;
      }
      if (sub == "message_deleted") {
        auto key = std::make_pair(ch, s(ev, "deleted_ts"));
        std::set<std::string> touched = urls_of(key);
        msgs_.erase(key);
        return touched;
      }
      return add_message(ch, ev);
    }
    if (type == "reaction_added" || type == "reaction_removed") {
      json item = obj(ev, "item");
      auto it = msgs_.find({s(item, "channel"), s(item, "ts")});
      if (it == msgs_.end()) return {};
      std::pair<std::string, std::string> r = {s(ev, "reaction"), s(ev, "user")};
      if (type == "reaction_added")
        it->second.reacts.insert(r);
      else
        it->second.reacts.erase(r);
      return {it->second.urls.begin(), it->second.urls.end()};
    }
    return {};
  }

  // The full anton line for one PR. {"pr": url} alone clears it.
  json line(const std::string& url) const {
    bool listed = false;
    std::set<std::string> reviewers;
    const Msg* deploy = nullptr;
    std::string deploy_ts, review_ts;
    for (auto& [key, msg] : msgs_) {
      if (std::find(msg.urls.begin(), msg.urls.end(), url) == msg.urls.end()) continue;
      if (key.first == c_.review_ch) {
        listed = true;
        review_ts = std::max(review_ts, key.second);
        for (auto& [emoji, user] : msg.reacts)
          if (c_.review_emoji.count(emoji)) reviewers.insert(user);
      }
      // Slack ts strings are fixed-width ("1793899716.006249"), so string
      // order is time order. The newest unrejected deploy post decides.
      if (key.first == c_.deploy_ch && !rejected(msg) && key.second > deploy_ts) {
        deploy = &msg;
        deploy_ts = key.second;
      }
    }
    json j = {{"pr", url}};
    if (listed) {
      j["lists"] = {"code-review"};
      // Newest post first, like the channel. A double holds a ts to ~1µs.
      j["order"] = std::strtod(review_ts.c_str(), nullptr);
    }
    if (deploy) {
      const Step* st = &c_.posted;
      for (auto& step : c_.steps)
        for (auto& [emoji, user] : deploy->reacts)
          if (emoji == step.emoji) st = &step;  // later steps overwrite: furthest wins
      j["status"] = {{"glyph", st->glyph}, {"text", st->text}};
    }
    std::vector<std::string> handles;
    for (auto& u : reviewers) handles.push_back(u == c_.me ? "you" : name(u));
    std::sort(handles.begin(), handles.end());
    if (!handles.empty()) {
      j["tags"] = json::array();
      for (auto& h : handles)
        j["tags"].push_back({{"glyph", "👀"}, {"text", h}, {"hint", "reviewing, via Slack"}});
    }
    return j;
  }

  // `v` in anton: your review emoji off every post naming the PR if you have
  // any, else the first review emoji on its newest post. {} if not posted.
  std::vector<React> toggle_review(const std::string& url) const {
    if (c_.me.empty() || c_.review_emoji.empty()) return {};
    auto out = my_review_reacts(url, false);
    if (!out.empty()) return out;
    auto post = newest_review_post(url, false);
    if (!post) return {};
    return {{post->first, post->second, c_.review_add}};
  }

  // You approved on GitHub: if you were reviewing, the approve emoji goes on
  // the newest post and your review emoji come off. Only posts naming just
  // this PR: a shared "stack: A, B, C" post is not A's to change. 👍 first, so
  // a failure part way leaves you still marked as reviewing, not neither.
  std::vector<React> approved(const std::string& url) const {
    if (c_.me.empty()) return {};
    auto offs = my_review_reacts(url, true);
    if (offs.empty()) return {};
    auto post = newest_review_post(url, true);
    std::vector<React> out;
    bool has = false;
    for (auto& [emoji, user] : msgs_.at(*post).reacts)  // 👍 with a skin tone is "+1::skin-tone-2"
      has = has || (user == c_.me && emoji.substr(0, emoji.find("::")) == c_.approve_emoji);
    if (!has) out.push_back({post->first, post->second, c_.approve_emoji});
    out.insert(out.end(), offs.begin(), offs.end());
    return out;
  }

  // The reaction event Slack will echo back for r, by you. Applying it before
  // the echo shows the change at once; the echo is then a no-op.
  json as_event(const React& r, bool undo = false) const {
    return {{"type", r.remove != undo ? "reaction_removed" : "reaction_added"},
            {"user", c_.me},
            {"reaction", r.emoji},
            {"item", {{"type", "message"}, {"channel", r.ch}, {"ts", r.ts}}}};
  }

  // Lines for the given PRs that differ from what was last emitted.
  std::vector<std::string> changed(const std::set<std::string>& urls) {
    std::vector<std::string> out;
    for (auto& u : urls) {
      std::string l = line(u).dump(-1, ' ', false, json::error_handler_t::replace);
      if (last_[u] == l) continue;
      last_[u] = l;
      out.push_back(l);
    }
    return out;
  }

 private:
  struct Msg {
    std::vector<std::string> urls;
    std::set<std::pair<std::string, std::string>> reacts;  // (emoji, user)
  };

  static json obj(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_object() ? *it : json::object();
  }
  // A thread reply that wasn't also sent to the channel.
  static bool is_plain_reply(const json& m) {
    std::string thread = s(m, "thread_ts");
    return !thread.empty() && thread != s(m, "ts") && s(m, "subtype") != "thread_broadcast";
  }
  static std::string s(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
  }
  bool watched(const std::string& ch) const {
    return !ch.empty() && (ch == c_.review_ch || ch == c_.deploy_ch);
  }
  bool rejected(const Msg& m) const {
    for (auto& [emoji, user] : m.reacts)
      if (c_.clear_emoji.count(emoji)) return true;
    return false;
  }
  std::string name(const std::string& id) const {
    auto it = c_.names.find(id);
    return it != c_.names.end() ? it->second : id;
  }
  bool review_post(const std::pair<std::string, std::string>& key, const Msg& msg, const std::string& url,
                   bool only_this) const {
    return key.first == c_.review_ch && std::find(msg.urls.begin(), msg.urls.end(), url) != msg.urls.end() &&
           (!only_this || msg.urls.size() == 1);
  }
  std::vector<React> my_review_reacts(const std::string& url, bool only_this) const {
    std::vector<React> out;
    for (auto& [key, msg] : msgs_) {
      if (!review_post(key, msg, url, only_this)) continue;
      for (auto& [emoji, user] : msg.reacts)
        if (user == c_.me && c_.review_emoji.count(emoji)) out.push_back({key.first, key.second, emoji, true});
    }
    return out;
  }
  std::optional<std::pair<std::string, std::string>> newest_review_post(const std::string& url,
                                                                         bool only_this) const {
    std::optional<std::pair<std::string, std::string>> best;
    for (auto& [key, msg] : msgs_)
      if (review_post(key, msg, url, only_this) && (!best || key.second > best->second))
        best = key;
    return best;
  }
  std::set<std::string> urls_of(const std::pair<std::string, std::string>& key) const {
    auto it = msgs_.find(key);
    if (it == msgs_.end()) return {};
    return {it->second.urls.begin(), it->second.urls.end()};
  }

  FoldConfig c_;
  std::map<std::pair<std::string, std::string>, Msg> msgs_;
  std::map<std::string, std::string> last_;  // url -> last emitted line
};

}  // namespace anton::slkx
