// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
// Authentication-only client: never enrolls, removes templates or resets lockout.
#include <aidl/android/hardware/biometrics/fingerprint/IFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSessionCallback.h>
#include <aidl/android/hardware/biometrics/common/ICancellationSignal.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <android-base/properties.h>
#include <json/json.h>
#include <openssl/rand.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <unistd.h>

namespace fp=aidl::android::hardware::biometrics::fingerprint;
namespace bio=aidl::android::hardware::biometrics::common;
namespace km=aidl::android::hardware::keymaster;
namespace {
std::mutex output_mutex;
volatile sig_atomic_t stopping=0;
void Stop(int){stopping=1;}
void Send(const std::string& event,const std::string& message="") {
  if(event!="acquired")
    __android_log_print(ANDROID_LOG_INFO,"AERA-Fingerprint","%s: %s",event.c_str(),message.c_str());
  Json::Value value;value["event"]=event;value["message"]=message;
  Json::StreamWriterBuilder writer;writer["indentation"]="";
  const auto bytes=Json::writeString(writer,value)+"\n";
  std::lock_guard<std::mutex> lock(output_mutex);size_t done=0;
  while(done<bytes.size()){const auto n=write(STDOUT_FILENO,bytes.data()+done,bytes.size()-done);if(n<=0)break;done+=n;}
}
void Props(const fp::SensorProps& sensor) {
  Json::Value value;value["event"]="sensor";value["id"]=sensor.commonProps.sensorId;
  value["type"]=static_cast<int>(sensor.sensorType);value["handlesTouches"]=sensor.halHandlesDisplayTouches;
  value["controlsIllumination"]=sensor.halControlsIllumination;
  if(!sensor.sensorLocations.empty()) {
    const auto& location=sensor.sensorLocations.front();
    value["x"]=location.sensorLocationX;value["y"]=location.sensorLocationY;value["radius"]=location.sensorRadius;
  }
  // Some Oplus AIDL HALs leave SensorLocations empty and publish their
  // physical target through vendor metadata after secure-world startup.
  if(value.get("radius",0).asInt()<=0 &&
      android::base::GetProperty("persist.vendor.fingerprint.sensor_type","")=="ultrasonic") {
    const auto location=android::base::GetProperty("persist.vendor.fingerprint.optical.sensorlocation","");
    const int size=android::base::GetIntProperty<int>("persist.vendor.fingerprint.optical.iconsize",0);
    int x=0,y=0;char extra=0;
    if(sscanf(location.c_str(),"%d::%d%c",&x,&y,&extra)==2&&x>0&&y>0&&x<=16384&&y<=16384&&size>=16&&size<=1024) {
      value["x"]=x;value["y"]=y;value["radius"]=size/2;
      value["type"]=static_cast<int>(fp::FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC);
    }
  }
  Json::StreamWriterBuilder writer;writer["indentation"]="";
  const auto bytes=Json::writeString(writer,value)+"\n";
  std::lock_guard<std::mutex> lock(output_mutex);write(STDOUT_FILENO,bytes.data(),bytes.size());
}
class Callback final:public fp::BnSessionCallback {
 public:
  std::mutex mutex;
  std::condition_variable changed;
  bool enumerated=false,failed=false;
  std::set<int32_t> ids;
  int64_t nonce=0;
  ndk::ScopedAStatus onChallengeGenerated(int64_t)override{return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onChallengeRevoked(int64_t)override{return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onAcquired(fp::AcquiredInfo,int32_t)override{Send("acquired");return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onError(fp::Error error,int32_t vendor)override {
    {std::lock_guard<std::mutex> lock(mutex);failed=true;}
    changed.notify_all();Send("error","Fingerprint error "+std::to_string(static_cast<int>(error))+" / "+std::to_string(vendor));
    return ndk::ScopedAStatus::ok();
  }
  ndk::ScopedAStatus onEnrollmentProgress(int32_t,int32_t)override {
    Send("error","Unexpected enrollment callback; authentication cancelled.");stopping=1;return ndk::ScopedAStatus::ok();
  }
  ndk::ScopedAStatus onAuthenticationSucceeded(int32_t id,const km::HardwareAuthToken& hat)override {
    bool valid=false;
    {std::lock_guard<std::mutex> lock(mutex);valid=nonce>0&&id>0&&ids.count(id)&&hat.challenge==nonce&&
        static_cast<int32_t>(hat.authenticatorType)==2&&hat.mac.size()==32;}
    if(valid){Send("authenticated");stopping=1;}
    else Send("error","Fingerprint authentication result could not be verified.");
    return ndk::ScopedAStatus::ok();
  }
  ndk::ScopedAStatus onAuthenticationFailed()override{Send("rejected","Fingerprint not recognized. Use PIN/password or try again.");return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onLockoutTimed(int64_t)override{Send("lockout","Fingerprint temporarily locked. Use PIN/password.");stopping=1;return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onLockoutPermanent()override{Send("lockout","Fingerprint locked. Use PIN/password.");stopping=1;return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onLockoutCleared()override{return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onInteractionDetected()override{return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onEnrollmentsEnumerated(const std::vector<int32_t>& value)override {
    {std::lock_guard<std::mutex> lock(mutex);for(auto id:value)if(id>0)ids.insert(id);enumerated=true;}
    changed.notify_all();return ndk::ScopedAStatus::ok();
  }
  ndk::ScopedAStatus onEnrollmentsRemoved(const std::vector<int32_t>&)override {
    Send("error","Unexpected template removal callback; authentication cancelled.");stopping=1;return ndk::ScopedAStatus::ok();
  }
  ndk::ScopedAStatus onAuthenticatorIdRetrieved(int64_t)override{return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onAuthenticatorIdInvalidated(int64_t)override{stopping=1;Send("error","Fingerprint enrollment changed. Use PIN/password.");return ndk::ScopedAStatus::ok();}
  ndk::ScopedAStatus onSessionClosed()override{stopping=1;return ndk::ScopedAStatus::ok();}
};
}
int main(int argc,char**argv) {
  const bool probe=argc==2&&!strcmp(argv[1],"--probe");
  const bool watch=argc==2&&!strcmp(argv[1],"--watch");
  if(!probe&&!watch){Send("error","Usage: aera-fingerprint --probe|--watch");return 2;}
  if(watch&&android::base::GetProperty("ro.bootmode","")!="recovery") {
    Send("error","Fingerprint authentication is restricted to recovery.");return 2;
  }
  signal(SIGTERM,Stop);signal(SIGINT,Stop);signal(SIGPIPE,SIG_IGN);
  ABinderProcess_setThreadPoolMaxThreadCount(2);ABinderProcess_startThreadPool();
  const std::string instance=std::string(fp::IFingerprint::descriptor)+"/default";
  const auto service=fp::IFingerprint::fromBinder(ndk::SpAIBinder(AServiceManager_checkService(instance.c_str())));
  if(!service){Send("unavailable","Fingerprint service is not available.");return 1;}
  std::vector<fp::SensorProps> sensors;
  const auto status=service->getSensorProps(&sensors);
  if(!status.isOk()||sensors.empty()){Send("unavailable","Could not read fingerprint sensor properties.");return 1;}
  // One active Android user only. Never open or enumerate another user's data.
  const auto& sensor=sensors.front();Props(sensor);
  // Use the HAL's actual sensor type for input handling. Vendor location
  // metadata above only positions the icon; it must not enable pointer calls
  // for a sensor whose touches already arrive through its hardware driver.
  const bool under_display=
      sensor.sensorType==fp::FingerprintSensorType::UNDER_DISPLAY_ULTRASONIC||
      sensor.sensorType==fp::FingerprintSensorType::UNDER_DISPLAY_OPTICAL;
  const bool forward_touches=under_display&&!sensor.halHandlesDisplayTouches;
  __android_log_print(ANDROID_LOG_INFO,"AERA-Fingerprint",
      "sensor type=%d, forward display touches=%d",
      static_cast<int>(sensor.sensorType),forward_touches);
  if(probe)return 0;
  if(sensor.sensorType==fp::FingerprintSensorType::UNDER_DISPLAY_OPTICAL&&!sensor.halControlsIllumination) {
    Send("unsupported","This optical sensor needs device-specific display illumination support.");return 1;
  }
  auto callback=ndk::SharedRefBase::make<Callback>();
  std::shared_ptr<fp::ISession> session;
  if(!service->createSession(sensor.commonProps.sensorId,0,callback,&session).isOk()||!session) {
    Send("error","Could not create a fingerprint authentication session.");return 1;
  }
  if(!session->enumerateEnrollments().isOk()){Send("error","Could not inspect existing fingerprint enrollment.");session->close();return 1;}
  {
    std::unique_lock<std::mutex> lock(callback->mutex);
    if(!callback->changed.wait_for(lock,std::chrono::seconds(5),[&]{return callback->enumerated||callback->failed;})||
        callback->failed||callback->ids.empty()) {
      Send("no_enrollment","Saved fingerprints could not be loaded. Use PIN/password.");session->close();return 1;
    }
  }
  uint64_t random=0;
  if(RAND_bytes(reinterpret_cast<unsigned char*>(&random),sizeof(random))!=1){session->close();return 1;}
  {std::lock_guard<std::mutex> lock(callback->mutex);callback->nonce=static_cast<int64_t>(random&INT64_MAX);if(!callback->nonce)callback->nonce=1;}
  std::shared_ptr<bio::ICancellationSignal> cancellation;
  if(!session->authenticate(callback->nonce,&cancellation).isOk()||!cancellation) {
    Send("error","Fingerprint authentication could not start.");session->close();return 1;
  }
  // Supported sensors handle illumination themselves; no onUiReady is needed.
  Send("ready","Touch the fingerprint sensor, or use PIN/password.");
  std::string pending;
  while(!stopping) {
    pollfd pfd{STDIN_FILENO,POLLIN,0};const int ready=poll(&pfd,1,100);
    if(ready<0){if(errno==EINTR)continue;break;}
    if(!ready)continue;
    if(pfd.revents&(POLLHUP|POLLERR|POLLNVAL))break;
    char buffer[256];const ssize_t count=read(STDIN_FILENO,buffer,sizeof(buffer));if(count<=0)break;
    pending.append(buffer,count);if(pending.size()>1024)break;
    for(size_t newline;(newline=pending.find('\n'))!=std::string::npos;) {
      const auto command=pending.substr(0,newline);pending.erase(0,newline+1);
      int x=0,y=0;
      if(command=="cancel"){stopping=1;break;}
      if(command=="up"&&forward_touches)session->onPointerUp(0);
      else if(sscanf(command.c_str(),"down %d %d",&x,&y)==2&&forward_touches&&x>=0&&y>=0&&x<=16384&&y<=16384)
        session->onPointerDown(0,x,y,12.0f,16.0f);
    }
  }
  cancellation->cancel();session->close();
  return 0;
}
