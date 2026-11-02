// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
#include <aeraui/lock_security.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace aeraui::lock_security {
namespace {
constexpr const char* kFile = "lockscreen.conf";
uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string Hex(const unsigned char* p, size_t n) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i=0;i<n;++i) { out += digits[p[i]>>4]; out += digits[p[i]&15]; }
  return out;
}
bool Unhex(const std::string& in, unsigned char* out, size_t size) {
  if (in.size()!=2*size) return false;
  auto digit=[](char c)->int { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; };
  for(size_t i=0;i<size;++i) {
    const int a=digit(in[2*i]),b=digit(in[2*i+1]); if(a<0||b<0)return false;
    out[i]=static_cast<unsigned char>((a<<4)|b);
  }
  return true;
}
bool Derive(const std::string& secret, const std::array<unsigned char,32>& salt,
            std::array<unsigned char,32>* hash) {
  return EVP_PBE_scrypt(secret.data(),secret.size(),salt.data(),salt.size(),
                       32768,8,3,64*1024*1024,hash->data(),hash->size())==1;
}
struct Fd { int value; ~Fd(){if(value>=0)close(value);} };
}
struct Store::Record {
  bool exists=false,valid=false,fingerprint=false;
  Kind kind=Kind::None;
  std::array<unsigned char,32> salt{},hash{};
  std::string error;
};
Store::Store(std::string directory):directory_(std::move(directory)) {}
bool Store::ValidSecret(Kind kind,const std::string& value) {
  if(kind==Kind::Pin)return value.size()>=4&&value.size()<=16&&
      value.find_first_not_of("0123456789")==std::string::npos;
  return kind==Kind::Password&&value.size()>=6&&value.size()<=128&&
      std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32||c==127;});
}
Store::Record Store::Read() const {
  Record record;
  Fd dir{open(directory_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
  if(dir.value<0) {
    if(errno!=ENOENT){record.exists=true;record.error="Lock settings are unavailable.";}
    return record;
  }
  struct stat directory_stat{};
  if(fstat(dir.value,&directory_stat)||directory_stat.st_uid!=geteuid()||(directory_stat.st_mode&077)!=0) {
    record.exists=true;record.error="Private lock settings have unsafe permissions.";return record;
  }
  Fd fd{openat(dir.value,kFile,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
  if(fd.value<0) {
    if(errno!=ENOENT){record.exists=true;record.error="Lock settings could not be read.";}
    return record;
  }
  record.exists=true;
  struct stat st{};
  if(fstat(fd.value,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||
      (st.st_mode&077)!=0||st.st_size<=0||st.st_size>1024) {
    record.error="Invalid lock settings. The screen remains locked.";return record;
  }
  std::string bytes(st.st_size,'\0');size_t offset=0;
  while(offset<bytes.size()) {
    const auto count=read(fd.value,bytes.data()+offset,bytes.size()-offset);
    if(count<0&&errno==EINTR)continue;
    if(count<=0){record.error="Lock settings could not be read.";return record;}offset+=count;
  }
  std::istringstream input(bytes);std::string version,kind,salt,hash,extra;int fingerprint=-1;
  if(!(input>>version>>kind>>fingerprint>>salt>>hash)||(input>>extra)||version!="AERA_LOCK_1"||
      (kind!="pin"&&kind!="password")||(fingerprint!=0&&fingerprint!=1)||
      !Unhex(salt,record.salt.data(),record.salt.size())||!Unhex(hash,record.hash.data(),record.hash.size())) {
    record.error="Invalid lock settings. The screen remains locked.";return record;
  }
  record.kind=kind=="pin"?Kind::Pin:Kind::Password;
  record.fingerprint=fingerprint==1;record.valid=true;return record;
}
uint32_t Store::RetrySeconds() const {
  std::lock_guard<std::mutex> lock(rate_);const auto now=Now();
  return retry_at_>now?static_cast<uint32_t>((retry_at_-now+999)/1000):0;
}
void Store::Failed() {
  std::lock_guard<std::mutex> lock(rate_);++failures_;
  const uint64_t delay=failures_<5?1000:std::min<uint64_t>(300000,30000ULL<<std::min(4U,(failures_-5)/5));
  retry_at_=Now()+delay;
}
Status Store::Inspect() const {
  const auto record=Read();Status out;
  out.available=record.error.empty();out.configured=record.exists;out.valid=record.valid;
  out.kind=record.kind;out.fingerprint=record.valid&&record.fingerprint;
  out.retry_seconds=RetrySeconds();out.error=record.error;return out;
}
Result Store::Check(const Record& record,const std::string& value) {
  if(RetrySeconds())return {false,"Too many attempts. Wait before trying again."};
  if(!record.valid)return {false,record.exists?record.error:"No screen-lock credential is configured."};
  if(!ValidSecret(record.kind,value)){Failed();return {false,"Incorrect PIN or password."};}
  std::array<unsigned char,32> actual{};
  const bool ok=Derive(value,record.salt,&actual)&&CRYPTO_memcmp(actual.data(),record.hash.data(),actual.size())==0;
  OPENSSL_cleanse(actual.data(),actual.size());
  if(!ok){Failed();return {false,"Incorrect PIN or password."};}
  {std::lock_guard<std::mutex> lock(rate_);failures_=0;retry_at_=0;}
  return {true,{}};
}
Result Store::Verify(const std::string& value) {
  std::lock_guard<std::mutex> lock(operation_);return Check(Read(),value);
}
Result Store::Write(const Record& record) {
  if(mkdir(directory_.c_str(),0700)&&errno!=EEXIST)return {false,"Could not create private lock settings."};
  Fd dir{open(directory_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
  if(dir.value<0)return {false,"Private lock settings are unavailable."};
  struct stat st{};
  if(fstat(dir.value,&st)||st.st_uid!=geteuid()||(st.st_mode&077)!=0)
    return {false,"Private lock settings have unsafe permissions."};
  std::array<unsigned char,12> nonce{};
  if(RAND_bytes(nonce.data(),nonce.size())!=1)return {false,"Could not create secure lock settings."};
  const auto temporary=".lock-"+Hex(nonce.data(),nonce.size());
  Fd fd{openat(dir.value,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600)};
  if(fd.value<0)return {false,"Could not save the screen lock."};
  const std::string bytes="AERA_LOCK_1 "+std::string(record.kind==Kind::Pin?"pin":"password")+" "+
      (record.fingerprint?"1":"0")+" "+Hex(record.salt.data(),record.salt.size())+" "+Hex(record.hash.data(),record.hash.size())+"\n";
  size_t offset=0;bool ok=true;
  while(offset<bytes.size()) {
    const auto count=write(fd.value,bytes.data()+offset,bytes.size()-offset);
    if(count<0&&errno==EINTR)continue;
    if(count<=0){ok=false;break;}offset+=count;
  }
  ok=ok&&fsync(fd.value)==0&&renameat(dir.value,temporary.c_str(),dir.value,kFile)==0;
  if(!ok){unlinkat(dir.value,temporary.c_str(),0);return {false,"Could not save the screen lock."};}
  if(fsync(dir.value))return {false,"Could not synchronize the screen lock."};
  return {true,{}};
}
Result Store::Set(const std::string& current,Kind kind,const std::string& value,const std::string& confirmation) {
  std::lock_guard<std::mutex> lock(operation_);const auto old=Read();
  if(old.exists){const auto checked=Check(old,current);if(!checked.ok)return checked;}
  if(value!=confirmation)return {false,"The PIN or password entries do not match."};
  if(!ValidSecret(kind,value))return {false,kind==Kind::Pin?"Use 4 to 16 digits.":"Use 6 to 128 characters."};
  Record record;record.kind=kind;record.exists=record.valid=true;
  // Changing the fallback credential explicitly turns fingerprint off again.
  if(RAND_bytes(record.salt.data(),record.salt.size())!=1||!Derive(value,record.salt,&record.hash))
    return {false,"Could not prepare the screen-lock credential."};
  return Write(record);
}
Result Store::Remove(const std::string& current) {
  std::lock_guard<std::mutex> lock(operation_);const auto checked=Check(Read(),current);if(!checked.ok)return checked;
  Fd dir{open(directory_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
  if(dir.value<0||unlinkat(dir.value,kFile,0)||fsync(dir.value))return {false,"Could not remove the screen lock."};
  return {true,{}};
}
Result Store::SetFingerprint(const std::string& current,bool enabled) {
  std::lock_guard<std::mutex> lock(operation_);auto record=Read();const auto checked=Check(record,current);
  if(!checked.ok)return checked;
  record.fingerprint=enabled;return Write(record);
}
} // namespace aeraui::lock_security
