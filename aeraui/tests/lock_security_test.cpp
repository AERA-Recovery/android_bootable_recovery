#include <aeraui/lock_security.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace aeraui::lock_security;
int main(){char d[]="/tmp/aera-lock-store-XXXXXX";assert(mkdtemp(d));Store store(d);
assert(!store.Inspect().configured);assert(!store.SetFingerprint("",true).ok);
assert(!store.Set("",Kind::Pin,"123","123").ok);
assert(!store.Set("",Kind::Pin,"673829","673828").ok);
assert(store.Set("",Kind::Pin,"673829","673829").ok);
assert(store.Inspect().configured && store.Inspect().valid && !store.Inspect().fingerprint);
assert(store.Verify("673829").ok);assert(store.SetFingerprint("673829",true).ok);
assert(store.Inspect().fingerprint);
assert(store.Set("673829",Kind::Password,"Mango planet 82","Mango planet 82").ok);
assert(!store.Inspect().fingerprint);assert(store.Verify("Mango planet 82").ok);
std::ifstream file(std::string(d)+"/lockscreen.conf");std::string contents;getline(file,contents);
assert(contents.find("Mango")==std::string::npos);struct stat st{};assert(!stat((std::string(d)+"/lockscreen.conf").c_str(),&st)&&(st.st_mode&0777)==0600);
assert(store.Remove("Mango planet 82").ok);assert(!store.Inspect().configured);
assert(store.Set("",Kind::Pin,"583921","583921").ok);
assert(!store.Verify("000000").ok);assert(store.Inspect().retry_seconds>0);
assert(!store.Verify("583921").ok);
std::ofstream(std::string(d)+"/lockscreen.conf")<<"corrupt";
assert(store.Inspect().configured && !store.Inspect().valid);
std::filesystem::remove_all(d);
}
