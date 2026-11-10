// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARCHIVE_AGENT_ARCHIVE_AGENT_H_
#define CHROME_BROWSER_ARCHIVE_AGENT_ARCHIVE_AGENT_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/threading/sequence_bound.h"
#include "base/values.h"

namespace archive_agent {

// Names the Pi command to run, where it is not "pi" on the PATH or in
// ~/.local/bin.
inline constexpr char kCommandSwitch[] = "archive-agent-command";

// Names the model the assistant uses, as Pi's --model takes it, where it is
// not the one Pi is set up to use by default.
inline constexpr char kModelSwitch[] = "archive-agent-model";

// The tools the assistant has, all of which only look: each is answered by
// Delegate::AnswerQuery() about the one tab the assistant belongs to.
//
// "tab_status": whether the tab is archiving or replaying, its page, its
//   archive, and its counts.
// "list_requests": the tab's requests, filtered -- `which` is one of "all",
//   "missing", "failed", "errors" or "closest"; `contains` matches the URL;
//   `limit` (at most 200) and `newest_first` choose which are returned.
// "summarize_requests": the tab's requests counted by host, type and status.
inline constexpr char kTools[] = "tab_status,list_requests,summarize_requests";

// An assistant that can look at -- and only look at -- what one tab archived
// or replayed: Pi (pi.dev), run as a child process in its RPC mode and spoken
// to over its stdin and stdout.
//
// Pi is a coding agent, and does not sandbox what it is asked to do. So it is
// started with none of its own tools -- no files, no shell -- and with none of
// what it would otherwise load from the user's set-up or the directory it runs
// in: no extensions, skills, prompt templates or context files. It is given
// instead the tools in kTools, each of which it can only use by asking the
// browser, through the dialog half of Pi's RPC protocol: a tool opens an
// "editor" dialog titled kQueryChannel holding its query, and the browser
// answers with the result as the edited text. Nothing else crosses: there is
// no socket to find and no file to read.
//
// What the tools return comes from web pages -- URLs, titles, types -- and is
// as untrusted as the pages are. The worst a page that addresses the model can
// do is mislead what the assistant says; there is nothing it can make it do.
//
// The model is reached with the user's own Pi set-up and login. Lives on the
// UI thread.
class ArchiveAgent {
 public:
  class Delegate {
   public:
    // The assistant's answer continues with `text`.
    virtual void OnAgentText(const std::string& text) = 0;

    // The assistant is using `tool` with `arguments`, as JSON.
    virtual void OnAgentToolCall(const std::string& tool,
                                 const std::string& arguments) = 0;

    // The assistant has finished answering, and can be asked again.
    virtual void OnAgentIdle() = 0;

    // Something went wrong, and `message` says what. If the assistant stopped
    // because of it, it is started again when next asked.
    virtual void OnAgentError(const std::string& message) = 0;

    // The assistant's `tool` asks, with `params`; returns the answer, as the
    // JSON the tool hands back to the model.
    virtual std::string AnswerQuery(const std::string& tool,
                                    const base::DictValue& params) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // The dialog title a tool's query arrives under.
  static constexpr char kQueryChannel[] = "chromium-archive-query";

  explicit ArchiveAgent(Delegate& delegate);
  ArchiveAgent(const ArchiveAgent&) = delete;
  ArchiveAgent& operator=(const ArchiveAgent&) = delete;
  ~ArchiveAgent();

  // Asks `question`, starting Pi first if it is not running.
  void Ask(const std::string& question);

  // Stops the answer being given, if one is.
  void Stop();

  // Whether an answer is being given.
  bool busy() const { return busy_; }

  // What a line Pi writes means, for the delegate: exposed for testing.
  void OnLineForTesting(const std::string& line) { OnLine(line); }

  // What starting Pi came to. Public only so that what starts it, off this
  // thread, can say.
  struct Launched;

 private:
  class PipeWriter;

  enum class State { kNotStarted, kStarting, kRunning };

  void OnLaunched(Launched launched);
  void SendPrompt(const std::string& question);
  void Send(base::DictValue command);
  void OnLine(const std::string& line);
  void OnUIRequest(const base::DictValue& request);
  void OnEnded();
  void OnLogRead(const std::string& tail);

  // Lets the process and its directory go: the process is ended and reaped,
  // and the directory deleted, off this thread.
  void Release();

  const raw_ref<Delegate> delegate_;

  State state_ = State::kNotStarted;
  bool busy_ = false;
  int next_id_ = 0;

  // Questions asked before Pi was running, to ask once it is.
  std::vector<std::string> waiting_;

  base::Process process_;
  // Where Pi runs: the extension giving it its tools, and its log.
  base::FilePath directory_;
  base::SequenceBound<PipeWriter> writer_;

  base::WeakPtrFactory<ArchiveAgent> weak_factory_{this};
};

}  // namespace archive_agent

#endif  // CHROME_BROWSER_ARCHIVE_AGENT_ARCHIVE_AGENT_H_
