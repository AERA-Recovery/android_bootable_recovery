/* SPDX-License-Identifier: Apache-2.0 */
#include "fonts/fontsource.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>

static std::string response;
static bool delay = false;
static int exit_code = 0;
extern "C" int __wrap_execv(const char*, char* const* arguments) {
  assert(!strcmp(arguments[1], "wget"));
  if (delay) for (;;) usleep(10000);
  size_t written = 0;
  while (written < response.size()) {
    const auto count = write(STDOUT_FILENO, response.data() + written, response.size() - written);
    if (count <= 0) _exit(1);
    written += count;
  }
  _exit(exit_code);
}
int main() {
  using aeraui::fontsource::Download;
  std::atomic<bool> cancel{false};
  std::string bytes, error;
  response = "a complete response";
  assert(Download("https://api.fontsource.org/v1/fonts", 100, cancel, bytes, error));
  assert(bytes == response);
  response = std::string(100000, 'x');
  assert(!Download("https://api.fontsource.org/v1/fonts", 100, cancel, bytes, error));
  assert(bytes.empty());
  response = "partial response"; exit_code = 1;
  assert(!Download("https://api.fontsource.org/v1/fonts", 100, cancel, bytes, error));
  assert(!Download("https://example.com/fonts", 100, cancel, bytes, error));
  assert(!Download("https://api.fontsource.org/v1/fonts/../../x", 100, cancel, bytes, error));
  delay = true;
  const auto start = std::chrono::steady_clock::now();
  std::thread stopper([&] { usleep(100000); cancel.store(true); });
  assert(!Download("https://api.fontsource.org/v1/fonts", 100, cancel, bytes, error));
  stopper.join();
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  std::cout << "Fontsource transport: bounded reads, failed downloads, URL validation and cancellation passed\n";
}
