// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../aera_efisp.hpp"
#include <aeraui/backend.hpp>
#include <cassert>
#include <vector>

static void Put16(std::vector<unsigned char>& b, size_t p, uint16_t v) {
  b[p] = v; b[p + 1] = v >> 8;
}
static void Put32(std::vector<unsigned char>& b, size_t p, uint32_t v) {
  Put16(b, p, v); Put16(b, p + 2, v >> 16);
}
int main(int argc, char** argv) {
#if defined(AERA_ENABLE_EFISP) && AERA_ENABLE_EFISP == 1
  assert(aera_efisp::Enabled());
#else
  assert(!aera_efisp::Enabled());
#endif
  using namespace aeraui;
  JobRequest request;
  request.job = Job::kWipe;
  request.partitions = {"/data"};
  assert(EfispWipeAuthorized(request));
  request.partitions.push_back("/efisp");
  assert(!EfispWipeAuthorized(request));
  request.confirmation = "yes";
  assert(!EfispWipeAuthorized(request));
  request.confirmation = "wipe-efisp";
  assert(EfispWipeAuthorized(request) == aera_efisp::Enabled());
  request.partitions = {"/efisp/subpath"};
  assert(!EfispWipeAuthorized(request));
  assert(!FormatDataAuthorized(request));
  // Invalid acknowledgement returns before discovering/opening any device.
  assert(!aera_efisp::Wipe(""));
  assert(!aera_efisp::Wipe("yes"));
  assert(!aera_efisp::Discover(nullptr));
  assert(!aera_efisp::ValidImage("/dev/null", 4096));
  char first[] = "/tmp/aera-efi-source-XXXXXX", second[] = "/tmp/aera-efi-target-XXXXXX";
  const int source = mkstemp(first), target = mkstemp(second);
  assert(source >= 0 && target >= 0);
  // Construct a minimal ARM64 PE32+ EFI application with one bounded section.
  std::vector<unsigned char> image(131089, 0);
  image[0] = 'M'; image[1] = 'Z'; Put32(image, 60, 64);
  image[64] = 'P'; image[65] = 'E'; Put16(image, 68, 0xaa64);
  Put16(image, 70, 1); Put16(image, 84, 112);
  Put16(image, 88, 0x20b); Put16(image, 156, 10);
  Put32(image, 216, image.size() - 240); Put32(image, 220, 240);
  assert(write(source, image.data(), image.size()) == ssize_t(image.size()));
  assert(aera_efisp::ValidImage(source, image.size()));
  assert(!aera_efisp::ValidImage(source, image.size() - 1));
  assert(write(target, image.data(), image.size()) == ssize_t(image.size()));
  assert(aera_efisp::CompareWritten(source, target, image.size()));
  unsigned char bad = 0xff;
  assert(pwrite(target, &bad, 1, image.size() - 1) == 1);
  assert(!aera_efisp::CompareWritten(source, target, image.size()));
  assert(!aera_efisp::CompareWritten(source, target, image.size() + 1));
  // The flash primitive replaces exactly the image range and preserves the
  // destination tail, including a partial final chunk. Only regular files here.
  const std::vector<unsigned char> tail(23, 0xa5);
  assert(pwrite(target, tail.data(), tail.size(), image.size()) == ssize_t(tail.size()));
  assert(aera_efisp::WriteAndVerify(source, target, image.size()));
  std::vector<unsigned char> remaining(tail.size());
  assert(pread(target, remaining.data(), remaining.size(), image.size()) == ssize_t(remaining.size()));
  assert(remaining == tail);
  assert(!aera_efisp::WriteAndVerify(source, -1, image.size()));
  assert(!aera_efisp::WriteAndVerify(source, target, 0));
  const int readonly = open(second, O_RDONLY);
  assert(readonly >= 0 && !aera_efisp::WriteAndVerify(source, readonly, image.size()));
  close(readonly);
  // Reject Windows binaries, wrong architecture, malformed/truncated offsets,
  // overflowing section data and invalid optional headers.
  for (const auto& change : std::vector<std::pair<size_t, unsigned char>>{
       {0, 0}, {64, 0}, {69, 0x86}, {88, 0}, {156, 3}, {63, 0xff},
       {71, 0xff}, {218, 0xff}}) {
    assert(pwrite(source, &change.second, 1, change.first) == 1);
    assert(!aera_efisp::ValidImage(source, image.size()));
    assert(pwrite(source, &image[change.first], 1, change.first) == 1);
  }
  assert(ftruncate(source, 100) == 0);
  assert(!aera_efisp::ValidImage(source, image.size()));
  close(source); close(target); unlink(first); unlink(second);
  // Optional inspection of a user-provided sample is strictly read-only.
  if (argc == 2) assert(aera_efisp::ValidImage(argv[1], 3145728));
}
