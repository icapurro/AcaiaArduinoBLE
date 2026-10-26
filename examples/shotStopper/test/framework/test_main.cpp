// Test runner: forks one child process per test with a time limit.
//
//   ./build/sketch_tests            run every test, print a summary
//   ./build/sketch_tests NAME       run only NAME, in-process (for a debugger)
//   ./build/sketch_tests --list     list test names
//
// Environment:
//   TEST_TIMEOUT=<seconds>   per-test limit (default 10)
//   TEST_VERBOSE=1           show output of passing tests too (and echo Serial)
#include "test.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace {

std::string readAll(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  return out;
}

int runInProcess(const testfw::TestCase& test) {
  test.fn();
  std::fflush(stdout);
  std::fflush(stderr);
  return testfw::failureCount() ? 1 : 0;
}

// Returns an empty string on pass, otherwise the failure reason.
std::string runForked(const testfw::TestCase& test, unsigned timeoutS, std::string& output) {
  int pipeFds[2];
  if (pipe(pipeFds) != 0) return "pipe() failed";
  std::fflush(stdout);
  std::fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) return "fork() failed";
  if (pid == 0) {
    close(pipeFds[0]);
    dup2(pipeFds[1], STDOUT_FILENO);
    dup2(pipeFds[1], STDERR_FILENO);
    close(pipeFds[1]);
    alarm(timeoutS);
    std::_Exit(runInProcess(test));
  }
  close(pipeFds[1]);
  output = readAll(pipeFds[0]);
  close(pipeFds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    return code == 0 ? "" : "failed (exit code " + std::to_string(code) + ")";
  }
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    if (sig == SIGALRM) return "timed out after " + std::to_string(timeoutS) + " s";
    return std::string("crashed: ") + strsignal(sig);
  }
  return "unknown exit status";
}

}  // namespace

int main(int argc, char** argv) {
  auto& tests = testfw::registry();

  if (argc > 1 && std::string(argv[1]) == "--list") {
    for (auto& t : tests) std::printf("%s\n", t.name);
    return 0;
  }

  if (argc > 1) {
    for (auto& t : tests) {
      if (std::string(t.name) == argv[1]) {
        int rc = runInProcess(t);
        std::printf("%s %s\n", rc ? "FAIL" : "PASS", t.name);
        return rc;
      }
    }
    std::fprintf(stderr, "no test named '%s' (use --list)\n", argv[1]);
    return 2;
  }

  const char* timeoutEnv = std::getenv("TEST_TIMEOUT");
  unsigned timeoutS = timeoutEnv ? static_cast<unsigned>(std::atoi(timeoutEnv)) : 10;
  bool verbose = std::getenv("TEST_VERBOSE") != nullptr;

  int passed = 0;
  std::vector<std::string> failed;
  for (auto& t : tests) {
    std::string output;
    std::string reason = runForked(t, timeoutS, output);
    if (reason.empty()) {
      passed++;
      std::printf("  ok    %s\n", t.name);
      if (verbose && !output.empty()) std::printf("%s", output.c_str());
    } else {
      failed.push_back(t.name);
      std::printf("  FAIL  %s (%s:%d): %s\n", t.name, t.file, t.line, reason.c_str());
      std::printf("%s", output.c_str());
    }
  }

  std::printf("\n%s: %d passed, %zu failed\n", argv[0], passed, failed.size());
  for (auto& name : failed) std::printf("  failed: %s\n", name.c_str());
  return failed.empty() ? 0 : 1;
}
