/* SPDX-License-Identifier: Apache-2.0 */
#include "features/update/payload_inspector.hpp"
#include "features/update/payload_arb.hpp"
#include <openssl/sha.h>
#include <bzlib.h>
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
void Put(std::vector<uint8_t> &v, size_t offset, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) v.at(offset + i) = value >> (8 * i);
}
std::vector<uint8_t> Firmware(uint32_t index) {
  std::vector<uint8_t> v(4096);
  memcpy(v.data(), "\177ELF", 4); v[4] = 2; v[5] = 1;
  Put(v, 32, 64, 8); Put(v, 54, 56, 2); Put(v, 56, 1, 2);
  Put(v, 72, 256, 8); Put(v, 96, 128, 8);
  Put(v, 256, 7, 4); Put(v, 268, 12, 4); Put(v, 272, 32, 4);
  Put(v, 292, 3, 4); Put(v, 300, index, 4);
  return v;
}
std::string Digest(const std::vector<uint8_t> &v) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(v.data(), v.size(), hash);
  return std::string(reinterpret_cast<char *>(hash), sizeof(hash));
}
TEST(PayloadArb, ZeroAndMalformedFirmware) {
  auto v = Firmware(0);
  ASSERT_TRUE(ReadFirmwareArb(v).available);
  EXPECT_EQ(ReadFirmwareArb(v).index, 0u);
  v = Firmware(5); EXPECT_EQ(ReadFirmwareArb(v).index, 5u);
  v[5] = 2; EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); Put(v, 32, UINT64_MAX, 8);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); Put(v, 268, 8, 4);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
  v = Firmware(5); v.resize(300);
  EXPECT_FALSE(ReadFirmwareArb(v).available);
}
TEST(PayloadArb, BothSlotsMustBeCheckedBeforeInstall) {
  using D = ArbDecision;
  const Arb zero{true, 0, ""}, one{true, 1, ""}, two{true, 2, ""}, unknown{};
  EXPECT_EQ(CompareArb(true, one, {zero, zero}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, zero, {one, one}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {one, one}), D::Same);
  EXPECT_EQ(CompareArb(true, zero, {zero, zero}), D::Same);
  EXPECT_EQ(CompareArb(true, one, {zero, two}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {two, zero}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {zero, one}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, one, {one, zero}), D::Upgrade);
  EXPECT_EQ(CompareArb(true, one, {unknown, two}), D::Downgrade);
  EXPECT_EQ(CompareArb(true, one, {zero, unknown}), D::Unknown);
  EXPECT_EQ(CompareArb(true, unknown, {zero, zero}), D::Unknown);
  EXPECT_EQ(CompareArb(false, unknown, {unknown, unknown}), D::NotApplicable);
  EXPECT_FALSE(ArbAllowsInstall(D::Upgrade, false));
  EXPECT_TRUE(ArbAllowsInstall(D::Upgrade, true));
  for (bool acknowledged : {false, true}) {
    EXPECT_FALSE(ArbAllowsInstall(D::Downgrade, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::Unknown, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::Same, acknowledged));
    EXPECT_TRUE(ArbAllowsInstall(D::NotApplicable, acknowledged));
  }
}
TEST(PayloadArb, RawBzipAndIntegrity) {
  for (bool compressed : {false, true}) {
    auto image = Firmware(7), data = image;
    if (compressed) {
      data.resize(8192); unsigned length = data.size();
      ASSERT_EQ(BZ2_bzBuffToBuffCompress(reinterpret_cast<char *>(data.data()), &length,
          reinterpret_cast<char *>(image.data()), image.size(), 9, 0, 30), BZ_OK);
      data.resize(length);
    }
    TemporaryFile file;
    ASSERT_EQ(write(file.fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
    DeltaArchiveManifest manifest;
    manifest.set_block_size(4096);
    auto *p = manifest.add_partitions(); p->set_partition_name("xbl_config");
    p->mutable_new_partition_info()->set_size(image.size());
    p->mutable_new_partition_info()->set_hash(Digest(image));
    auto *op = p->add_operations();
    op->set_type(compressed ? chromeos_update_engine::InstallOperation::REPLACE_BZ :
                             chromeos_update_engine::InstallOperation::REPLACE);
    op->set_data_length(data.size()); op->set_data_sha256_hash(Digest(data));
    op->add_dst_extents()->set_num_blocks(1);
    const auto result = InspectArb(file.fd, 0, data.size(), manifest);
    ASSERT_TRUE(result.available) << result.detail;
    EXPECT_EQ(result.index, 7u);
    p->mutable_new_partition_info()->set_hash(std::string(32, 'x'));
    EXPECT_FALSE(InspectArb(file.fd, 0, data.size(), manifest).available);
    op->set_type(chromeos_update_engine::InstallOperation::SOURCE_COPY);
    EXPECT_FALSE(InspectArb(file.fd, 0, data.size(), manifest).available);
  }
}
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
  const auto arb = InspectZip(path, true);
  ASSERT_TRUE(arb.arb_available) << arb.arb_detail;
  EXPECT_EQ(arb.arb_index, 0u);
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
