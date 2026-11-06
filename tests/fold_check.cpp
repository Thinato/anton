// Assert-based self-check for plugins/slkx/fold.hpp.
#include <cassert>
#include <cstdio>

#include "fold.hpp"

using namespace anton::slkx;
using nlohmann::json;

static const std::string PR1 = "https://github.com/o/r/pull/1";
static const std::string PR2 = "https://github.com/o/r/pull/2";

static json msg(const std::string& ch, const std::string& ts, const std::string& text) {
  return {{"type", "message"}, {"channel", ch}, {"user", "U1"}, {"ts", ts}, {"text", text}};
}
static json react(bool add, const std::string& ch, const std::string& ts, const std::string& emoji,
                  const std::string& user) {
  return {{"type", add ? "reaction_added" : "reaction_removed"},
          {"user", user},
          {"reaction", emoji},
          {"item", {{"type", "message"}, {"channel", ch}, {"ts", ts}}}};
}
static std::vector<std::string> handles(const json& line) {
  std::vector<std::string> out;
  for (auto& t : line.value("tags", json::array())) out.push_back(t["text"]);
  return out;
}

int main() {
  FoldConfig c;
  c.review_ch = "CR";
  c.deploy_ch = "DP";
  c.me = "UME";
  c.names = {{"UA", "alice"}, {"UB", "bob"}};
  Fold f(c);

  // URL extraction: Slack's <url|label> wrapping, dedupe, /files suffix
  auto urls = pr_urls("<https://github.com/o/r/pull/1|o/r#1> and https://github.com/o/r/pull/1/files "
                      "https://github.com/o/r/pull/2");
  assert(urls.size() == 2 && urls[0] == PR1 && urls[1] == PR2);

  // backfill: a history message with reactions; other emoji ignored
  json hist = msg("CR", "1700000000.000100", "<" + PR1 + "|x> - feat: thing");
  hist["reactions"] = {{{"name", "eyes"}, {"users", {"UA"}}},
                       {{"name", "+1"}, {"users", {"UB"}}},
                       {{"name", "zoiando"}, {"users", {"UME"}}}};
  assert(f.add_message("CR", hist).count(PR1));
  json l = f.line(PR1);
  assert(l["lists"] == json({"code-review"}));
  assert((handles(l) == std::vector<std::string>{"alice", "you"}));
  assert(l["watch"] == true);  // you're reviewing it
  assert(!l.contains("status"));
  assert(l["order"] == 1700000000.0001);

  // changed() emits once, then dedupes
  assert(f.changed({PR1}).size() == 1);
  assert(f.changed({PR1}).empty());

  // live: messages in unwatched channels and plain thread replies are ignored
  assert(f.event(msg("OTHER", "1700000000.000200", PR2)).empty());
  json reply = msg("CR", "1700000000.000300", PR2);
  reply["thread_ts"] = "1700000000.000100";
  assert(f.event(reply).empty());
  json broadcast = reply;
  broadcast["subtype"] = "thread_broadcast";
  assert(f.event(broadcast).count(PR2));  // broadcasts count

  // live reactions
  f.event(react(true, "CR", "1700000000.000100", "eyes", "UB"));
  assert((handles(f.line(PR1)) == std::vector<std::string>{"alice", "bob", "you"}));
  f.event(react(false, "CR", "1700000000.000100", "eyes", "UA"));
  assert((handles(f.line(PR1)) == std::vector<std::string>{"bob", "you"}));
  assert(f.event(react(true, "CR", "9999999999.000000", "eyes", "UA")).empty());  // unknown message

  // repost: reviewers are the union; deleting one post keeps the PR listed
  f.event(msg("CR", "1700000000.000400", PR1));
  f.event(react(true, "CR", "1700000000.000400", "eyes", "UA"));
  assert((handles(f.line(PR1)) == std::vector<std::string>{"alice", "bob", "you"}));
  assert(f.line(PR1)["order"] == 1700000000.0004);  // the repost moves it to the top
  json del = {{"type", "message"}, {"subtype", "message_deleted"}, {"channel", "CR"},
              {"deleted_ts", "1700000000.000100"}};
  assert(f.event(del).count(PR1));
  assert(f.line(PR1).contains("lists"));
  assert((handles(f.line(PR1)) == std::vector<std::string>{"alice"}));

  // edit removing the URL takes the message out; last post gone -> cleared
  json edit = {{"type", "message"}, {"subtype", "message_changed"}, {"channel", "CR"},
               {"message", {{"ts", "1700000000.000400"}, {"text", "oops, wrong link"}}}};
  assert(f.event(edit).count(PR1));
  assert(f.line(PR1) == json({{"pr", PR1}}));
  // edit adding a URL to a message that had none
  json edit2 = {{"type", "message"}, {"subtype", "message_changed"}, {"channel", "CR"},
                {"message", {{"ts", "1700000000.000400"}, {"text", PR1}}}};
  assert(f.event(edit2).count(PR1) && f.line(PR1).contains("lists"));

  // editing a plain thread reply must not list its PR (review finding)
  json reply_edit = {{"type", "message"}, {"subtype", "message_changed"}, {"channel", "CR"},
                     {"message", {{"ts", "1700000000.000500"}, {"thread_ts", "1700000000.000100"},
                                  {"text", "https://github.com/o/r/pull/9"}}}};
  f.event(reply_edit);
  assert(f.line("https://github.com/o/r/pull/9") == json({{"pr", "https://github.com/o/r/pull/9"}}));
  // joins/topic changes don't count; bot posts do
  json topic = msg("CR", "1700000000.000600", "https://github.com/o/r/pull/8");
  topic["subtype"] = "channel_topic";
  assert(f.event(topic).empty());
  json bot = msg("CR", "1700000000.000700", "https://github.com/o/r/pull/8");
  bot["subtype"] = "bot_message";
  assert(f.event(bot).count("https://github.com/o/r/pull/8"));
  // malformed reactions are ignored, not fatal
  json weird = msg("CR", "1700000000.000800", "https://github.com/o/r/pull/7");
  weird["reactions"] = {"not-an-object", {{"name", "eyes"}, {"users", nullptr}}};
  assert(f.add_message("CR", weird).size() == 1);

  // deploy: posted -> queued; waiting-for-dev -> Ready?; next step -> queued again
  f.event(msg("DP", "1700000001.000000", PR2));
  assert(f.line(PR2)["status"]["text"] == "queued");
  f.event(react(true, "DP", "1700000001.000000", "p2r2-picked-up-your-pr", "BOT"));
  f.event(react(true, "DP", "1700000001.000000", "p2r2-waiting-for-dev", "BOT"));
  assert(f.line(PR2)["status"]["text"] == "Ready?");
  assert(f.line(PR2)["status"]["glyph"] == "💬");
  f.event(react(true, "DP", "1700000001.000000", "p2r2-pr-queued-for-merge", "BOT"));
  assert(f.line(PR2)["status"]["text"] == "queued");
  assert(f.line(PR2)["lists"] == json({"code-review"}));  // from the broadcast earlier

  // merged reaction: anton should re-check GitHub now
  assert(!f.line(PR2).contains("watch"));
  f.event(react(true, "DP", "1700000001.000000", "p2r2-pr-merged", "BOT"));
  assert(f.line(PR2)["watch"] == true);

  // rejected -> no status; a newer post re-queues it
  f.event(react(true, "DP", "1700000001.000000", "p2r2-rejected-pr", "BOT"));
  assert(!f.line(PR2).contains("status"));
  f.event(msg("DP", "1700000002.000000", PR2));
  assert(f.line(PR2)["status"]["text"] == "queued");

  // deploy-only PR is not on the code-review list
  f.event(msg("DP", "1700000003.000000", "https://github.com/o/r/pull/3"));
  assert(!f.line("https://github.com/o/r/pull/3").contains("lists"));

  // review toggle (v in anton) and approval, on a fresh fold
  {
    Fold g(c);
    const std::string P = "https://github.com/o/r/pull/50";
    assert(g.toggle_review(P).empty());  // not posted: nothing to react to
    g.event(msg("CR", "1700000010.000000", P));
    g.event(msg("CR", "1700000020.000000", P));  // repost: the newest gets the emoji
    auto t = g.toggle_review(P);
    assert(t.size() == 1 && t[0].ts == "1700000020.000000" && t[0].emoji == "eyes" && !t[0].remove);
    assert(g.approved(P).empty());  // not reviewing: approval touches nothing
    g.event(g.as_event(t[0]));      // applied before Slack echoes it
    assert((handles(g.line(P)) == std::vector<std::string>{"you"}));
    g.event(react(true, "CR", "1700000010.000000", "zoiando", "UME"));
    g.event(react(true, "CR", "1700000010.000000", "eyes", "UA"));  // others' stay
    t = g.toggle_review(P);  // on -> off: every review emoji of yours comes off
    assert(t.size() == 2 && t[0].remove && t[1].remove);
    assert(g.as_event(t[0], true)["type"] == "reaction_added");  // undoing a removal re-adds
    auto a = g.approved(P);  // 👍 first, then your review emoji off
    assert(a.size() == 3 && a[0].emoji == "+1" && !a[0].remove && a[0].ts == "1700000020.000000");
    assert(a[1].remove && a[2].remove);
    for (auto& r : a) g.event(g.as_event(r));
    assert((handles(g.line(P)) == std::vector<std::string>{"alice"}));
    assert(!g.line(P).contains("watch"));  // not yours to review any more
    assert(g.approved(P).empty());  // done: a repeated event is a no-op

    // a shared "stack" post is not one PR's to change on approval
    const std::string Q = "https://github.com/o/r/pull/60", R = "https://github.com/o/r/pull/61";
    g.event(msg("CR", "1700000030.000000", Q + " " + R));
    g.event(react(true, "CR", "1700000030.000000", "eyes", "UME"));
    assert(g.approved(Q).empty());
    g.event(msg("CR", "1700000040.000000", Q));  // Q alone, reviewed, already 👍 with a skin tone
    g.event(react(true, "CR", "1700000040.000000", "eyes", "UME"));
    g.event(react(true, "CR", "1700000040.000000", "+1::skin-tone-3", "UME"));
    a = g.approved(Q);
    assert(a.size() == 1 && a[0].remove && a[0].ts == "1700000040.000000");  // no second 👍; stack post kept
  }

  // v puts on the first configured review emoji, not the alphabetically first
  {
    FoldConfig c2 = c;
    c2.review_emoji = {"zoiando", "eyes"};
    c2.review_add = "zoiando";
    Fold h(c2);
    h.event(msg("CR", "1700000050.000000", PR1));
    assert(h.toggle_review(PR1)[0].emoji == "zoiando");
  }

  std::puts("fold_check: all checks passed");
}
