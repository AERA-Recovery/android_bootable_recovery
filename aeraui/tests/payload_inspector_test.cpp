/* SPDX-License-Identifier: Apache-2.0 */
#include "features/update/payload_inspector.hpp"
#include "update_engine/update_metadata.pb.h"
#include "ota_metadata.pb.h"
#include <cstdlib>
#include <android-base/file.h>
#include <gtest/gtest.h>
#include <ziparchive/zip_writer.h>
#include <cstdio>
#include <limits>

using namespace aeraui::payload;
using chromeos_update_engine::DeltaArchiveManifest;
namespace {
std::string Payload(const DeltaArchiveManifest &manifest) {
  const auto proto = manifest.SerializeAsString();
  std::string header = "CrAU";
  for (int shift = 56; shift >= 0; shift -= 8)
    header += static_cast<char>(uint64_t{2} >> shift);
  for (int shift = 56; shift >= 0; shift -= 8)
    header += static_cast<char>(static_cast<uint64_t>(proto.size()) >> shift);
  header.append(4, '\0');
  return header + proto;
}
DeltaArchiveManifest Manifest() {
  DeltaArchiveManifest manifest;
  auto *partition = manifest.add_partitions();
  partition->set_partition_name("system");
  partition->mutable_new_partition_info()->set_size(4096);
  partition->add_operations()->set_type(chromeos_update_engine::InstallOperation::ZERO);
  manifest.set_security_patch_level("2026-10-01");
  return manifest;
}
Info Inspect(const std::string &payload, bool compressed = false,
             const char *entry_name = "payload.bin", bool oem = false) {
  TemporaryFile file;
  FILE *stream = fdopen(dup(file.fd), "wb");
  EXPECT_NE(stream, nullptr);
  {
    ZipWriter zip(stream);
    EXPECT_EQ(zip.StartEntry(entry_name, compressed ? ZipWriter::kCompress : 0), 0);
    EXPECT_EQ(zip.WriteBytes(payload.data(), payload.size()), 0);
    EXPECT_EQ(zip.FinishEntry(), 0);
    std::string metadata = "pre-device=lighthouse\npost-build-incremental=TEST123\npost-sdk-level=36\n";
    if (oem) metadata += "product_name=CPH2653\nversion_name=CPH2653_16.0.2.402(EX01)\n";
    EXPECT_EQ(zip.StartEntry("META-INF/com/android/metadata", ZipWriter::kCompress), 0);
    EXPECT_EQ(zip.WriteBytes(metadata.data(), metadata.size()), 0);
    EXPECT_EQ(zip.FinishEntry(), 0);
    EXPECT_EQ(zip.Finish(), 0);
  }
  fclose(stream);
  return InspectZip(file.path);
}
TEST(PayloadReview, FullAndIncremental) {
  auto manifest = Manifest();
  auto info = Inspect(Payload(manifest));
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_FALSE(info.incremental);
  EXPECT_EQ(info.expanded_bytes, 4096u);
  EXPECT_EQ(info.target_device, "lighthouse");
  EXPECT_EQ(info.build_id, "TEST123");
  EXPECT_TRUE(info.name_from_filename);
  EXPECT_EQ(info.security_patch, "2026-10-01");
  EXPECT_EQ(info.target_sdk, "Android 16");
  manifest.mutable_partitions(0)->mutable_old_partition_info()->set_size(4096);
  EXPECT_TRUE(Inspect(Payload(manifest)).incremental);
}
TEST(PayloadReview, NormalInstaller) {
  auto info = Inspect("#!/sbin/sh\n", false, "META-INF/com/google/android/update-binary");
  EXPECT_FALSE(info.is_payload);
  EXPECT_TRUE(info.error.empty());
}
TEST(PayloadReview, OemDisplayFieldsAndFullRevision) {
  auto manifest = Manifest();
  manifest.set_minor_version(6);
  auto info = Inspect(Payload(manifest), false, "payload.bin", true);
  ASSERT_TRUE(info.valid);
  EXPECT_EQ(info.target_device, "CPH2653");
  EXPECT_EQ(info.target_build, "CPH2653_16.0.2.402(EX01)");
  EXPECT_EQ(info.build_id, "TEST123");
  EXPECT_FALSE(info.incremental);
  manifest.mutable_partitions(0)->mutable_operations(0)->set_type(chromeos_update_engine::InstallOperation::SOURCE_COPY);
  EXPECT_TRUE(Inspect(Payload(manifest)).incremental);
}
TEST(PayloadReview, CustomRomFileWhenProvided) {
  const char *path = std::getenv("AERA_TEST_CUSTOM_OTA");
  if (!path) GTEST_SKIP() << "No local custom OTA fixture provided";
  const auto info = InspectZip(path);
  ASSERT_TRUE(info.valid) << info.error;
  EXPECT_TRUE(info.name_from_filename);
  EXPECT_EQ(info.target_device, "lighthouse");
  EXPECT_EQ(info.target_sdk, "Android 17");
  EXPECT_EQ(info.target_build, "Project_Infinity-X-4.0-BETA-lighthouse-08.10.2026-GAPPS-UNOFFICIAL");
  EXPECT_EQ(info.build_id, "1791456637");
}
TEST(PayloadReview, RejectBrokenHeaderAndCompression) {
  EXPECT_FALSE(Inspect("CrAU").valid);
  auto data = Payload(Manifest());
  data[12] = 127;
  EXPECT_FALSE(Inspect(data).valid);
  EXPECT_FALSE(Inspect(Payload(Manifest()), true).valid);
}
TEST(PayloadReview, RejectInvalidNamesAndSizeOverflow) {
  auto manifest = Manifest();
  manifest.mutable_partitions(0)->set_partition_name("../system");
  EXPECT_FALSE(Inspect(Payload(manifest)).valid);
  manifest = Manifest();
  auto *other = manifest.add_partitions();
  other->set_partition_name("vendor");
  other->mutable_new_partition_info()->set_size(std::numeric_limits<uint64_t>::max());
  EXPECT_FALSE(Inspect(Payload(manifest)).valid);
}
TEST(PayloadReview, SnapshotAndPartialMetadata) {
  auto manifest = Manifest();
  manifest.set_partial_update(true);
  auto *dynamic = manifest.mutable_dynamic_partition_metadata();
  dynamic->add_groups()->set_name("main");
  dynamic->set_snapshot_enabled(true);
  dynamic->set_vabc_enabled(true);
  const auto info = Inspect(Payload(manifest));
  ASSERT_TRUE(info.valid);
  EXPECT_TRUE(info.partial);
  EXPECT_TRUE(info.dynamic_partitions);
  EXPECT_TRUE(info.snapshots);
  EXPECT_TRUE(info.virtual_ab_compression);
}
}
