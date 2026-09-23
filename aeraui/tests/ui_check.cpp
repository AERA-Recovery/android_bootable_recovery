#include "scene.hpp"
#include "ui_components.hpp"
#include "picture_viewer.hpp"
#include "browser/runtime.hpp"
#include "browser/protocol.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <png.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace aeraui;
static std::string backup_root;
static std::string file_root;
static bool preferences[8] = {true, false, true, true, false, false, false, true};
static int utc_offset = 120;
static uint32_t accent_color = design::kDefaultAccentRgb;
static bool light_mode = false;
static bool save_succeeds = true;
static InterfaceSize interface_size = InterfaceSize::kNormal;
static std::string active_slot = "A";
static bool wifi_auto_enable = false;
static bool wifi_auto_connect = false;
static WifiRequest wifi_request;
static SideloadStatus sideload_status;
static int callback_count = 0;
static Action last_action = Action::kNone;
static void RecordAction(Action action, void*) { ++callback_count; last_action = action; }
namespace aeraui::web {
bool RuntimeInstalled() { return true; }
void PrepareRuntime(Preparation &state, const char *, const char *) {
  state.verified = true; state.progress.store(100); state.done.store(true);
}
void RemoveRuntime(const std::string &) {}
std::string LaunchBlockReason() { return "Sandbox unavailable in this test."; }
}
namespace aeraui {
bool RecoveryPreference(Preference p) { return preferences[static_cast<int>(p)]; }
bool RecoverySetPreference(Preference p, bool enabled) { preferences[static_cast<int>(p)] = enabled; return true; }
bool RecoverySha256Available() { return true; }
int RecoveryUtcOffset() { return utc_offset; }
bool RecoverySetUtcOffset(int minutes) { utc_offset = minutes; return true; }
uint32_t RecoveryAccentColor() { return accent_color; }
bool RecoverySetAccentColor(uint32_t rgb) { accent_color = rgb; return true; }
bool RecoveryLightMode() { return light_mode; }
bool RecoverySetLightMode(bool enabled) { light_mode = enabled; return true; }
InterfaceSize RecoveryInterfaceSize() { return interface_size; }
bool RecoverySetInterfaceSize(InterfaceSize size) {
  return static_cast<int>(size) >= 0 && static_cast<int>(size) <= 2;
}
KeyboardLayout RecoveryKeyboardLayout() { return KeyboardLayout::kQwerty; }
bool RecoverySetKeyboardLayout(KeyboardLayout layout) {
  return layout == KeyboardLayout::kQwerty ||
         layout == KeyboardLayout::kQwertz;
}
int RecoveryHomeGridColumns() { return 3; }
bool RecoverySetHomeGridColumns(int columns) { return columns == 2 || columns == 3; }
DockLayout RecoveryDockLayout() { return DockLayout::kGlass; }
bool RecoverySetDockLayout(DockLayout) { return true; }
int RecoveryDockTransparency() { return 60; }
bool RecoverySetDockTransparency(int) { return true; }
int RecoveryDockBlur() { return 24; }
bool RecoverySetDockBlur(int) { return true; }
bool RecoveryDockHideInApps() { return false; }
bool RecoverySetDockHideInApps(bool) { return true; }
std::string recovery_language = "en";
std::string RecoveryLanguage() { return recovery_language; }
bool RecoverySetLanguage(const std::string &language) {
  recovery_language = language;
  return true;
}
std::string RecoveryBrowserHomepage() { return "aera://start"; }
bool RecoverySetBrowserHomepage(const std::string &) { return true; }
int RecoveryBrowserZoom() { return 100; }
bool RecoverySetBrowserZoom(int) { return true; }
BrowserCookiePolicy RecoveryBrowserCookiePolicy() {
  return BrowserCookiePolicy::kFirstParty;
}
bool RecoverySetBrowserCookiePolicy(BrowserCookiePolicy) { return true; }
bool RecoverySavePreferences() { return save_succeeds; }
bool RecoveryHapticsAvailable() { return true; }
int RecoveryHapticDuration(Haptic haptic) {
  return haptic == Haptic::kAction ? 160 : 40;
}
bool RecoverySetHapticDuration(Haptic, int) { return true; }
void RecoveryVibrate(Haptic) {}
void RecoveryTerminalStart(int, int, int, int) {}
bool RecoveryTerminalPoll() { return false; }
int RecoveryTerminalUpdateCounter() { return 0; }
std::vector<std::string> RecoveryTerminalLines(size_t) { return {}; }
void RecoveryTerminalWrite(const std::string &) {}
void RecoveryTerminalSendKey(TerminalKey) {}
void RecoveryTerminalClear() {}
bool RecoveryTerminalRunning() { return true; }
void AttachStatusBar(lv_obj_t *screen, void (*)(Action,void*),void*,StatusBarAction,bool) {
  auto *label=design::Label(screen,"23:30                         100%",&lv_font_montserrat_32,design::kText);
  lv_obj_set_pos(label,54,65);
}
int32_t StatusBarHeight() { return 165; }
bool StatusBarShadeOpen() { return false; }
std::vector<Volume> RecoveryVolumes(const std::string &kind) {
  if (kind == "wipe") return {{"Dalvik / ART cache","DALVIK",0},{"Data","/data",23000000000},
    {"Internal storage","INTERNAL",12000000000},{"Metadata","/metadata",40960000}};
  return {{"Metadata","/metadata",40960000},{"Data","/data",23000000000},{"Vendor Boot","/vendor_boot",100663296},
    {"Boot","/boot",100663296},{"DTBO","/dtbo",25165824},{"EFS","/efs",12648448},{"Init Boot","/init_boot",8388608},{"Modem","/modem",446693376}};
}
std::vector<Volume> RecoveryRestoreVolumes(const std::string &) { return RecoveryVolumes(""); }
std::string RecoveryStorage() { return file_root.empty() ? "/tmp" : file_root; }
std::string RecoveryBackupRoot() { return backup_root; }
std::string RecoverySlot() { return active_slot; }
std::string RecoveryVersion() { return "R1.0"; }
std::string RecoveryBuildType() { return "Stable"; }
std::string RecoveryBuildStatus() { return "Unofficial"; }
std::string RecoveryDevice() { return "dodge"; }
std::string RecoveryBuildDate() { return "2026-09-13"; }
std::string RecoveryMaintainer() { return "Jonas Salo & Daniel Springer"; }
bool RecoverySetActiveSlot(const std::string &slot) {
  if (slot != "A" && slot != "B") return false;
  active_slot = slot;
  return true;
}
bool RecoveryDataLocked() { return false; }
std::vector<AndroidUser> RecoveryAndroidUsers() {
  return {{0, "Owner", 3, true}, {10, "Work profile", 2, false}};
}
bool RecoverySetStorage(const std::string &) { return true; }
int RecoveryBrightness() { return 50; }
void RecoverySetBrightness(int) {}
bool RecoveryMtpEnabled() { return false; }
bool RecoverySetMtp(bool) { return true; }
std::vector<Volume> RecoveryImageVolumes() {
  return {{"Recovery", "/recovery", 100663296},
          {"Boot", "/boot", 100663296}};
}
int recovery_progress = 38;
std::string installer_status;
int RecoveryProgress() { return recovery_progress; }
std::string RecoveryOperationDetail() { return "Backing up / Boot\n38MB of 100MB (38%)"; }
std::string RecoveryInstallerStatus() { return installer_status; }
SideloadStatus RecoverySideloadStatus() { return sideload_status; }
bool RecoveryCancelSideload() {
  sideload_status.cancel_requested = true;
  return true;
}
void RecoveryWifiInitialize() {}
WifiStatus RecoveryWifiStatus() {
  WifiStatus status;
  status.supported = true;
  status.enabled = true;
  status.state = "enabled";
  status.networks = {{"AERA Lab", "WPA2", false, false},
                     {"Saved network", "WPA3", true, false},
                     {"Guest", "OPEN", false, false}};
  return status;
}
int RecoveryRunWifi(const WifiRequest &request) { wifi_request = request; return 0; }
bool RecoveryWifiAutoEnable() { return wifi_auto_enable; }
bool RecoveryWifiAutoConnect() { return wifi_auto_connect; }
bool RecoverySetWifiAutoEnable(bool enabled) { wifi_auto_enable = enabled; return true; }
bool RecoverySetWifiAutoConnect(bool enabled) { wifi_auto_connect = enabled; return true; }
void SetPluginRequest(const plugins::Request &) {}
}
namespace aeraui::plugins {
bool IsPackageFile(const std::string &name) {
  return name.size() >= 6 && name.substr(name.size() - 6) == ".aerap";
}
bool InspectLocalPackage(const std::string &, Plugin &, std::string &error) {
  error = "Package inspection is unavailable in this host test.";
  return false;
}
}
static void Tick(int count=40) {
  for(int i=0;i<count;++i) { lv_tick_inc(16); lv_timer_handler(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
static lv_obj_t *Find(lv_obj_t *root,const char *text) {
  if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return lv_obj_get_parent(root);
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) if(auto *found=Find(lv_obj_get_child(root,i),text)) return found;
  return nullptr;
}
static lv_obj_t *FindLabel(lv_obj_t *root,const char *text) {
  if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return root;
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
    if(auto *found=FindLabel(lv_obj_get_child(root,i),text)) return found;
  return nullptr;
}
static lv_obj_t *FindLabelContaining(lv_obj_t *root,const char *text) {
  if (lv_obj_check_type(root, &lv_label_class) &&
      strstr(lv_label_get_text(root), text) != nullptr) return root;
  for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i)
    if (auto *found = FindLabelContaining(lv_obj_get_child(root, i), text)) return found;
  return nullptr;
}
static lv_obj_t *FindType(lv_obj_t *root, const lv_obj_class_t *type) {
  if(lv_obj_check_type(root,type)) return root;
  for(uint32_t i=0;i<lv_obj_get_child_count(root);++i)
    if(auto *found=FindType(lv_obj_get_child(root,i),type)) return found;
  return nullptr;
}
static void PressKeyboardKey(lv_obj_t *keyboard, const char *key) {
  const char *const *map = lv_buttonmatrix_get_map(keyboard);
  uint32_t button = 0;
  for (size_t index = 0; map[index][0] != '\0'; ++index) {
    if (!strcmp(map[index], "\n")) continue;
    if (!strcmp(map[index], key)) {
      lv_buttonmatrix_set_selected_button(keyboard, button);
      lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, nullptr);
      return;
    }
    ++button;
  }
  assert(false);
}
static lv_area_t Bounds(lv_obj_t *object) {
  lv_area_t area{};
  lv_obj_get_coords(object, &area);
  return area;
}
static bool Overlaps(lv_obj_t *left, lv_obj_t *right, int gap = 0) {
  const lv_area_t a = Bounds(left);
  const lv_area_t b = Bounds(right);
  return a.x1 < b.x2 + gap && a.x2 + gap > b.x1 &&
         a.y1 < b.y2 + gap && a.y2 + gap > b.y1;
}
static void AssertInside(lv_obj_t *object, int width, int height) {
  const lv_area_t area = Bounds(object);
  if (area.x1 < 0 || area.y1 < 0 || area.x2 >= width || area.y2 >= height)
    fprintf(stderr, "Object outside viewport: (%d,%d)-(%d,%d), viewport %dx%d\n",
            area.x1, area.y1, area.x2, area.y2, width, height);
  assert(area.x1 >= 0 && area.y1 >= 0);
  assert(area.x2 < width && area.y2 < height);
}
static void AssertContained(lv_obj_t *child, lv_obj_t *parent) {
  const lv_area_t inner = Bounds(child);
  const lv_area_t outer = Bounds(parent);
  assert(inner.x1 >= outer.x1 && inner.y1 >= outer.y1);
  assert(inner.x2 <= outer.x2 && inner.y2 <= outer.y2);
}
static void AssertCentered(lv_obj_t *child, lv_obj_t *parent) {
  const lv_area_t inner = Bounds(child);
  const lv_area_t outer = Bounds(parent);
  const int dx = (inner.x1 + inner.x2) - (outer.x1 + outer.x2);
  const int dy = (inner.y1 + inner.y2) - (outer.y1 + outer.y2);
  assert(dx >= 34 && dx <= 38);
  assert(dy >= -2 && dy <= 2);
}
static void AssertVisibleCaret(lv_obj_t *input) {
  assert(input && lv_obj_check_type(input, &lv_textarea_class));
  assert(lv_obj_get_style_border_width(input, LV_PART_CURSOR) >= 3);
  assert(lv_obj_get_style_border_opa(input, LV_PART_CURSOR) == LV_OPA_COVER);
  assert(lv_obj_get_style_border_side(input, LV_PART_CURSOR) == LV_BORDER_SIDE_LEFT);
  assert(lv_obj_get_style_anim_duration(input, LV_PART_CURSOR) > 0);
}
int main(int argc,char **argv) {
  assert(argc==2);
  char fixture_dir[]="/tmp/aera-restore-test-XXXXXX"; assert(mkdtemp(fixture_dir));
  backup_root=fixture_dir;
  file_root=backup_root+"/files";
  std::filesystem::create_directory(file_root);
  std::filesystem::create_directory(file_root+"/Folder");
  {
    std::ofstream(file_root+"/alpha.txt") << "alpha\n";
    std::ofstream(file_root+"/long-file-name-for-layout-validation.txt") << "layout\n";
    std::ofstream(file_root+"/package.zip") << "not a real package";
    std::ofstream log(file_root+"/broken.log", std::ios::binary);
    log << "plain\n\033[31mred\033[0m\ninvalid: ";
    const char invalid[] = {static_cast<char>(0xc3), '('};
    log.write(invalid, sizeof(invalid));
    std::ofstream(file_root+"/large.log", std::ios::binary)
        << std::string(160 * 1024, 'x');
  }
  std::filesystem::create_directory(backup_root+"/AERA-test-backup");
  lv_init();
  i18n::Initialize("en");
  const lv_font_t *latin_font = design::UiFont(&lv_font_montserrat_32);
  for (uint32_t codepoint : {0x00c4u, 0x00d6u, 0x00dcu, 0x00dfu,
                             0x00e4u, 0x00f6u, 0x00fcu}) {
    lv_font_glyph_dsc_t glyph{};
    assert(lv_font_get_glyph_dsc(latin_font, &glyph, codepoint, 0));
    assert(glyph.adv_w > 0);
  }
  std::vector<uint8_t> frame(1440*3168*4);
  auto *display=lv_display_create(1440,3168);
  lv_display_set_buffers(display,frame.data(),nullptr,frame.size(),LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display,[](lv_display_t *d,const lv_area_t *,uint8_t *){lv_display_flush_ready(d);});
  auto save=[&](const char *path) {
    Tick(); png_image image{}; image.version=PNG_IMAGE_VERSION;
    image.width=lv_display_get_horizontal_resolution(display);
    image.height=lv_display_get_vertical_resolution(display);
    image.format=PNG_FORMAT_BGRA;
    assert(png_image_write_to_file(&image,path,0,frame.data(),0,nullptr));
  };
  auto *screen=lv_screen_active();
  if (!strcmp(argv[1], "--about")) {
    auto *about=lv_obj_create(nullptr);
    BuildAboutScene(about,RecordAction,nullptr);
    lv_screen_load(about); Tick();
    assert(Find(about,"Jonas Salo · koaaN"));
    assert(Find(about,"Daniel Springer · Daniel210191"));
    assert(Find(about,"R1.0"));
    save("/tmp/aera-about-host.png");
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    return 0;
  }
  if (strcmp(argv[1], "--files")) {
  auto *wifi=lv_obj_create(nullptr); BuildWifiScene(wifi,RecordAction,nullptr);
  lv_screen_load(wifi); Tick();
  assert(Find(wifi,"AERA Lab"));
  lv_obj_send_event(Find(wifi,"AERA Lab"),LV_EVENT_CLICKED,nullptr);
  assert(FindType(wifi,&lv_keyboard_class));
  auto *wifi_password=FindType(wifi,&lv_textarea_class); assert(wifi_password);
  AssertVisibleCaret(wifi_password);
  lv_textarea_set_text(wifi_password,"correct horse battery staple");
  lv_obj_send_event(Find(wifi,"Connect"),LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kRunWifiOperation);
  assert(GetWifiRequest().ssid=="AERA Lab");
  assert(GetWifiRequest().password=="correct horse battery staple");
  lv_screen_load(screen); lv_obj_delete(wifi);
  auto *web = lv_obj_create(nullptr); BuildWebScene(web, RecordAction, nullptr, -1, -1, false);
  lv_screen_load(web); Tick();
  auto *web_input = FindType(web, &lv_textarea_class); assert(web_input);
  AssertVisibleCaret(web_input);
  auto *web_keyboard = FindType(web, &lv_keyboard_class); assert(web_keyboard);
  auto *web_navigation = lv_obj_get_parent(Find(web, "Menu"));
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(!lv_obj_has_flag(web_navigation, LV_OBJ_FLAG_HIDDEN));
  lv_obj_send_event(web_input, LV_EVENT_CLICKED, nullptr);
  assert(!lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(lv_obj_has_flag(web_navigation, LV_OBJ_FLAG_HIDDEN));
  Tick(); save("/tmp/aera-browser-keyboard-host.png");
  lv_textarea_set_text(web_input, "example.org");
  const int before_browser_go = callback_count;
  lv_obj_send_event(web_keyboard, LV_EVENT_READY, nullptr);
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(!lv_obj_has_flag(web_navigation, LV_OBJ_FLAG_HIDDEN));
  assert(!strcmp(lv_textarea_get_text(web_input), "https://example.org"));
  assert(Find(web, "Browsing unavailable"));
  assert(callback_count == before_browser_go); // Never starts a recovery job.
  assert(widgets::DismissModal(web));
  lv_obj_send_event(Find(web, "Verify and prepare engine"), LV_EVENT_CLICKED, nullptr);
  Tick(); assert(Find(web, "Runtime verified"));
  save("/tmp/aera-browser-host.png");
  lv_screen_load(screen); lv_obj_delete(web);
  int browser_channels[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, browser_channels) == 0);
  int browser_frame = memfd_create("aera-ui-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  assert(browser_frame >= 0 && ftruncate(browser_frame, web::kSharedBytes) == 0);
  auto *browser_pixels = static_cast<uint32_t *>(mmap(nullptr, web::kSharedBytes,
      PROT_READ | PROT_WRITE, MAP_SHARED, browser_frame, 0));
  assert(browser_pixels != MAP_FAILED);
  auto *browser_slot = browser_pixels +
      web::FrameSlot(1) * web::kWidth * web::kHeight;
  std::fill(browser_slot, browser_slot + web::kWidth * web::kHeight, 0xff38536b);
  assert(fcntl(browser_frame, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
  web = lv_obj_create(nullptr); BuildWebScene(web, RecordAction, nullptr,
      browser_frame, browser_channels[0], false);
  lv_screen_load(web);
  web::Message browser_message; browser_message.kind = web::Kind::kFrame;
  browser_message.sequence = 1; browser_message.x = web::kWidth; browser_message.y = web::kHeight;
  browser_message.value = web::kFrameBytes;
  assert(send(browser_channels[1], &browser_message, sizeof(browser_message), 0) == sizeof(browser_message));
  Tick();
  web::Message browser_ack;
  assert(recv(browser_channels[1], &browser_ack, sizeof(browser_ack), MSG_DONTWAIT) == sizeof(browser_ack));
  assert(browser_ack.kind == web::Kind::kAck && browser_ack.sequence == 1);
  assert(!lv_obj_is_visible(Find(web, "Web engine installed")));
  web::Message keyboard_message; keyboard_message.kind = web::Kind::kKeyboardShow;
  assert(send(browser_channels[1], &keyboard_message, sizeof(keyboard_message), 0) == sizeof(keyboard_message));
  Tick();
  web_keyboard = FindType(web, &lv_keyboard_class); assert(web_keyboard);
  web_navigation = lv_obj_get_parent(Find(web, "Menu"));
  assert(!lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(lv_obj_has_flag(web_navigation, LV_OBJ_FLAG_HIDDEN));
  keyboard_message = web::Message{}; keyboard_message.kind = web::Kind::kKeyboardHide;
  assert(send(browser_channels[1], &keyboard_message, sizeof(keyboard_message), 0) == sizeof(keyboard_message));
  Tick();
  assert(lv_obj_has_flag(web_keyboard, LV_OBJ_FLAG_HIDDEN));
  assert(!lv_obj_has_flag(web_navigation, LV_OBJ_FLAG_HIDDEN));
  web_input = FindType(web, &lv_textarea_class);
  lv_textarea_set_text(web_input, "example.org");
  lv_obj_send_event(Find(web, "Go"), LV_EVENT_CLICKED, nullptr);
  assert(recv(browser_channels[1], &browser_ack, sizeof(browser_ack), MSG_DONTWAIT) == sizeof(browser_ack));
  assert(browser_ack.kind == web::Kind::kOpen && !strcmp(browser_ack.text, "https://example.org"));
  save("/tmp/aera-browser-viewport-host.png");
  close(browser_channels[1]); Tick(); assert(Find(web, "Browser stopped"));
  lv_screen_load(screen); lv_obj_delete(web);
  munmap(browser_pixels, web::kSharedBytes);
  JobRequest format_request; format_request.job=Job::kFormatData; format_request.path="/data";
  for(const char *invalid : {"", "YES", "yes ", "yesplease", " yes", "yes\n"}) {
    format_request.confirmation=invalid; assert(!FormatDataAuthorized(format_request));
  }
  format_request.confirmation="yes"; assert(FormatDataAuthorized(format_request));
  format_request.path="/metadata"; assert(!FormatDataAuthorized(format_request));
  format_request.path="/data"; format_request.partitions={"/data"}; assert(!FormatDataAuthorized(format_request));
  format_request.partitions.clear(); format_request.job=Job::kWipe; assert(!FormatDataAuthorized(format_request));
  auto *wipe=lv_obj_create(nullptr); BuildToolScene(wipe,Action::kWipe,RecordAction,nullptr);
  lv_screen_load(wipe); Tick();
  assert(Find(wipe,"Format Data"));
  auto *wipe_review=Find(wipe,"Review wipe"); assert(wipe_review && lv_obj_has_state(wipe_review,LV_STATE_DISABLED));
  auto *wipe_data=Find(wipe,"Data"); assert(wipe_data); lv_obj_send_event(wipe_data,LV_EVENT_CLICKED,nullptr);
  assert(!lv_obj_has_state(wipe_review,LV_STATE_DISABLED));
  lv_area_t bar_area{};
  lv_obj_get_coords(lv_obj_get_parent(Find(wipe,"Home")),&bar_area);
  assert(bar_area.y2==3167);
  save("/tmp/aera-wipe-host.png");
  lv_obj_send_event(Find(wipe,"Format Data"),LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kFormatData);
  lv_screen_load(screen); lv_obj_delete(wipe);
  auto *format=lv_obj_create(nullptr); BuildToolScene(format,Action::kFormatData,RecordAction,nullptr);
  lv_screen_load(format); Tick();
  auto *input=FindType(format,&lv_textarea_class), *submit=Find(format,"Format data now");
  assert(input && submit && lv_obj_has_state(submit,LV_STATE_DISABLED));
  callback_count=0;
  for(const char *invalid : {"", "YES", "yes ", "yesplease", " yes"}) {
    lv_textarea_set_text(input,invalid);
    assert(lv_obj_has_state(submit,LV_STATE_DISABLED));
    lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr); assert(callback_count==0);
  }
  lv_textarea_set_text(input,"yes");
  assert(!lv_obj_has_state(submit,LV_STATE_DISABLED) && callback_count==0);
  lv_obj_send_event(FindType(format,&lv_keyboard_class),LV_EVENT_READY,nullptr);
  assert(callback_count==0);
  save("/tmp/aera-format-host.png");
  lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr);
  assert(callback_count==1 && last_action==Action::kRunOperation);
  assert(FormatDataAuthorized(GetJobRequest()));
  assert(lv_obj_has_state(submit,LV_STATE_DISABLED) && !strcmp(lv_textarea_get_text(input),""));
  lv_obj_send_event(submit,LV_EVENT_CLICKED,nullptr); assert(callback_count==1);
  lv_obj_send_event(Find(format,"Cancel"),LV_EVENT_CLICKED,nullptr); assert(last_action==Action::kWipe);
  lv_screen_load(screen); lv_obj_delete(format);
  auto *prefs=lv_obj_create(nullptr); BuildToolScene(prefs,Action::kPreferences,RecordAction,nullptr);
  lv_screen_load(prefs); Tick();
  auto *clock_row=Find(prefs,"24-hour clock");
  assert(clock_row && lv_obj_get_width(clock_row)>=1200);
  for(const auto &item : std::vector<std::pair<const char*,Preference>>{
      {"24-hour clock",Preference::kClock24},{"Gesture navigation",Preference::kRecents},
      {"Show hidden files",Preference::kHiddenFiles},
      {"Verify ZIP signatures",Preference::kVerifyZip},{"Compress backups by default",Preference::kCompression},
      {"SHA-256 backup checksums",Preference::kSha256}}) {
    const bool old=RecoveryPreference(item.second);
    auto *toggle=Find(prefs,item.first); assert(toggle);
    lv_obj_send_event(toggle,LV_EVENT_CLICKED,nullptr); assert(RecoveryPreference(item.second)!=old);
    lv_obj_send_event(toggle,LV_EVENT_CLICKED,nullptr); assert(RecoveryPreference(item.second)==old);
  }
  save("/tmp/aera-preferences-host.png");
  lv_obj_send_event(Find(prefs,LV_SYMBOL_PLUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==135);
  utc_offset=840; lv_obj_send_event(Find(prefs,LV_SYMBOL_PLUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==840);
  utc_offset=-720; lv_obj_send_event(Find(prefs,LV_SYMBOL_MINUS),LV_EVENT_CLICKED,nullptr); assert(utc_offset==-720);
  lv_obj_send_event(Find(prefs,"Save preferences"),LV_EVENT_CLICKED,nullptr);
  assert(Find(prefs,"Preferences saved")); assert(widgets::DismissModal(prefs)); Tick();
  save_succeeds=false;
  lv_obj_send_event(Find(prefs,"Save preferences"),LV_EVENT_CLICKED,nullptr);
  assert(Find(prefs,"Could not save preferences")); assert(widgets::DismissModal(prefs)); Tick();
  lv_screen_load(screen); lv_obj_delete(prefs);
  auto *users=lv_obj_create(nullptr);
  BuildToolScene(users,Action::kUsers,RecordAction,nullptr);
  lv_screen_load(users); Tick();
  assert(Find(users,"Owner") && Find(users,"Unlocked"));
  auto *work_user=Find(users,"Work profile"); assert(work_user);
  lv_obj_send_event(work_user,LV_EVENT_CLICKED,nullptr);
  assert(last_action==Action::kDecryptUser);
  const auto user_request=GetUserDecryptRequest();
  assert(user_request.user.id==10 && user_request.user.credential_type==2);
  lv_screen_load(screen); lv_obj_delete(users);
  BuildToolScene(screen,Action::kBackup,[](Action,void*){},nullptr);
  assert(!lv_obj_get_style_bg_image_tiled(screen,LV_PART_MAIN));
  assert(lv_obj_get_style_bg_grad_dir(screen,LV_PART_MAIN)==LV_GRAD_DIR_NONE);
  const auto *grain=static_cast<const lv_image_dsc_t *>(lv_obj_get_style_bg_image_src(screen,LV_PART_MAIN));
  assert(grain && grain->header.w==1440 && grain->header.h==3168);
  assert(grain->header.cf==LV_COLOR_FORMAT_XRGB8888 && grain->header.stride==1440*4);
  assert(memcmp(grain->data,grain->data+192*4,192*4)!=0);
  assert(memcmp(grain->data,grain->data+192*grain->header.stride,192*grain->header.stride)!=0);
  Tick();
  auto *row=Find(screen,"Boot"); assert(row); lv_obj_send_event(row,LV_EVENT_CLICKED,nullptr);
  auto *review=Find(screen,"Review backup"); assert(review && !lv_obj_has_state(review,LV_STATE_DISABLED));
  save("/tmp/aera-matte-backup-host.png");
  auto *restore=lv_obj_create(nullptr); BuildToolScene(restore,Action::kRestore,[](Action,void*){},nullptr);
  lv_screen_load(restore); Tick();
  auto *restore_review=Find(restore,"Review restore"); assert(restore_review && lv_obj_has_state(restore_review,LV_STATE_DISABLED));
  auto *folder=Find(restore,"AERA-test-backup"); assert(folder); lv_obj_send_event(folder,LV_EVENT_CLICKED,nullptr); Tick();
  auto *restore_boot=Find(restore,"Boot"); assert(restore_boot); lv_obj_send_event(restore_boot,LV_EVENT_CLICKED,nullptr);
  assert(!lv_obj_has_state(restore_review,LV_STATE_DISABLED));
  save("/tmp/aera-restore-host.png"); lv_screen_load(screen); lv_obj_delete(restore);
  auto *unlock=lv_obj_create(nullptr); BuildDecryptScene(unlock,3,true,0,3,[](Action,void*){},nullptr);
  lv_screen_load(unlock); save("/tmp/aera-matte-unlock-host.png"); lv_screen_load(screen); lv_obj_delete(unlock);
  for(int i=0;i<8;++i) {
    OpenPicture(screen,argv[1]);
    Tick(5);
    if(i%2) Tick(500); // Alternate immediate cancellation and completed decode.
    auto *plus=Find(screen,LV_SYMBOL_PLUS);
    if (!plus) {
      assert(widgets::DismissModal(screen));
      Tick(20);
      continue;
    }
    lv_obj_send_event(plus,LV_EVENT_CLICKED,nullptr); Tick(5);
    if(i==7) save("/tmp/aera-picture-host.png");
    assert(widgets::DismissModal(screen)); Tick(20);
  }
  auto *job=lv_obj_create(nullptr);
  JobRequest request; request.job=Job::kBackup; request.title="Backup"; request.path="/tmp/test-backup";
  auto operation=BuildJobScene(job,request,[](Action,void*){},nullptr);
  lv_screen_load(job); RefreshOperationScene(operation); save("/tmp/aera-operation-host.png");
  CompleteOperationScene(operation,true,"Backup completed"); Tick();
  lv_screen_load(screen); lv_obj_delete(job);
  recovery_progress=0;
  installer_status="- Preparing recovery update...\n- Installing AERA to slot _a...";
  auto *install_job=lv_obj_create(nullptr);
  JobRequest install_request; install_request.job=Job::kInstall; install_request.title="Install AERA Update";
  auto installing=BuildJobScene(install_job,install_request,[](Action,void*){},nullptr);
  lv_screen_load(install_job); RefreshOperationScene(installing); Tick();
  assert(Find(install_job,"Installing"));
  assert(Find(install_job,"Installing AERA to slot _a..."));
  assert(Find(install_job,"Preparing recovery update...\nInstalling AERA to slot _a..."));
  lv_screen_load(screen); lv_obj_delete(install_job);
  recovery_progress=38;
  installer_status.clear();
  auto *format_job=lv_obj_create(nullptr);
  format_request.job=Job::kFormatData;
  auto formatting=BuildJobScene(format_job,format_request,RecordAction,nullptr);
  lv_screen_load(format_job); Tick();
  assert(lv_obj_has_flag(formatting.done,LV_OBJ_FLAG_HIDDEN));
  CompleteOperationScene(formatting,true,"Format completed"); Tick();
  assert(Find(format_job,"Data was formatted successfully. Reboot recovery before using /data again."));
  auto *reboot_options=Find(format_job,"Reboot options"); assert(reboot_options);
  lv_obj_send_event(reboot_options,LV_EVENT_CLICKED,nullptr); assert(last_action==Action::kOpenReboot);
  lv_screen_load(screen); lv_obj_delete(format_job);
  auto *sideload=lv_obj_create(nullptr);
  BuildSideloadScene(sideload,RecordAction,nullptr);
  lv_screen_load(sideload); Tick();
  assert(Find(sideload,"Start ADB sideload"));
  assert(Find(sideload,"adb sideload package.zip"));
  assert(Find(sideload,"No partition or persistent file will be modified.")==nullptr);
  lv_screen_load(screen); lv_obj_delete(sideload);
  sideload_status={true,false,32ULL*1024*1024,64ULL*1024*1024};
  installer_status="Reading test package";
  auto *sideload_job=lv_obj_create(nullptr);
  JobRequest sideload_request; sideload_request.job=Job::kSideload;
  sideload_request.title="ADB Sideload";
  auto receiving=BuildJobScene(sideload_job,sideload_request,RecordAction,nullptr);
  lv_screen_load(sideload_job); RefreshOperationScene(receiving); Tick();
  assert(Find(sideload_job,"50%"));
  assert(Find(sideload_job,"32.0 MB / 64.0 MB"));
  assert(Find(sideload_job,"Reading test package"));
  lv_screen_load(screen); lv_obj_delete(sideload_job);
  installer_status.clear(); sideload_status={};
  }

  const std::vector<std::pair<InterfaceSize, const char*>> file_sizes = {
      {InterfaceSize::kSmall, "small"},
      {InterfaceSize::kNormal, "normal"},
      {InterfaceSize::kLarge, "large"},
  };
  for (bool landscape : {false, true}) {
    lv_display_set_resolution(display, landscape ? 3168 : 1440,
                              landscape ? 1440 : 3168);
    for (const auto& [size, size_name] : file_sizes) {
      interface_size = size;
      design::ApplyInterfaceSize(static_cast<int>(size));
      auto* files = lv_obj_create(nullptr);
      BuildFilesScene(files, RecordAction, nullptr);
      lv_screen_load(files);
      Tick();
      assert(Find(files, "alpha.txt"));
      assert(Find(files, "long-file-name-for-layout-validation.txt"));
      auto* alpha_row = Find(files, "alpha.txt");
      assert(lv_obj_get_height(alpha_row) ==
             (size == InterfaceSize::kSmall ? 150
              : size == InterfaceSize::kLarge ? 228
                                               : 174));
      assert(Find(files, LV_SYMBOL_PLUS));
      for (const char* symbol : {LV_SYMBOL_LIST, LV_SYMBOL_PLUS,
                                 LV_SYMBOL_OK, LV_SYMBOL_REFRESH}) {
        auto* button = Find(files, symbol);
        assert(button && lv_obj_get_child_count(button) != 0);
        AssertCentered(lv_obj_get_child(button, 0), button);
      }
      char screenshot[128];
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-files-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);

      if (!landscape && size == InterfaceSize::kLarge) {
        lv_obj_send_event(Find(files, LV_SYMBOL_OK), LV_EVENT_CLICKED, nullptr);
        Tick();
        auto* copy = Find(files, "Copy");
        assert(copy && lv_obj_get_height(copy) == 180);
        save("/tmp/aera-file-selection-portrait-large.png");
        lv_obj_send_event(Find(files, LV_SYMBOL_CLOSE), LV_EVENT_CLICKED, nullptr);
        Tick();
      }

      if (!landscape && size == InterfaceSize::kNormal) {
        auto* row = Find(files, "alpha.txt");
        lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Information"));
        assert(Find(files, "Delete"));
        save("/tmp/aera-file-actions-portrait-normal.png");
        auto* action_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        lv_obj_send_event(action_overlay, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(!Find(files, "Information"));

        row = Find(files, "alpha.txt");
        lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        Tick();
        lv_obj_send_event(Find(files, "Information"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Information"));
        save("/tmp/aera-file-information-portrait-normal.png");
        auto* information_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        lv_obj_send_event(information_overlay, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(!Find(files, "Information"));

        lv_obj_send_event(Find(files, "broken.log"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "plain\nred\ninvalid: ?("));
        assert(widgets::DismissModal(files));
        Tick();

        lv_obj_send_event(Find(files, "large.log"), LV_EVENT_CLICKED, nullptr);
        Tick();
        auto* viewer_overlay = lv_obj_get_child(
            files, static_cast<int32_t>(lv_obj_get_child_count(files)) - 1);
        assert(FindLabelContaining(viewer_overlay, "Preview truncated at 128 KB"));
        auto* large_page = FindLabelContaining(viewer_overlay, "xxxxxxxxxxxxxxxx");
        assert(large_page && lv_obj_get_height(large_page) < 8000);
        auto* next_page = Find(viewer_overlay, LV_SYMBOL_RIGHT);
        assert(next_page);
        lv_obj_send_event(next_page, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(FindLabelContaining(viewer_overlay, "2 / "));
        assert(widgets::DismissModal(files));
        Tick();

        auto* select_button = Find(files, LV_SYMBOL_OK);
        assert(select_button);
        lv_obj_send_event(select_button, LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "0 selected"));
        lv_obj_send_event(Find(files, "alpha.txt"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "1 selected"));
        assert(Find(files, "Copy") && Find(files, "Cut") && Find(files, "Delete"));
        assert(lv_obj_get_height(Find(files, "Copy")) == 146);
        save("/tmp/aera-file-selection-portrait-normal.png");
        lv_obj_send_event(Find(files, "Copy"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Paste here"));
        save("/tmp/aera-file-clipboard-portrait-normal.png");
        lv_obj_send_event(Find(files, "Paste here"), LV_EVENT_CLICKED, nullptr);
        Tick();
        assert(Find(files, "Replace") && Find(files, "Keep both") && Find(files, "Skip"));
        save("/tmp/aera-file-conflict-portrait-normal.png");
        lv_obj_send_event(Find(files, "Keep both"), LV_EVENT_CLICKED, nullptr);
        Tick(200);
        auto* paste_after_work = Find(files, "Paste here");
        assert(paste_after_work);
        assert(lv_obj_has_flag(lv_obj_get_parent(paste_after_work), LV_OBJ_FLAG_HIDDEN));
        assert(Find(files, "alpha (1).txt"));
      }

      lv_obj_send_event(Find(files, LV_SYMBOL_PLUS), LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* new_text = Find(files, "New text file");
      assert(new_text);
      lv_obj_send_event(new_text, LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* name_input = FindType(files, &lv_textarea_class);
      auto* name_keyboard = FindType(files, &lv_keyboard_class);
      auto* name_cancel = Find(files, "Cancel");
      auto* name_create = Find(files, "Create");
      assert(name_input && name_keyboard && name_cancel && name_create);
      AssertVisibleCaret(name_input);
      save("/tmp/aera-file-name-debug.png");
      AssertInside(name_input, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_keyboard, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_cancel, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(name_create, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      assert(!Overlaps(name_input, name_keyboard, 16));
      assert(!Overlaps(name_keyboard, name_cancel, 16));
      assert(!Overlaps(name_keyboard, name_create, 16));
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-name-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);

      const std::string filename = std::string("ui-") +
          (landscape ? "landscape-" : "portrait-") + size_name + ".txt";
      lv_textarea_set_text(name_input, filename.c_str());
      if (!landscape && size == InterfaceSize::kNormal)
        lv_obj_send_event(name_keyboard, LV_EVENT_READY, nullptr);
      else
        lv_obj_send_event(name_create, LV_EVENT_CLICKED, nullptr);
      Tick();
      auto* editor = FindType(files, &lv_textarea_class);
      auto* editor_keyboard = FindType(files, &lv_keyboard_class);
      auto* editor_cancel = Find(files, "Cancel");
      auto* editor_save = Find(files, "Save");
      assert(editor && editor_keyboard && editor_cancel && editor_save);
      AssertVisibleCaret(editor);
      AssertInside(editor, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_keyboard, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_cancel, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      AssertInside(editor_save, landscape ? 3168 : 1440, landscape ? 1440 : 3168);
      assert(!Overlaps(editor, editor_keyboard, 16));
      assert(!Overlaps(editor_keyboard, editor_cancel, 16));
      assert(!Overlaps(editor_keyboard, editor_save, 16));
      lv_textarea_set_text(editor, "First line");
      lv_textarea_set_cursor_pos(editor, LV_TEXTAREA_CURSOR_LAST);
      PressKeyboardKey(editor_keyboard, LV_SYMBOL_NEW_LINE);
      assert(!strcmp(lv_textarea_get_text(editor), "First line\n"));
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-editor-%s-%s.png",
               landscape ? "landscape" : "portrait", size_name);
      save(screenshot);
      lv_textarea_set_text(editor, "Created by the AERA file-manager UI check.\n");
      lv_obj_send_event(editor_save, LV_EVENT_CLICKED, nullptr);
      Tick();
      std::ifstream created(file_root + "/" + filename);
      std::string contents((std::istreambuf_iterator<char>(created)),
                           std::istreambuf_iterator<char>());
      assert(contents == "Created by the AERA file-manager UI check.\n");
      lv_screen_load(screen);
      lv_obj_delete(files);
      Tick();
    }
  }
  lv_display_set_resolution(display, 1440, 3168);
  interface_size = InterfaceSize::kLarge;
  for (const auto& language : i18n::AvailableLanguages()) {
    assert(i18n::SetLanguage(language.code));
    auto* files = lv_obj_create(nullptr);
    BuildFilesScene(files, RecordAction, nullptr);
    lv_screen_load(files);
    Tick();
    if (!language.rtl) {
      assert(Find(files, i18n::Translate("Files")));
    }
    if (!strcmp(language.code, "ar_SA"))
      save("/tmp/aera-file-language-ar_SA.png");

    auto* row = Find(files, "alpha.txt");
    assert(row);
    lv_obj_send_event(row, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(row, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    Tick();
    if (!language.rtl) {
      assert(Find(files, i18n::Translate("Copy")));
      assert(Find(files, i18n::Translate("Information")));
      assert(Find(files, i18n::Translate("Delete")));
    }
    assert(widgets::DismissModal(files));
    Tick();

    if (language.rtl) {
      lv_screen_load(screen);
      lv_obj_delete(files);
      Tick();
      continue;
    }

    lv_obj_send_event(Find(files, LV_SYMBOL_PLUS), LV_EVENT_CLICKED, nullptr);
    Tick();
    auto* create_text = Find(files, i18n::Translate("New text file"));
    assert(create_text);
    lv_obj_send_event(create_text, LV_EVENT_CLICKED, nullptr);
    Tick();
    auto* keyboard = FindType(files, &lv_keyboard_class);
    auto* cancel = Find(files, i18n::Translate("Cancel"));
    auto* create = Find(files, i18n::Translate("Create"));
    assert(keyboard && cancel && create);
    AssertInside(keyboard, 1440, 3168);
    assert(!Overlaps(keyboard, cancel, 16));
    assert(!Overlaps(keyboard, create, 16));
    auto* cancel_label = FindLabel(cancel, i18n::Translate("Cancel"));
    auto* create_label = FindLabel(create, i18n::Translate("Create"));
    assert(cancel_label && create_label);
    AssertContained(cancel_label, cancel);
    AssertContained(create_label, create);
    if (!strcmp(language.code, "de_DE") || !strcmp(language.code, "sv_SE") ||
        !strcmp(language.code, "zh_CN") ||
        !strcmp(language.code, "ru")) {
      char screenshot[128];
      snprintf(screenshot, sizeof(screenshot), "/tmp/aera-file-language-%s.png",
               language.code);
      save(screenshot);
    }
    assert(widgets::DismissModal(files));
    Tick();
    lv_screen_load(screen);
    lv_obj_delete(files);
    Tick();
  }
  i18n::SetLanguage("en");
  interface_size = InterfaceSize::kNormal;

  if (!strcmp(argv[1], "--files")) {
    puts("Headless File Manager UI checks passed in portrait and landscape at Small, Normal and Large interface sizes.");
    lv_deinit();
    std::filesystem::remove_all(backup_root);
    return 0;
  }

  auto *about=lv_obj_create(nullptr);
  BuildAboutScene(about,RecordAction,nullptr);
  lv_screen_load(about); Tick();
  assert(Find(about,"Jonas Salo · koaaN"));
  assert(Find(about,"Daniel Springer · Daniel210191"));
  assert(Find(about,"R1.0"));
  save("/tmp/aera-about-host.png");
  lv_screen_load(screen); lv_obj_delete(about);
  puts("Headless UI checks passed: file-manager long press, creation, editor and keyboard geometry in six viewport/size combinations; wipe selection, format confirmation guard, keyboard no auto-submit, bottom navigation geometry, preference toggles, backup/restore, Android users, unlock, viewer, operation completion.");
  lv_deinit();
  std::filesystem::remove_all(backup_root);
}
