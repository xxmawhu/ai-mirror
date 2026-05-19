#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace ai_mirror::daemon {

struct HealthStatus {
  std::string mount_point;
  bool healthy;
  bool stale = false; // mount not in current config, should be cleaned
  std::string detail;
};

class HealthCheck {
public:
  explicit HealthCheck(const std::string &user_prefix = "i");
  std::vector<HealthStatus> check_all();
  HealthStatus check_mount(const std::string &mount_point);
  int run_periodic(int interval_seconds);
  void stop();

private:
  std::string prefix_;
  std::atomic<bool> running_{true};
  std::condition_variable cv_;
  std::mutex mtx_;
  static std::atomic<HealthCheck *> instance_;
  static std::atomic<bool> signal_received_;
  static void signal_handler(int sig);
};

} // namespace ai_mirror::daemon
