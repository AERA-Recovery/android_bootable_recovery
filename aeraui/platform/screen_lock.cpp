// SPDX-License-Identifier: Apache-2.0
#include <aeraui/backend.hpp>
#include <aeraui/screen_lock.hpp>
#include <android-base/properties.h>
#include "partitions.hpp"
#include <minuitwrp/minui.h>
#include <json/json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace aeraui {
namespace {
lock_security::Store store("/data/misc/aera-lock");
std::mutex gate;
uint64_t generation=0,revision=0;
bool active=false, required=false, granted=false;
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
pid_t child=-1;
int output=-1,input=-1;
std::string pending;
ScreenFingerprint fingerprint,cached_sensor;
std::atomic<int> closing_children{0};
bool enabled=false,service_requested=false;
uint64_t started=0,spawned=0,retry_at=0;
uint64_t Now() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void CloseChild() {
 gr_set_fingerprint_pressed(false);
 if(input>=0){close(input);input=-1;}if(output>=0){close(output);output=-1;}
 if(child>0) {
   const pid_t pid=child;child=-1;++closing_children;
   // EOF lets the helper cancel and close its HAL session before another opens.
   std::thread([pid]{
     bool reaped=false;
     for(int attempt=0;attempt<25;++attempt) {
       const auto result=waitpid(pid,nullptr,WNOHANG);
       if(result==pid||(result<0&&errno==ECHILD)){reaped=true;break;}
       std::this_thread::sleep_for(std::chrono::milliseconds(10));
     }
     if(!reaped){kill(pid,SIGKILL);while(waitpid(pid,nullptr,0)<0&&errno==EINTR){}}
     --closing_children;
   }).detach();
 }
 pending.clear();
}
void StopFingerprint(bool stop_service=true) {
 enabled=false;service_requested=false;fingerprint={};CloseChild();
 if(stop_service)android::base::SetProperty("vendor.aera.fingerprint.start","0");
}
void Spawn() {
 if(closing_children.load()>0)return;
 int in[2],out[2];if(pipe2(in,O_CLOEXEC)){enabled=false;return;}
 if(pipe2(out,O_CLOEXEC)){close(in[0]);close(in[1]);enabled=false;return;}
 // Vendor graphics atfork handlers can deadlock recovery; child only performs exec-safe syscalls.
 const pid_t pid=_Fork();
 if(pid==0){dup2(in[0],STDIN_FILENO);dup2(out[1],STDOUT_FILENO);close(in[0]);close(in[1]);close(out[0]);close(out[1]);execl("/system/bin/aera-fingerprint","aera-fingerprint","--watch",static_cast<char*>(nullptr));_exit(127);}
 close(in[0]);close(out[1]);
 if(pid<0){close(in[1]);close(out[0]);enabled=false;return;}
 input=in[1];output=out[0];child=pid;spawned=Now();
 fcntl(output,F_SETFL,fcntl(output,F_GETFL)|O_NONBLOCK);
 fcntl(input,F_SETFL,fcntl(input,F_GETFL)|O_NONBLOCK);
}
#else
void StopFingerprint(bool=true) {}
#endif
bool DataAvailable() {
 auto* data=PartitionManager.Find_Partition_By_Path("/data");
 return data&&data->Is_Mounted()&&!RecoveryDataLocked();
}
lock_security::Result Unavailable(){return {false,"Unlock internal storage before changing the AERA screen lock."};}
}
lock_security::Status RecoveryScreenLockStatus() {
 if(!DataAvailable()){lock_security::Status status;status.error="Unlock internal storage to use AERA screen-lock settings.";return status;}
 return store.Inspect();
}
lock_security::Result RecoverySetScreenLock(const std::string& current,lock_security::Kind kind,const std::string& secret,const std::string& repeat) {
 if(!DataAvailable())return Unavailable();
 auto result=store.Set(current,kind,secret,repeat);
 if(result.ok){std::lock_guard<std::mutex> lock(gate);++revision;granted=false;if(active)required=true;StopFingerprint();}
 return result;
}
lock_security::Result RecoveryRemoveScreenLock(const std::string& current) {
 if(!DataAvailable())return Unavailable();auto result=store.Remove(current);
 if(result.ok){std::lock_guard<std::mutex> lock(gate);++revision;granted=false;required=false;StopFingerprint();}
 return result;
}
lock_security::Result RecoverySetScreenFingerprint(const std::string& current,bool value) {
 if(!kFingerprintSupported)return {false,"Fingerprint support is not enabled in this build."};
 if(!DataAvailable())return Unavailable();auto result=store.SetFingerprint(current,value);
 if(result.ok){std::lock_guard<std::mutex> lock(gate);++revision;granted=false;StopFingerprint();}
 return result;
}
void RecoveryPrepareScreenFingerprint() {
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
 std::lock_guard<std::mutex> lock(gate);
 const auto status=RecoveryScreenLockStatus();
 if(!status.valid||!status.fingerprint||!DataAvailable())return;
 if(android::base::GetBoolProperty("post.decrypt.modules",false))
   android::base::SetProperty("vendor.aera.fingerprint.start","1");
#endif
}
void RecoveryShutdownScreenFingerprint() {
 std::lock_guard<std::mutex> lock(gate);StopFingerprint();active=false;granted=false;++generation;
}
uint64_t RecoveryBeginScreenLock(bool ambient) {
 std::lock_guard<std::mutex> lock(gate);StopFingerprint(false);++generation;active=true;granted=false;
 const auto status=RecoveryScreenLockStatus();required=status.configured||(!status.available&&required);
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
 if(required&&status.valid&&status.fingerprint&&DataAvailable()) {
   fingerprint=cached_sensor;
   enabled=!ambient||RecoveryLockTheme().aod_fingerprint;
   started=0;retry_at=0;
   fingerprint.message="Starting fingerprint sensor...";
   if(android::base::GetBoolProperty("post.decrypt.modules",false))
     android::base::SetProperty("vendor.aera.fingerprint.start","1");
 }else StopFingerprint();
#else
 (void)ambient;
#endif
 return generation;
}
void RecoveryEndScreenLock(uint64_t token,bool keep_sensor_warm) {
 std::lock_guard<std::mutex> lock(gate);if(!active||token!=generation)return;
 StopFingerprint(!keep_sensor_warm);active=false;granted=false;++generation;
}
lock_security::Result RecoveryVerifyScreenLock(uint64_t token,const std::string& secret) {
 uint64_t checked_revision;
 {std::lock_guard<std::mutex> lock(gate);checked_revision=revision;if(!active||token!=generation||!required)return {false,"The lock screen has changed. Try again."};}
 if(!DataAvailable())return Unavailable();
 auto result=store.Verify(secret);
 std::lock_guard<std::mutex> lock(gate);
 if(!active||token!=generation||checked_revision!=revision)return {false,"The lock screen has changed. Try again."};
 if(result.ok)granted=true;return result;
}
bool RecoveryConsumeScreenUnlock(uint64_t token) {
 std::lock_guard<std::mutex> lock(gate);if(!active||token!=generation||(required&&!granted))return false;
 granted=false;StopFingerprint();active=false;++generation;return true;
}
ScreenFingerprint RecoveryPollScreenFingerprint(uint64_t token) {
 std::lock_guard<std::mutex> lock(gate);
 if(!active||token!=generation)return {};
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
 if(!enabled)return fingerprint;
 if(!DataAvailable()){StopFingerprint();return {};}
 if(!service_requested) {
   if(!android::base::GetBoolProperty("post.decrypt.modules",false))return fingerprint;
   if(!android::base::SetProperty("vendor.aera.fingerprint.start","1")) {
     enabled=false;fingerprint.message="Could not start fingerprint service. Use PIN/password.";return fingerprint;
   }
   service_requested=true;started=Now();retry_at=started;
 }
 if(child<0&&Now()>=retry_at)Spawn();
 if(output>=0) {
   char buffer[2048];ssize_t count;
   while((count=read(output,buffer,sizeof(buffer)))>0){pending.append(buffer,count);if(pending.size()>16384){StopFingerprint();return {};}}
   for(size_t newline;(newline=pending.find('\n'))!=std::string::npos;) {
     const auto line=pending.substr(0,newline);pending.erase(0,newline+1);
     Json::CharReaderBuilder builder;std::unique_ptr<Json::CharReader> reader(builder.newCharReader());Json::Value value;std::string error;
     if(!reader->parse(line.data(),line.data()+line.size(),&value,&error))continue;
     const auto event=value["event"].asString();
     if(event=="sensor"){fingerprint.display_width=value.get("displayWidth",gr_fb_width()).asInt();fingerprint.display_height=value.get("displayHeight",gr_fb_height()).asInt();fingerprint.x=value["x"].asInt();fingerprint.y=value["y"].asInt();fingerprint.radius=value["radius"].asInt();fingerprint.illumination=value.get("illumination",false).asBool();cached_sensor=fingerprint;cached_sensor.ready=false;cached_sensor.authenticated=false;cached_sensor.message.clear();}
     else if(event=="authenticated"&&fingerprint.ready){granted=true;fingerprint.authenticated=true;enabled=false;CloseChild();android::base::SetProperty("vendor.aera.fingerprint.start","0");break;}
     else if(event=="ready"){fingerprint.ready=true;fingerprint.message=value["message"].asString();}
     else if(event=="unavailable"&&!fingerprint.ready&&Now()-started<15000){CloseChild();retry_at=Now()+500;break;}
     else if(event=="error"||event=="unavailable"||event=="unsupported"||event=="lockout"||event=="no_enrollment") {
       fingerprint.message=value["message"].asString();fingerprint.ready=false;enabled=false;CloseChild();android::base::SetProperty("vendor.aera.fingerprint.start","0");break;
     }else if(event=="rejected")fingerprint.message=value["message"].asString();
   }
 }
 if(enabled&&child>0&&!fingerprint.ready&&Now()-spawned>7000){CloseChild();enabled=false;fingerprint.message="Fingerprint unavailable. Use PIN/password.";android::base::SetProperty("vendor.aera.fingerprint.start","0");}
 if(child>0){int status=0;const auto ended=waitpid(child,&status,WNOHANG);if(ended==child){child=-1;if(!fingerprint.authenticated){fingerprint.ready=false;enabled=false;fingerprint.message="Fingerprint stopped. Use PIN/password.";}CloseChild();android::base::SetProperty("vendor.aera.fingerprint.start","0");}}
 return fingerprint;
#else
 return {};
#endif
}
void RecoveryScreenFingerprintTouch(uint64_t token,bool down,int x,int y) {
#if defined(AERA_SUPPORT_FINGERPRINT) && AERA_SUPPORT_FINGERPRINT == 1
 std::lock_guard<std::mutex> lock(gate);if(!active||token!=generation||!enabled||!fingerprint.ready||input<0)return;
 if(fingerprint.illumination)gr_set_fingerprint_pressed(down);
 const auto command=down?"down "+std::to_string(x)+" "+std::to_string(y)+"\n":"up\n";
 // The reader can exit asynchronously; block SIGPIPE only on this calling thread.
 sigset_t set,old;sigemptyset(&set);sigaddset(&set,SIGPIPE);pthread_sigmask(SIG_BLOCK,&set,&old);
 const auto written=write(input,command.data(),command.size());
 if(written<0&&errno==EPIPE){timespec zero{};sigtimedwait(&set,nullptr,&zero);}
 pthread_sigmask(SIG_SETMASK,&old,nullptr);
#else
 (void)token;(void)down;(void)x;(void)y;
#endif
}
}
