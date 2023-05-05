// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/command.h"

#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>
#include <string_view>
#include <system_error>  // NOLINT

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "absl/time/clock.h"
#include "./centipede/logging.h"
#include "./centipede/util.h"

namespace centipede {

// See the definition of --fork_server flag.
inline constexpr std::string_view kCommandLineSeparator(" \\\n");
inline constexpr std::string_view kNoForkServerRequestPrefix("%f");

// TODO(ussuri): Encapsulate as much of the fork server functionality from
//  this source as possible in this struct, and make it a class.
struct Command::ForkServerProps {
  // The file paths of the comms pipes.
  std::string fifo_path_[2];
  // The file descriptors of the comms pipes.
  int pipe_[2] = {-1, -1};
  // The file path to write the PID of the fork server process to.
  std::string pid_file_path_;
  // The PID of the fork server process. Used to verify that the fork server is
  // running and the pipes are ready for comms.
  pid_t pid_ = -1;
  // A `stat` of the fork server's binary right after it's started. Used to
  // detect that the running process with `pid_` is still the original fork
  // server, not a PID recycled by the OS.
  struct stat exe_stat_ = {};

  ~ForkServerProps() {
    for (int i = 0; i < 2; ++i) {
      if (pipe_[i] >= 0 && close(pipe_[i]) != 0) {
        LOG(ERROR) << "Failed to close fork server pipe for " << fifo_path_[i];
      }
      std::error_code ec;
      if (!fifo_path_[i].empty() &&
          !std::filesystem::remove(fifo_path_[i], ec)) {
        LOG(ERROR) << "Failed to remove fork server pipe file " << fifo_path_[i]
                   << ": " << ec;
      }
    }
  }
};

// NOTE: Because std::unique_ptr<T> requires T to be a complete type wherever
// the deleter is instantiated, the special member functions must be defined
// out-of-line here, now that ForkServerProps is complete (that's by-the-book
// PIMPL).
Command::Command(Command &&other) noexcept = default;
Command::~Command() = default;

Command::Command(std::string_view path, std::vector<std::string> args,
                 std::vector<std::string> env, std::string_view out,
                 std::string_view err, absl::Duration timeout,
                 std::string_view temp_file_path)
    : path_(path),
      args_(std::move(args)),
      env_(std::move(env)),
      out_(out),
      err_(err),
      timeout_(timeout),
      temp_file_path_(temp_file_path) {}

std::string Command::ToString() const {
  std::vector<std::string> ss;
  // env.
  ss.reserve(env_.size());
  for (const auto &env : env_) {
    ss.emplace_back(env);
  }
  // path.
  std::string path = path_;
  // Strip the % prefixes, if any.
  if (absl::StartsWith(path, kNoForkServerRequestPrefix)) {
    path = path.substr(kNoForkServerRequestPrefix.size());
  }
  // Replace @@ with temp_file_path_.
  constexpr std::string_view kTempFileWildCard = "@@";
  if (absl::StrContains(path, kTempFileWildCard)) {
    CHECK(!temp_file_path_.empty());
    path = absl::StrReplaceAll(path, {{kTempFileWildCard, temp_file_path_}});
  }
  ss.emplace_back(path);
  // args.
  for (const auto &arg : args_) {
    ss.emplace_back(arg);
  }
  // out/err.
  if (!out_.empty()) {
    ss.emplace_back(absl::StrCat("> ", out_));
  }
  if (!err_.empty()) {
    if (out_ != err_) {
      ss.emplace_back(absl::StrCat("2> ", err_));
    } else {
      ss.emplace_back("2>&1");
    }
  }
  // Trim trailing space and return.
  return absl::StrJoin(ss, kCommandLineSeparator);
}

bool Command::StartForkServer(std::string_view temp_dir_path,
                              std::string_view prefix) {
  if (absl::StartsWith(path_, kNoForkServerRequestPrefix)) {
    LOG(INFO) << "Fork server disabled for " << path();
    return false;
  }
  LOG(INFO) << "Starting fork server for " << path();

  fork_server_.reset(new ForkServerProps);
  fork_server_->fifo_path_[0] = std::filesystem::path(temp_dir_path)
                                    .append(absl::StrCat(prefix, "_FIFO0"));
  fork_server_->fifo_path_[1] = std::filesystem::path(temp_dir_path)
                                    .append(absl::StrCat(prefix, "_FIFO1"));
  const std::string pid_file_path =
      std::filesystem::path(temp_dir_path).append("pid");
  (void)std::filesystem::create_directory(temp_dir_path);  // it may not exist.
  for (int i = 0; i < 2; ++i) {
    PCHECK(mkfifo(fork_server_->fifo_path_[i].c_str(), 0600) == 0)
        << VV(i) << VV(fork_server_->fifo_path_[i]);
  }

  // NOTE: A background process does not return its exit status to the subshell,
  // so failures will never propagate to the caller of `system()`. Instead, we
  // save out the background process's PID to a file and use it later to assert
  // that the process has started and is still running.
  static constexpr std::string_view kForkServerCommandStub = R"sh(
  {
    CENTIPEDE_FORK_SERVER_FIFO0="%s" \
    CENTIPEDE_FORK_SERVER_FIFO1="%s" \
    %s
  } &
  echo -n $! > "%s"
)sh";
  const std::string fork_server_command = absl::StrFormat(
      kForkServerCommandStub, fork_server_->fifo_path_[0],
      fork_server_->fifo_path_[1], command_line_, pid_file_path);
  LOG(INFO) << "Fork server command:" << fork_server_command;

  const int exit_code = system(fork_server_command.c_str());

  // Check if `system()` was able to parse and run the command at all.
  if (exit_code != EXIT_SUCCESS) {
    LOG(ERROR) << "Failed to parse or run command to launch fork server; will "
                  "proceed without it";
    return false;
  }

  // The fork server is probably running now. However, one failure scenario is
  // that it starts and exits early. Try opening the read/write comms pipes with
  // it: if that fails, something is wrong.
  fork_server_->pipe_[0] = open(fork_server_->fifo_path_[0].c_str(), O_WRONLY);
  fork_server_->pipe_[1] = open(fork_server_->fifo_path_[1].c_str(), O_RDONLY);
  if (fork_server_->pipe_[0] < 0 || fork_server_->pipe_[1] < 0) {
    LOG(INFO) << "Failed to establish communication with fork server; will "
                 "proceed without it";
    return false;
  }

  // The fork server has started and the comms pipes got opened successfully.
  // Read the fork server's PID and the initial /proc/<PID>/exe symlink pointing
  // at the fork server's binary, written to the provided files by `command`.
  // `Execute()` uses these to monitor the fork server health.
  std::string pid_str;
  ReadFromLocalFile(pid_file_path, pid_str);
  CHECK(absl::SimpleAtoi(pid_str, &fork_server_->pid_)) << VV(pid_str);
  std::string proc_exe = absl::StrFormat("/proc/%d/exe", fork_server_->pid_);
  if (stat(proc_exe.c_str(), &fork_server_->exe_stat_) != EXIT_SUCCESS) {
    PLOG(ERROR) << "Fork server appears not running; will proceed without it: "
                << VV(proc_exe);
    return false;
  }

  return true;
}

absl::Status Command::VerifyForkServerIsHealthy() {
  // Preconditions: the callers (`Execute()`) should call us only when the fork
  // server is presumed to be running (`fork_server_pid_` >= 0). If it is, the
  // comms pipes are guaranteed to be opened by `StartForkServer()`.
  CHECK(fork_server_ != nullptr) << "Fork server wasn't started";
  CHECK(fork_server_->pid_ >= 0) << "Fork server process failed to start";
  CHECK(fork_server_->pipe_[0] >= 0 && fork_server_->pipe_[1] >= 0)
      << "Failed to connect to fork server";

  // A process with the fork server PID exists (_some_ process, possibly with a
  // recycled PID)...
  if (kill(fork_server_->pid_, 0) != EXIT_SUCCESS) {
    return absl::UnknownError(absl::StrCat(
        "Can't communicate with fork server, PID=", fork_server_->pid_));
  }
  // ...and it is a process with our expected binary, so it's practically
  // guaranteed to be our original fork server process.
  const std::string proc_exe =
      absl::StrFormat("/proc/%d/exe", fork_server_->pid_);
  struct stat proc_exe_stat = {};
  if (stat(proc_exe.c_str(), &proc_exe_stat) != EXIT_SUCCESS) {
    return absl::UnknownError(absl::StrCat(
        "Failed to stat fork server's /proc/<PID>/exe symlink, PID=",
        fork_server_->pid_));
  }
  if (proc_exe_stat.st_dev != fork_server_->exe_stat_.st_dev ||
      proc_exe_stat.st_ino != fork_server_->exe_stat_.st_ino) {
    return absl::UnknownError(absl::StrCat(
        "Fork server's /proc/<PID>/exe symlink changed (new process?), PID=",
        fork_server_->pid_));
  }
  return absl::OkStatus();
}

int Command::Execute() {
  int exit_code = 0;

  if (fork_server_ != nullptr) {
    if (const auto status = VerifyForkServerIsHealthy(); !status.ok()) {
      LOG(ERROR) << "Fork server should be running, but isn't: " << status;
      return EXIT_FAILURE;
    }

    // Wake up the fork server.
    char x = ' ';
    CHECK_EQ(1, write(fork_server_->pipe_[0], &x, 1));

    // The fork server forks, the child is running. Block until some readable
    // data appears in the pipe (that is, after the fork server writes the
    // execution result to it).
    struct pollfd poll_fd = {};
    int poll_ret = -1;
    auto poll_deadline = absl::Now() + timeout_;
    // The `poll()` syscall can get interrupted: it sets errno==EINTR in that
    // case. We should tolerate that.
    do {
      // NOTE: `poll_fd` has to be reset every time.
      poll_fd = {
          .fd = fork_server_->pipe_[1],  // The file descriptor to wait for.
          .events = POLLIN,              // Wait until `fd` gets readable data.
      };
      const int poll_timeout_ms = static_cast<int>(absl::ToInt64Milliseconds(
          std::max(poll_deadline - absl::Now(), absl::Milliseconds(1))));
      poll_ret = poll(&poll_fd, 1, poll_timeout_ms);
    } while (poll_ret < 0 && errno == EINTR);

    if (poll_ret != 1 || (poll_fd.revents & POLLIN) == 0) {
      // The fork server errored out or timed out, or some other error occurred,
      // e.g. the syscall was interrupted.
      std::string fork_server_log = "<not dumped>";
      if (!out_.empty()) {
        ReadFromLocalFile(out_, fork_server_log);
      }
      if (poll_ret == 0) {
        LOG(FATAL) << "Timeout while waiting for fork server: " << VV(timeout_)
                   << VV(fork_server_log) << VV(command_line_);
      } else {
        PLOG(FATAL) << "Error or interrupt while waiting for fork server: "
                    << VV(poll_ret) << VV(poll_fd.revents)
                    << VV(fork_server_log) << VV(command_line_);
      }
      __builtin_unreachable();
    }

    // The fork server wrote the execution result to the pipe: read it.
    CHECK_EQ(sizeof(exit_code),
             read(fork_server_->pipe_[1], &exit_code, sizeof(exit_code)));
  } else {
    // No fork server, use system().
    exit_code = system(command_line_.c_str());
  }
  if (WIFSIGNALED(exit_code) && (WTERMSIG(exit_code) == SIGINT))
    RequestEarlyExit(EXIT_FAILURE);
  if (WIFEXITED(exit_code)) return WEXITSTATUS(exit_code);
  return exit_code;
}

}  // namespace centipede
