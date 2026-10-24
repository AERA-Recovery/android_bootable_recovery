/* SPDX-License-Identifier: Apache-2.0 */
#include "../features/root/module_assets.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

using namespace aeraui::root;
int main() {
  for (const char *kmi : {"android12-5.10", "android13-5.10", "android13-5.15",
                          "android14-5.15", "android14-6.1", "android15-6.6",
                          "android16-6.12", "android17-6.18"}) {
    const std::string key = kmi;
    const auto names = ModuleAssetNames(Provider::kKernelSUNext, key);
    assert(names == std::vector<std::string>({"aarch64_" + key + "_kernelsu.ko",
                                            "aarch64-" + key + "_kernelsu.ko",
                                            key + "_kernelsu.ko"}));
    for (const auto &wrong : {"x86_64_" + key + "_kernelsu.ko",
                              "aarch64_" + key + "_kernelsu.ko.sig",
                              std::string("aarch64_android99-9.99_kernelsu.ko")})
      assert(std::find(names.begin(), names.end(), wrong) == names.end());
    assert(ModuleAssetNames(Provider::kKernelSU, key) ==
           std::vector<std::string>({"lkm-aarch64-" + key + "_kernelsu.ko"}));
    assert(ModuleAssetNames(Provider::kSukiSU, key) ==
           std::vector<std::string>({"lkm-aarch64-" + key + "_kernelsu.ko",
                                    "aarch64-" + key + "-lkm.zip"}));
    for (const auto provider : {Provider::kKernelSU, Provider::kKernelSUNext, Provider::kSukiSU}) {
      const auto accepted = ModuleAssetNames(provider, key);
      for (const auto &name : accepted) {
        const bool legacy_archive = provider == Provider::kSukiSU && name == "aarch64-" + key + "-lkm.zip";
        assert(ModuleAssetArchive(provider, name) == legacy_archive);
      }
    }
    const auto sukisu = ModuleAssetNames(Provider::kSukiSU, key);
    for (const auto &wrong : {"lkm-x86_64-" + key + "_kernelsu.ko",
                             "lkm-aarch64-" + key + "_kernelsu.ko.sig",
                             std::string("lkm-aarch64-android99-9.99_kernelsu.ko")})
      assert(std::find(sukisu.begin(), sukisu.end(), wrong) == sukisu.end());
  }
  assert(!ModuleAssetArchive(Provider::kKernelSU, "aarch64-android16-6.12-lkm.zip"));
  assert(!ModuleAssetArchive(Provider::kSukiSU, "module.zip.sig"));
  puts("KernelSU/Next names, SukiSU direct/legacy assets, exact KMI and archive handling passed");
}
