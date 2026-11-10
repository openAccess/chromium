// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/archive_agent/archive_agent.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_command_line.h"
#include "base/scoped_environment_variable_override.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace archive_agent {
namespace {

class RecordingDelegate : public ArchiveAgent::Delegate {
 public:
  void OnAgentText(const std::string& text) override { text_ += text; }
  void OnAgentToolCall(const std::string& tool,
                       const std::string& arguments) override {
    tool_calls_.push_back(tool + " " + arguments);
  }
  void OnAgentIdle() override {
    ++idles_;
    if (on_idle_) {
      std::move(on_idle_).Run();
    }
  }
  void OnAgentError(const std::string& message) override {
    errors_.push_back(message);
    if (on_error_) {
      std::move(on_error_).Run();
    }
  }
  std::string AnswerQuery(const std::string& tool,
                          const base::DictValue& params) override {
    queries_.push_back(tool);
    return answer_;
  }

  std::string text_;
  std::vector<std::string> tool_calls_;
  std::vector<std::string> errors_;
  std::vector<std::string> queries_;
  std::string answer_ = "{\"mode\":\"replaying\"}";
  int idles_ = 0;
  base::OnceClosure on_idle_;
  base::OnceClosure on_error_;
};

class ArchiveAgentTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  // Writes a stand-in for Pi that runs `script`, and makes it the command the
  // agent runs.
  void UseFakePi(const std::string& script) {
    const base::FilePath pi = temp_dir_.GetPath().Append("fake-pi");
    ASSERT_TRUE(base::WriteFile(pi, "#!/bin/sh\n" + script));
    ASSERT_TRUE(base::SetPosixFilePermissions(pi, 0700));
    command_line_.GetProcessCommandLine()->AppendSwitchPath(kCommandSwitch,
                                                            pi);
  }

  base::FilePath Path(const std::string& name) {
    return temp_dir_.GetPath().Append(name);
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedCommandLine command_line_;
  base::ScopedTempDir temp_dir_;
};

#if BUILDFLAG(IS_POSIX)

// The whole round: a question goes in on Pi's stdin, a tool's query comes out
// as a dialog, the browser's answer goes back as the dialog's value, and the
// answer streams out as text.
TEST_F(ArchiveAgentTest, AToolsQueryIsAnsweredThroughTheDialogChannel) {
  UseFakePi(
      "read -r prompt\n"
      "printf '%s\\n' \"$prompt\" > '" + Path("prompt.json").value() + "'\n"
      "echo '{\"type\":\"tool_execution_start\",\"toolName\":\"tab_status\","
      "\"args\":{}}'\n"
      "echo '{\"type\":\"extension_ui_request\",\"id\":\"q1\","
      "\"method\":\"editor\",\"title\":\"chromium-archive-query\","
      "\"prefill\":\"{\\\"tool\\\":\\\"tab_status\\\",\\\"params\\\":{}}\"}'\n"
      "read -r answer\n"
      "printf '%s\\n' \"$answer\" > '" + Path("answer.json").value() + "'\n"
      "echo '{\"type\":\"message_update\",\"assistantMessageEvent\":"
      "{\"type\":\"text_delta\",\"delta\":\"It is \"}}'\n"
      "echo '{\"type\":\"message_update\",\"assistantMessageEvent\":"
      "{\"type\":\"text_delta\",\"delta\":\"replaying.\"}}'\n"
      "echo '{\"type\":\"agent_settled\"}'\n"
      "read -r nothing\n");

  RecordingDelegate delegate;
  ArchiveAgent agent(delegate);
  base::RunLoop idle;
  delegate.on_idle_ = idle.QuitClosure();
  agent.Ask("What is this tab doing?");
  EXPECT_TRUE(agent.busy());
  idle.Run();

  EXPECT_FALSE(agent.busy());
  EXPECT_EQ("It is replaying.", delegate.text_);
  EXPECT_EQ(std::vector<std::string>{"tab_status {}"}, delegate.tool_calls_);
  EXPECT_EQ(std::vector<std::string>{"tab_status"}, delegate.queries_);
  EXPECT_TRUE(delegate.errors_.empty());

  std::string prompt;
  ASSERT_TRUE(base::ReadFileToString(Path("prompt.json"), &prompt));
  EXPECT_NE(std::string::npos, prompt.find("\"type\":\"prompt\""));
  EXPECT_NE(std::string::npos,
            prompt.find("\"message\":\"What is this tab doing?\""));

  std::string answer;
  ASSERT_TRUE(base::ReadFileToString(Path("answer.json"), &answer));
  EXPECT_NE(std::string::npos,
            answer.find("\"type\":\"extension_ui_response\""));
  EXPECT_NE(std::string::npos, answer.find("\"id\":\"q1\""));
  EXPECT_NE(std::string::npos,
            answer.find("\"value\":\"{\\\"mode\\\":\\\"replaying\\\"}\""));
}

// A dialog that is not a tool's query is declined, even one that looks like
// a query in every other way: the tools ask nothing of the user, and nothing
// else should be able to get an answer out of the browser.
TEST_F(ArchiveAgentTest, AnyOtherDialogIsDeclined) {
  UseFakePi(
      "read -r prompt\n"
      "echo '{\"type\":\"extension_ui_request\",\"id\":\"d1\","
      "\"method\":\"confirm\",\"title\":\"chromium-archive-query\","
      "\"prefill\":\"{\\\"tool\\\":\\\"tab_status\\\"}\"}'\n"
      "read -r answer\n"
      "printf '%s\\n' \"$answer\" > '" + Path("answer.json").value() + "'\n"
      "echo '{\"type\":\"agent_settled\"}'\n"
      "read -r nothing\n");

  RecordingDelegate delegate;
  ArchiveAgent agent(delegate);
  base::RunLoop idle;
  delegate.on_idle_ = idle.QuitClosure();
  agent.Ask("Anything");
  idle.Run();

  EXPECT_TRUE(delegate.queries_.empty());
  std::string answer;
  ASSERT_TRUE(base::ReadFileToString(Path("answer.json"), &answer));
  EXPECT_NE(std::string::npos, answer.find("\"id\":\"d1\""));
  EXPECT_NE(std::string::npos, answer.find("\"cancelled\":true"));
}

// Pi going away is an error the user sees, with what it said before it went.
TEST_F(ArchiveAgentTest, AnAgentThatStopsSaysWhy) {
  UseFakePi("echo 'No API key found for anthropic' >&2\nexit 1\n");

  RecordingDelegate delegate;
  ArchiveAgent agent(delegate);
  base::RunLoop error;
  delegate.on_error_ = error.QuitClosure();
  agent.Ask("Anything");
  error.Run();

  ASSERT_EQ(1u, delegate.errors_.size());
  EXPECT_NE(std::string::npos,
            delegate.errors_[0].find("No API key found for anthropic"));
  EXPECT_FALSE(agent.busy());
}

// A directory left by a browser that went without deleting it -- killed, or
// ended with the session -- is deleted when an assistant next starts, but only
// once the process it was for is gone: another browser's may be running.
TEST_F(ArchiveAgentTest, ADirectoryLeftBehindIsDeletedOnceItsProcessIsGone) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::ScopedEnvironmentVariableOverride tmpdir("TMPDIR",
                                                 temp.GetPath().value());

  // One for a process long gone (no pid is this large), and one for this one.
  const base::FilePath gone = temp.GetPath().Append("x.archive-agent-gone");
  const base::FilePath live = temp.GetPath().Append("x.archive-agent-live");
  const base::FilePath other = temp.GetPath().Append("x.archive-agent-other");
  for (const base::FilePath& directory : {gone, live, other}) {
    ASSERT_TRUE(base::CreateDirectory(directory));
  }
  ASSERT_TRUE(base::WriteFile(gone.Append("archive-tools.ts"), ""));
  ASSERT_TRUE(base::WriteFile(gone.Append("pid"), "2147483646"));
  ASSERT_TRUE(base::WriteFile(live.Append("archive-tools.ts"), ""));
  ASSERT_TRUE(base::WriteFile(
      live.Append("pid"),
      base::NumberToString(base::Process::Current().Pid())));
  // Not an assistant's, whatever it is called.
  ASSERT_TRUE(base::WriteFile(other.Append("pid"), "2147483646"));

  UseFakePi("exit 0\n");
  RecordingDelegate delegate;
  ArchiveAgent agent(delegate);
  base::RunLoop error;
  delegate.on_error_ = error.QuitClosure();
  agent.Ask("Anything");
  error.Run();

  EXPECT_FALSE(base::PathExists(gone));
  EXPECT_TRUE(base::PathExists(live));
  EXPECT_TRUE(base::PathExists(other));
}

// A model that fails ends its message with an error, which is shown.
TEST_F(ArchiveAgentTest, AModelErrorIsShown) {
  RecordingDelegate delegate;
  ArchiveAgent agent(delegate);
  agent.OnLineForTesting(
      "{\"type\":\"message_end\",\"message\":{\"role\":\"assistant\","
      "\"stopReason\":\"error\",\"errorMessage\":\"overloaded\"}}");
  EXPECT_EQ(std::vector<std::string>{"overloaded"}, delegate.errors_);
}

#endif  // BUILDFLAG(IS_POSIX)

}  // namespace
}  // namespace archive_agent
