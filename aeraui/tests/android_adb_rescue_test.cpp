/* SPDX-License-Identifier: Apache-2.0 */
#include "aeraui/android_adb_rescue.hpp"
#include "../features/backup/android_packages_xml.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace aeraui;
namespace fs = std::filesystem;
static std::string Read(const fs::path &p) { std::ifstream f(p,std::ios::binary); return {(std::istreambuf_iterator<char>(f)),{}}; }
static void Write(const fs::path &p,const std::string &s) { fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<s;f.close();assert(f); }
static std::string V(uint64_t n) { std::string s;do{auto b=n&127;n>>=7;s+=char(b|(n?128:0));}while(n);return s; }
static std::string F(unsigned n,const std::string &s) {return V((n<<3)|2)+V(s.size())+s;}
static std::string P(const std::string &k,const std::string &v) {return F(1,F(1,k)+F(2,v));}
static std::string Key(unsigned marker = 0) {
 std::string raw(524,'\0');raw[0]=64;raw[8]=1;raw[9]=marker;raw[263]=char(128);raw[520]=1;raw[522]=1;
 std::string b64(701,'\0');const int n=EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64.data()),reinterpret_cast<const unsigned char*>(raw.data()),raw.size());b64.resize(n);return b64+" fixture@pc";
}
static std::string Abx() {
 std::string s("ABX\0",4);s+=char(0x10);
 auto utf=[&](const std::string &v){s+=char(v.size()>>8);s+=char(v.size());s+=v;};
 auto intern=[&](const std::string &v){s+=char(255);s+=char(255);utf(v);};
 auto tag=[&](bool end,const std::string &name){s+=char(end?0x33:0x32);intern(name);};
 auto attr=[&](const std::string &name,const std::string &value){s+=char(0x2f);intern(name);utf(value);};
 tag(false,"settings");attr("version","201");tag(false,"setting");attr("id","5");attr("name","adb_enabled");attr("value","0");
 s+=char(0x1f);intern("defaultValue");tag(true,"setting");
 tag(false,"setting");attr("name","unrelated");attr("value","keep & < this");tag(true,"setting");tag(true,"settings");
 tag(false,"namespaceHashes");tag(false,"namespace");attr("name","unrelated");tag(true,"namespace");tag(true,"namespaceHashes");s+=char(0x11);return s;
}
int main() {
 if(geteuid()!=0){std::cout<<"Root is required to verify Android ownership preservation.\n";return 77;}
 const auto root=fs::temp_directory_path()/("aera-adb-rescue-test-"+std::to_string(getpid()));fs::remove_all(root);
 const auto key=Key();adb_rescue::PublicKey parsed;std::string error;assert(adb_rescue::ParsePublicKey(key,&parsed,&error));
 assert(!adb_rescue::ParsePublicKey("-----BEGIN PRIVATE KEY-----",&parsed,&error));
 assert(!adb_rescue::ParsePublicKey(key+"\n"+key,&parsed,&error));
 // Regression: inactive vendor USB properties are not the Android ADB switch.
 fs::create_directories(root/"misc/adb");
 Write(root/"system/users/0/settings_global.xml","<settings version=\"201\"><setting name=\"adb_enabled\" value=\"1\"/></settings>");
 for(const std::string functions:{"adb","mtp,adb","mtp"}) {
  Write(root/"property/persistent_properties",P("persist.sys.usb.config",functions)+P("persist.vendor.usb.config",""));
  const auto status=adb_rescue::Inspect(root);
  assert(status.available&&status.enabled&&status.usb_functions==functions);
 }
 Write(root/"system/users/0/settings_global.xml","<settings version=\"201\"><setting name=\"adb_enabled\" value=\"0\"/></settings>");
 Write(root/"property/persistent_properties",P("persist.sys.usb.config","adb"));
 assert(!adb_rescue::Inspect(root).enabled);
 for(bool binary:{false,true}) {
  fs::remove_all(root);fs::create_directories(root/"misc/adb");fs::create_directories(root/"property");
  const std::string settings=binary?Abx():"<settings version=\"201\"><setting id=\"5\" name=\"adb_enabled\" value=\"0\"/><setting name=\"unrelated\" value=\"keep\"/></settings>";
  const auto path=root/"system/users/0/settings_global.xml";
  const auto props=P("persist.sys.usb.config","mtp")+P("persist.vendor.usb.config","")+P("persist.keep","untouched")+V(80)+V(42);
  Write(path,settings);Write(path.string()+".fallback",settings);Write(root/"property/persistent_properties",props);
  Write(root/"misc/adb/adb_keys","");Write(root/"misc/adb/adb_temp_keys.xml","<keyStore version=\"1\"><wifiAP bssid=\"keep\"/></keyStore>");
  if(geteuid()==0){assert(chown(path.c_str(),1000,1000)==0);assert(chown((root/"misc/adb/adb_keys").c_str(),1000,2000)==0);}
  chmod(path.c_str(),0640);struct stat before{};assert(stat(path.c_str(),&before)==0);
  auto initial=adb_rescue::Inspect(root);assert(initial.available&&!initial.enabled&&initial.authorized_keys==0);
  assert(adb_rescue::Enable(key,&error,root));auto after=adb_rescue::Inspect(root);assert(after.available&&after.enabled&&after.can_restore&&after.authorized_keys==1);
  struct stat applied{};assert(stat(path.c_str(),&applied)==0);assert(before.st_uid==applied.st_uid&&before.st_gid==applied.st_gid&&(before.st_mode&07777)==(applied.st_mode&07777));
  const auto output=Read(path);assert((output.compare(0,4,"ABX\0",4)==0)==binary);
  tinyxml2::XMLDocument xml;assert(appvault::ParseSettingsXml(output,xml,error));
  assert(xml.FirstChildElement("settings"));if(binary)assert(xml.FirstChildElement("namespaceHashes"));
  assert(Read(root/"property/persistent_properties").find("untouched")!=std::string::npos);
  assert(Read(root/"misc/adb/adb_temp_keys.xml").find("wifiAP")!=std::string::npos);
  // Newer Android data must never be silently overwritten during rollback.
  Write(path,output+"<");assert(!adb_rescue::Restore(&error,root));Write(path,output);
  assert(adb_rescue::Restore(&error,root));assert(Read(path)==settings);assert(Read(root/"property/persistent_properties")==props);
  assert(Read(root/"misc/adb/adb_keys").empty());assert(!adb_rescue::Inspect(root).can_restore);
  // Android may rewrite unrelated settings, properties, key timestamps and Wi-Fi
  // authorizations after boot. Undo only the rescue changes, not those updates.
  assert(adb_rescue::Enable(key,&error,root));
  auto changed=Read(path);
  const std::string old_value=binary?"keep & < this":"keep", new_value=binary?"newer Android":"kept";
  assert(old_value.size()==new_value.size());auto pos=changed.find(old_value);assert(pos!=std::string::npos);
  changed.replace(pos,old_value.size(),new_value);Write(path,changed);
  Write(root/"property/persistent_properties",Read(root/"property/persistent_properties")+P("persist.android.new","preserve-me"));
  Write(root/"misc/adb/adb_keys",Read(root/"misc/adb/adb_keys")+Key(1)+"\n");
  auto meta=Read(root/"misc/adb/adb_temp_keys.xml");pos=meta.find("bssid=\"keep\"");assert(pos!=std::string::npos);
  meta.replace(pos,12,"bssid=\"android-new\"");Write(root/"misc/adb/adb_temp_keys.xml",meta);
  assert(adb_rescue::Restore(&error,root));
  assert(Read(path).find(new_value)!=std::string::npos);
  assert(Read(root/"property/persistent_properties").find("preserve-me")!=std::string::npos);
  assert(Read(root/"misc/adb/adb_keys").find(Key(1))!=std::string::npos);
  assert(Read(root/"misc/adb/adb_keys").find(key+"\n")==std::string::npos);
  assert(Read(root/"misc/adb/adb_temp_keys.xml").find("android-new")!=std::string::npos);
  assert(!adb_rescue::Inspect(root).enabled);
  // Enabling first and importing the key later remains possible.
  assert(adb_rescue::Enable("",&error,root));assert(adb_rescue::Enable(key,&error,root));
  assert(adb_rescue::Restore(&error,root));assert(adb_rescue::Inspect(root).enabled);
  // Reset the fixture for the independent corruption checks.
  Write(path,settings);Write(root/"property/persistent_properties",props);
  // Pending AtomicFile recovery and symlinks are rejected before any writes.
  Write(path.string()+".bak",settings);assert(!adb_rescue::Enable(key,&error,root));fs::remove(path.string()+".bak");
  fs::remove(path);fs::create_symlink(root/"property/persistent_properties",path);assert(!adb_rescue::Enable(key,&error,root));assert(Read(root/"property/persistent_properties")==props);
 }
 if(geteuid()==0){
  fs::remove_all(root);fs::create_directories(root/"misc/adb");Write(root/"system/users/0/settings_global.xml","<settings version=\"201\"/>");Write(root/"property/persistent_properties",P("persist.sys.usb.config","mtp"));
  assert(adb_rescue::Enable(key,&error,root));struct stat k{},t{};assert(stat((root/"misc/adb/adb_keys").c_str(),&k)==0);assert(stat((root/"misc/adb/adb_temp_keys.xml").c_str(),&t)==0);
  assert(k.st_uid==1000&&k.st_gid==2000&&(k.st_mode&0777)==0640);assert(t.st_uid==1000&&t.st_gid==2000&&(t.st_mode&0777)==0600);
  assert(adb_rescue::Restore(&error,root));assert(!fs::exists(root/"misc/adb/adb_keys"));
 }
 fs::remove_all(root);std::cout<<"Android ADB rescue XML/ABX, keys, metadata, protobuf preservation and rollback passed.\n";
}
