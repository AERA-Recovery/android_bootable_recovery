// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
// Authentication only: never enroll, remove, or reset biometric lockout.
#include <android/hardware/biometrics/fingerprint/2.3/IBiometricsFingerprint.h>
#include <android/hardware/biometrics/fingerprint/2.1/IBiometricsFingerprintClientCallback.h>
#include <android-base/properties.h>
#include <android/log.h>
#include <hardware/hw_auth_token.h>
#include <hidl/HidlTransportSupport.h>
#include <json/json.h>
#include <openssl/rand.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <endian.h>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <unistd.h>
namespace fp=android::hardware::biometrics::fingerprint::V2_1;
namespace fp3=android::hardware::biometrics::fingerprint::V2_3;
using android::hardware::Return;
using android::hardware::Void;
using android::hardware::hidl_vec;
namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};
std::mutex output_mutex;
void Stop(int){stopping=1;}
void Emit(Json::Value value) {
  Json::StreamWriterBuilder builder;builder["indentation"]="";
  const auto line=Json::writeString(builder,value)+"\n";
  std::lock_guard<std::mutex> lock(output_mutex);
  size_t offset=0;
  while(offset<line.size()) {
    const auto count=write(STDOUT_FILENO,line.data()+offset,line.size()-offset);
    if(count<0&&errno==EINTR)continue;
    if(count<=0)break;
    offset+=count;
  }
}
void Send(const char* event,const std::string& message="") {
  if(std::strcmp(event,"acquired"))__android_log_print(ANDROID_LOG_INFO,"AERA-Fingerprint","%s: %s",event,message.c_str());
  Json::Value value;value["event"]=event;value["message"]=message;Emit(value);
}
struct Callback final:fp::IBiometricsFingerprintClientCallback {
  std::mutex mutex;std::condition_variable changed;
  std::set<uint32_t> ids;bool enumerated=false,failed=false;uint64_t challenge=0;
  Return<void> onEnrollResult(uint64_t,uint32_t,uint32_t,uint32_t)override {
    Send("error","Unexpected enrollment callback; authentication cancelled.");stopping=1;return Void();
  }
  Return<void> onRemoved(uint64_t,uint32_t,uint32_t,uint32_t)override {
    Send("error","Fingerprint enrollment changed. Use PIN/password.");stopping=1;return Void();
  }
  Return<void> onAcquired(uint64_t,fp::FingerprintAcquiredInfo,int32_t)override {Send("acquired");return Void();}
  Return<void> onEnumerate(uint64_t,uint32_t id,uint32_t group,uint32_t remaining)override {
    {std::lock_guard<std::mutex> lock(mutex);if(id&&group==0)ids.insert(id);if(!remaining)enumerated=true;}
    changed.notify_all();return Void();
  }
  Return<void> onError(uint64_t,fp::FingerprintError error,int32_t vendor)override {
    if(stopping)return Void();
    {std::lock_guard<std::mutex> lock(mutex);failed=true;}changed.notify_all();
    Send(error==fp::FingerprintError::ERROR_LOCKOUT?"lockout":"error",
         "Fingerprint error "+std::to_string(static_cast<int>(error))+" / "+std::to_string(vendor));
    stopping=1;return Void();
  }
  Return<void> onAuthenticated(uint64_t,uint32_t id,uint32_t group,const hidl_vec<uint8_t>& token)override {
    if(stopping)return Void();
    if(!id){Send("rejected","Fingerprint not recognized. Use PIN/password or try again.");return Void();}
    bool valid=false;
    if(group==0&&token.size()==sizeof(hw_auth_token_t)) {
      hw_auth_token_t hat{};std::memcpy(&hat,token.data(),sizeof(hat));
      std::lock_guard<std::mutex> lock(mutex);
      valid=ids.count(id)&&challenge>0&&hat.version==0&&hat.challenge==challenge&&be32toh(hat.authenticator_type)==2;
    }
    // As with AIDL, trust the HAL/TEE MAC; validate the session and token shape.
    Send(valid?"authenticated":"error",valid?"":"Fingerprint authentication result could not be verified.");
    stopping=1;return Void();
  }
};
bool Ok(const Return<fp::RequestStatus>& result) {return result.isOk()&&static_cast<fp::RequestStatus>(result)==fp::RequestStatus::SYS_OK;}
}
int main(int argc,char** argv) {
  signal(SIGTERM,Stop);signal(SIGINT,Stop);signal(SIGPIPE,SIG_IGN);
  android::hardware::configureRpcThreadpool(2,false);
  auto service=fp3::IBiometricsFingerprint::tryGetService();
  if(!service){Send("unavailable","Fingerprint service is not available.");return 1;}
  const bool optical=android::base::GetProperty("persist.vendor.fingerprint.sensor_type","")=="optical";
  const int x=android::base::GetIntProperty<int>("vendor.aera.fingerprint.sensor_x",0);
  const int y=android::base::GetIntProperty<int>("vendor.aera.fingerprint.sensor_y",0);
  const int radius=android::base::GetIntProperty<int>("vendor.aera.fingerprint.sensor_radius",0);
  const int width=android::base::GetIntProperty<int>("vendor.aera.fingerprint.display_width",0);
  const int height=android::base::GetIntProperty<int>("vendor.aera.fingerprint.display_height",0);
  if(x<=0||y<=0||radius<8||radius>512||width<=x||height<=y||width>16384||height>16384) {
    Send("unsupported","Fingerprint sensor location is not available.");return 1;
  }
  if(optical&&!android::base::GetBoolProperty("vendor.aera.fingerprint.optical_illumination",false)) {
    Send("unsupported","This optical sensor needs device-specific display illumination support.");return 1;
  }
  Json::Value sensor;sensor["event"]="sensor";sensor["x"]=x;sensor["y"]=y;sensor["radius"]=radius;
  sensor["displayWidth"]=width;sensor["displayHeight"]=height;sensor["illumination"]=optical;Emit(sensor);
  if(argc>1&&std::string(argv[1])=="--probe")return 0;
  android::sp<Callback> callback=new Callback;
  auto notified=service->setNotify(callback);
  if(!notified.isOk()||!static_cast<uint64_t>(notified)){Send("error","Could not create a fingerprint authentication session.");return 1;}
  const auto cleanup=[&]{stopping=1;service->onFingerUp();service->cancel();};
  if(!Ok(service->setActiveGroup(0,"/data/vendor_de/0/fpdata"))||!Ok(service->enumerate())) {
    Send("error","Could not inspect existing fingerprint enrollment.");cleanup();return 1;
  }
  {
    std::unique_lock<std::mutex> lock(callback->mutex);
    if(!callback->changed.wait_for(lock,std::chrono::seconds(5),[&]{return callback->enumerated||callback->failed;})||callback->failed||callback->ids.empty()) {
      lock.unlock();Send("no_enrollment","Saved fingerprints could not be loaded. Use PIN/password.");cleanup();return 1;
    }
  }
  uint64_t random=0;
  if(RAND_bytes(reinterpret_cast<unsigned char*>(&random),sizeof(random))!=1){cleanup();return 1;}
  {std::lock_guard<std::mutex> lock(callback->mutex);callback->challenge=(random&INT64_MAX);if(!callback->challenge)callback->challenge=1;}
  if(!Ok(service->authenticate(callback->challenge,0))){Send("error","Fingerprint authentication could not start.");cleanup();return 1;}
  Send("ready","Touch the fingerprint sensor, or use PIN/password.");
  std::string pending;
  while(!stopping) {
    pollfd descriptor{STDIN_FILENO,POLLIN,0};const int ready=poll(&descriptor,1,100);
    if(ready<0){if(errno==EINTR)continue;break;}if(!ready)continue;
    if(descriptor.revents&(POLLHUP|POLLERR|POLLNVAL))break;
    char buffer[256];const auto count=read(STDIN_FILENO,buffer,sizeof(buffer));if(count<=0)break;
    pending.append(buffer,count);if(pending.size()>1024)break;
    for(size_t newline;(newline=pending.find('\n'))!=std::string::npos;) {
      const auto command=pending.substr(0,newline);pending.erase(0,newline+1);
      int tx=0,ty=0;
      if(command=="cancel"){stopping=1;break;}
      if(command=="up")service->onFingerUp();
      else if(sscanf(command.c_str(),"down %d %d",&tx,&ty)==2&&tx>=0&&ty>=0&&tx<width&&ty<height)
        service->onFingerDown(tx,ty,12.0f,16.0f);
    }
  }
  cleanup();return 0;
}
