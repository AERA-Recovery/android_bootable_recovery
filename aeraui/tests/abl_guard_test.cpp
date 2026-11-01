// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../aera_abl.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
using namespace aera_efisp;
static void Put16(std::vector<unsigned char>& b, size_t i, uint16_t v) { b[i]=v; b[i+1]=v>>8; }
static void Put32(std::vector<unsigned char>& b, size_t i, uint32_t v) { Put16(b,i,v);Put16(b,i+2,v>>16); }
static const unsigned char no_loader[] = {93,0,0,128,0,255,255,255,255,255,255,255,255,0,38,150,124,27,140,187,24,27,140,54,206,218,186,63,45,116,221,87,160,98,62,95,170,97,212,177,43,237,18,22,109,75,122,87,152,178,110,74,133,102,255,255,207,146,0,0};
static const unsigned char with_loader[] = {93,0,0,128,0,255,255,255,255,255,255,255,255,0,38,150,124,27,140,187,24,27,140,54,206,218,186,63,45,116,221,87,160,98,62,95,170,97,212,177,43,237,18,22,109,75,122,87,120,54,205,60,40,78,134,149,131,159,216,171,0,247,21,2,6,4,255,255,5,98,32,0};

static std::vector<unsigned char> Container(bool present) {
  const auto* compressed = present ? with_loader : no_loader;
  const size_t bytes = present ? sizeof(with_loader) : sizeof(no_loader);
  std::vector<unsigned char> b(128+24+bytes,0);
  memcpy(b.data(), "\x7f" "ELF",4); b[4]=1; b[5]=1;
  Put32(b,28,52);Put16(b,42,32);Put16(b,44,1);
  Put32(b,52,1);Put32(b,56,128);Put32(b,68,24+bytes);
  Put32(b,128,24+bytes);b[131]=2;
  const unsigned char guid[]={0x98,0x58,0x4e,0xee,0x14,0x39,0x59,0x42,0x9d,0x6e,0xdc,0x7b,0xd7,0x94,0x03,0xcf};
  memcpy(b.data()+132,guid,16);Put16(b,148,24);Put16(b,150,1);
  memcpy(b.data()+152,compressed,bytes);
  return b;
}
int main(int argc,char** argv) {
  if(argc==2) {
    const auto begin=std::chrono::steady_clock::now();
    const auto first=ReadAbl(argv[1],"abl");
    const auto middle=std::chrono::steady_clock::now();
    const auto cached=ReadAbl(argv[1],"abl",{first});
    assert(first.digest==cached.digest && first.status==cached.status);
    printf("%s: %s; inspect %.3f ms, cached %.3f ms; sha256 %s\n",argv[1],AblStatusName(first.status),
       std::chrono::duration<double,std::milli>(middle-begin).count(),
       std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-middle).count(),first.digest.c_str());
    return first.status==AblStatus::Unknown;
  }
  assert(InspectAbl(Container(true))==AblStatus::EfispLoader);
  assert(InspectAbl(Container(false))==AblStatus::NoEfispLoader);
  assert(InspectAbl({})==AblStatus::Unknown);
  auto truncated=Container(true);truncated.pop_back();assert(InspectAbl(truncated)==AblStatus::Unknown);
  auto invalid=Container(true);Put32(invalid,56,0xfffffff0);assert(InspectAbl(invalid)==AblStatus::Unknown);
  invalid=Container(true);Put16(invalid,44,65535);assert(InspectAbl(invalid)==AblStatus::Unknown);
  invalid=Container(true);Put16(invalid,148,65535);assert(InspectAbl(invalid)==AblStatus::Unknown);
  invalid=Container(true);invalid[152]=0xff;assert(InspectAbl(invalid)==AblStatus::Unknown);
  invalid=Container(true);invalid[158]=0;assert(InspectAbl(invalid)==AblStatus::Unknown);
  // Partition padding is outside the ELF extent and must not affect cache keys.
  char name[]="/tmp/aera-abl-test-XXXXXX";const int fd=mkstemp(name);assert(fd>=0);
  auto image=Container(true);assert(write(fd,image.data(),image.size())==ssize_t(image.size()));
  const auto first=ReadAbl(name,"abl_a");assert(first.status==AblStatus::EfispLoader);
  const std::vector<unsigned char> padding(4096,0xa5);assert(write(fd,padding.data(),padding.size())==ssize_t(padding.size()));
  const auto padded=ReadAbl(name,"abl_a",{first});assert(padded.digest==first.digest);
  image=Container(false);assert(pwrite(fd,image.data(),image.size(),0)==ssize_t(image.size()));
  const auto replaced=ReadAbl(name,"abl_a",{first});assert(replaced.digest!=first.digest && replaced.status==AblStatus::NoEfispLoader);
  close(fd);unlink(name);
  AblSession session;
  const AblSample good_a{"abl_a","old-a",AblStatus::EfispLoader}, good_b{"abl_b","old-b",AblStatus::EfispLoader};
  session.Capture({good_a,good_b});
  assert(session.Check({good_a,good_b}).text.empty());
  assert(session.Check({{"abl_a","new",AblStatus::EfispLoader},good_b}).text.empty());
  auto warning=session.Check({{"abl_a","bad",AblStatus::NoEfispLoader},good_b});
  assert(warning.text.find("overwritten")!=std::string::npos && warning.text.find("abl_a")!=std::string::npos);
  assert(warning.text.find("abl_b:")==std::string::npos && !warning.token.empty());
  assert(session.Check({good_a,good_b}).text.empty()); // restoring the old image clears the warning
  assert(!session.Check({good_a}).text.empty()); // slot B disappeared
  auto unknown=session.Check({{"abl_a","unrecognized",AblStatus::Unknown},good_b});
  assert(unknown.text.find("could not be verified")!=std::string::npos && unknown.token!=warning.token);
  session.Capture({});assert(!session.Check({}).text.empty()); // boot baseline cannot be reset
  AblSession initially_absent;initially_absent.Capture({{"abl_a","bad",AblStatus::NoEfispLoader}});
  assert(initially_absent.Check({}).text.empty());
  puts("ABL bounded reader, loader detection, hash cache and both-slot session policy passed.");
}
