// SPDX-License-Identifier: Apache-2.0
#include "scene.hpp"
#include "browser/runtime.hpp"
#include "phone_keyboard.hpp"
#include "telegram/launcher.hpp"
#include "telegram/protocol.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

struct TelegramScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *surface = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *detail = nullptr;
  lv_obj_t *progress = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *navigation = nullptr;
  lv_obj_t *keyboard = nullptr;
  lv_obj_t *composer = nullptr;
  lv_obj_t *send_button = nullptr;
  lv_obj_t *attach_button = nullptr;
  lv_timer_t *timer = nullptr;
  telegram::Process process;
  web::Preparation preparation;
  std::thread worker;
  int control = -1;
  telegram::AuthState auth = telegram::AuthState::kStarting;
  int64_t chat_id = 0;
  int item_y = 0;
  bool launched = false;
  bool existing_vault = false;
  bool keyboard_visible = false;
  bool history_loading = false;
  bool landscape = false;
  ActionCallback callback = nullptr;
  void *context = nullptr;

  ~TelegramScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    Send(telegram::Kind::kClose);
    if (control >= 0) close(control);
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }

  bool Send(telegram::Kind kind, const std::string &text = {},
            int64_t primary = 0) {
    if (control < 0 || text.size() >= telegram::kTextBytes) return false;
    telegram::Message message;
    message.kind = kind;
    message.primary = primary;
    memcpy(message.text, text.data(), text.size());
    const ssize_t count = send(control, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_NOSIGNAL);
    return count == static_cast<ssize_t>(sizeof(message));
  }
};

lv_obj_t *Input(lv_obj_t *parent, int y, const char *placeholder,
                bool password = false) {
  auto *input = lv_textarea_create(parent);
  lv_obj_set_align(input, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(input, 42, y);
  lv_textarea_set_one_line(input, true);
  // set_one_line() selects a content-height widget in LVGL. Apply the intended
  // touch height afterwards so the field cannot collapse into a thin line.
  lv_obj_set_size(input, 1228, 132);
  lv_textarea_set_placeholder_text(input, placeholder);
  lv_textarea_set_password_mode(input, password);
  lv_textarea_set_password_show_time(input, 0);
  lv_textarea_set_max_length(input, 512);
  lv_obj_set_style_text_font(input, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(input, kText, 0);
  lv_obj_set_style_text_color(input, kMutedStrong,
                              LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(input, kMainBottom, 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, kLineBright, 0);
  lv_obj_set_style_border_color(input, kCyan, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(input, 2, 0);
  lv_obj_set_style_border_width(input, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 28, 0);
  lv_obj_set_style_pad_all(input, 34, 0);
  return input;
}

void StyleKeyboard(lv_obj_t *keyboard) {
  phone_keyboard::Apply(keyboard);
  lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_size(keyboard, 1228, 850);
  lv_obj_set_style_bg_color(keyboard, kMainBottom, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(keyboard, kMainLine, LV_PART_MAIN);
  lv_obj_set_style_radius(keyboard, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_row(keyboard, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_column(keyboard, 8, LV_PART_MAIN);
  lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_48, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, kText, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, kMainPanel, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, kCyan,
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(keyboard, kLineBright, LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 18, LV_PART_ITEMS);
}

void ShowStatus(TelegramScene *scene, const char *title, const char *detail) {
  lv_obj_clean(scene->surface);
  scene->title = Label(scene->surface, title, &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, 1180);
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 170);
  scene->detail = Label(scene->surface, detail, &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_width(scene->detail, 1080);
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(scene->detail, 14, 0);
  lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 280);
  scene->progress = lv_bar_create(scene->surface);
  lv_obj_set_size(scene->progress, 920, 16);
  lv_obj_align(scene->progress, LV_ALIGN_TOP_MID, 0, 470);
  lv_bar_set_value(scene->progress, scene->preparation.progress.load(), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(scene->progress, kCyan, LV_PART_INDICATOR);
}

void BindKeyboard(lv_obj_t *input, lv_obj_t *keyboard) {
  lv_obj_add_event_cb(input, [](lv_event_t *event) {
    auto *keyboard = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_FOCUSED)
      lv_keyboard_set_textarea(keyboard, lv_event_get_target_obj(event));
  }, LV_EVENT_FOCUSED, keyboard);
}

lv_obj_t *PasswordToggle(lv_obj_t *parent, lv_obj_t *input, int y) {
  auto *toggle = lv_button_create(parent);
  Clear(toggle);
  lv_obj_set_align(toggle, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(toggle, 1102, y);
  lv_obj_set_size(toggle, 168, 132);
  lv_obj_set_style_radius(toggle, 28, 0);
  lv_obj_set_style_bg_color(toggle, kMainBottom, 0);
  lv_obj_set_style_bg_color(toggle, kMainSelected, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(toggle, 2, 0);
  lv_obj_set_style_border_color(toggle, kLineBright, 0);
  auto *icon = Label(toggle, LV_SYMBOL_EYE_OPEN,
                     &lv_font_montserrat_32, kMutedStrong);
  lv_obj_center(icon);
  OnClick(toggle, [input, icon] {
    const bool hidden = lv_textarea_get_password_mode(input);
    lv_textarea_set_password_mode(input, !hidden);
    lv_label_set_text(icon, hidden ? LV_SYMBOL_EYE_CLOSE
                                   : LV_SYMBOL_EYE_OPEN);
  });
  return toggle;
}

void ShowAuth(TelegramScene *scene, telegram::AuthState state,
              const std::string &detail) {
  scene->auth = state;
  lv_obj_clean(scene->surface);
  scene->title = nullptr;
  scene->detail = nullptr;
  scene->progress = nullptr;
  scene->list = nullptr;
  const bool configure = state == telegram::AuthState::kNeedConfiguration;
  const bool vault = state == telegram::AuthState::kNeedVault;
  const bool new_vault = vault && !scene->existing_vault;
  const char *title = configure ? "Set up AERA Telegram" :
      new_vault ? "Create AERA Vault" :
      vault ? "Unlock AERA Telegram" :
      state == telegram::AuthState::kNeedPhone ? "Your phone number" :
      state == telegram::AuthState::kNeedCode ? "Telegram code" :
      state == telegram::AuthState::kNeedPassword ? "Two-step verification" :
      state == telegram::AuthState::kNeedEmail ? "Recovery email" :
      state == telegram::AuthState::kNeedEmailCode ? "Email code" :
      state == telegram::AuthState::kNeedRegistration ? "Create account" :
      state == telegram::AuthState::kConfirmElsewhere ? "Confirm sign-in" :
      "Connecting";
  auto *logo = IconPlate(scene->surface, LV_SYMBOL_ENVELOPE, kCanvas, kCyan, 126);
  lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 70);
  auto *badge = Kicker(scene->surface,
      configure ? "ONE-TIME CLIENT SETUP" :
      new_vault ? "FIRST-TIME SECURITY" :
      vault ? "ENCRYPTED LOCAL SESSION" : "TELEGRAM SIGN IN",
      configure ? kAccent : kCyan);
  lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 222);
  scene->title = Label(scene->surface, title, &lv_font_montserrat_48, kText);
  lv_obj_set_width(scene->title, 1180);
  lv_obj_set_style_text_align(scene->title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->title, LV_ALIGN_TOP_MID, 0, 300);
  const std::string shown_detail = new_vault
      ? "Choose a password to protect Telegram data stored by this recovery."
      : vault
          ? "Enter the vault password you created on this device."
          : detail;
  scene->detail = Label(scene->surface, shown_detail.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_width(scene->detail, 1120);
  lv_obj_set_style_text_align(scene->detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(scene->detail, LV_ALIGN_TOP_MID, 0, 382);

  if (state == telegram::AuthState::kConfirmElsewhere) {
    auto *note = Label(scene->surface,
        "Approve this login from an existing Telegram device, then wait here.",
        &lv_font_montserrat_32, kMutedStrong);
    lv_obj_set_pos(note, 92, 560); lv_obj_set_width(note, 1128);
    return;
  }
  if (state == telegram::AuthState::kStarting ||
      state == telegram::AuthState::kClosing) return;

  if (vault) {
    auto *hint = Label(scene->surface,
        new_vault
            ? "This is not your Telegram account password. Use at least 8 characters and keep it safe."
            : "Your password never leaves this device.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(hint, 62, 480);
    lv_obj_set_width(hint, 1188);

    auto *password_label = Label(scene->surface, "Vault password",
                                 &lv_font_montserrat_24, kText);
    lv_obj_set_pos(password_label, 62, 550);
    auto *first = Input(scene->surface, 600,
                        new_vault ? "Create a password" : "Enter your password",
                        true);
    lv_obj_set_width(first, 1044);
    PasswordToggle(scene->surface, first, 600);

    lv_obj_t *confirmation = nullptr;
    if (new_vault) {
      auto *confirm_label = Label(scene->surface, "Confirm password",
                                  &lv_font_montserrat_24, kText);
      lv_obj_set_pos(confirm_label, 62, 758);
      confirmation = Input(scene->surface, 808, "Type it again", true);
      lv_obj_set_width(confirmation, 1044);
      PasswordToggle(scene->surface, confirmation, 808);
    }

    auto *keyboard = lv_keyboard_create(scene->surface);
    StyleKeyboard(keyboard);
    const int keyboard_y = scene->landscape ? 260 : new_vault ? 988 : 800;
    lv_obj_set_pos(keyboard, scene->landscape ? 1600 : 42, keyboard_y);
    lv_obj_set_size(keyboard, scene->landscape ? 1500 : 1228,
                    scene->landscape ? 850 : 690);
    BindKeyboard(first, keyboard);
    if (confirmation) BindKeyboard(confirmation, keyboard);
    lv_keyboard_set_textarea(keyboard, first);
    lv_obj_add_state(first, LV_STATE_FOCUSED);

    auto *submit = Button(scene->surface,
        new_vault ? "Create vault and continue" : "Unlock and continue",
        [scene, first, confirmation, new_vault] {
          const std::string value = lv_textarea_get_text(first);
          if (value.size() < 8) {
            lv_label_set_text(scene->detail,
                              "Use at least 8 characters for the vault password.");
            return;
          }
          if (new_vault &&
              value != std::string(lv_textarea_get_text(confirmation))) {
            lv_label_set_text(scene->detail,
                              "The two passwords do not match. Please try again.");
            return;
          }
          scene->Send(telegram::Kind::kConfigure, value, 0);
          lv_textarea_set_text(first, "");
          if (confirmation) lv_textarea_set_text(confirmation, "");
          ShowStatus(scene, "Opening encrypted session",
                     new_vault
                         ? "Creating your private AERA Telegram database."
                         : "Unlocking your local messages and account state.");
        }, true);
    lv_obj_set_pos(submit, scene->landscape ? 62 : 42,
                   scene->landscape ? 1050 : keyboard_y + 740);
    lv_obj_set_size(submit, scene->landscape ? 1208 : 1228, 132);
    return;
  }

  if (configure) {
    auto *explanation = lv_obj_create(scene->surface);
    Panel(explanation, 28, kMainSheet);
    lv_obj_set_pos(explanation, 42, 500);
    lv_obj_set_size(explanation, 1228, 150);
    auto *info = Label(explanation,
        "This is not your account login. Telegram requires every independent client to have its own API ID and hash from my.telegram.org. You enter these only once.",
        &lv_font_montserrat_24, kMutedStrong);
    lv_obj_set_pos(info, 28, 22); lv_obj_set_width(info, 1172);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
  }

  const int first_y = configure ? 690 : 550;
  auto *first = Input(scene->surface, first_y,
      configure ? "Telegram API ID" :
      state == telegram::AuthState::kNeedPhone ? "+46 70 123 45 67" :
      state == telegram::AuthState::kNeedCode ? "Login code" :
      state == telegram::AuthState::kNeedPassword ? "Telegram password" :
      state == telegram::AuthState::kNeedEmail ? "Email address" :
      state == telegram::AuthState::kNeedEmailCode ? "Email code" : "First name",
      state == telegram::AuthState::kNeedPassword);
  lv_obj_t *second = nullptr;
  lv_obj_t *third = nullptr;
  if (configure) {
    lv_textarea_set_accepted_chars(first, "0123456789");
    lv_textarea_set_max_length(first, 10);
    second = Input(scene->surface, first_y + 148, "Telegram API hash");
    third = Input(scene->surface, first_y + 296, "AERA vault password", true);
    lv_textarea_set_max_length(second, 64);
    lv_textarea_set_max_length(third, 128);
  } else if (state == telegram::AuthState::kNeedRegistration) {
    second = Input(scene->surface, first_y + 148, "Last name");
  }

  auto *keyboard = lv_keyboard_create(scene->surface);
  StyleKeyboard(keyboard);
  lv_obj_set_pos(keyboard, scene->landscape ? 1600 : 42,
                 scene->landscape ? 300 : configure ? 1320 : 950);
  if (scene->landscape) lv_obj_set_size(keyboard, 1500, 850);
  BindKeyboard(first, keyboard);
  if (second) BindKeyboard(second, keyboard);
  if (third) BindKeyboard(third, keyboard);
  lv_keyboard_set_textarea(keyboard, first);

  auto *submit = Button(scene->surface,
      configure ? "Save and continue" : "Continue",
      [scene, state, first, second, third] {
        std::string value = lv_textarea_get_text(first);
        telegram::Kind kind = telegram::Kind::kPhone;
        if (state == telegram::AuthState::kNeedConfiguration) {
          const long api_id = strtol(value.c_str(), nullptr, 10);
          const std::string hash = second ? lv_textarea_get_text(second) : "";
          const std::string vault = third ? lv_textarea_get_text(third) : "";
          if (api_id <= 0 || hash.size() < 16 || vault.size() < 8) {
            lv_label_set_text(scene->detail,
                "API ID/hash are available at my.telegram.org. Use at least 8 characters for the vault password.");
            return;
          }
          scene->Send(telegram::Kind::kConfigure, hash + "\n" + vault, api_id);
          lv_textarea_set_text(third, "");
          ShowStatus(scene, "Opening encrypted session",
                     "TDLib is unlocking your local AERA Telegram database.");
          return;
        }
        if (value.empty()) return;
        if (state == telegram::AuthState::kNeedCode) kind = telegram::Kind::kCode;
        else if (state == telegram::AuthState::kNeedPassword) kind = telegram::Kind::kPassword;
        else if (state == telegram::AuthState::kNeedEmail) kind = telegram::Kind::kEmail;
        else if (state == telegram::AuthState::kNeedEmailCode) kind = telegram::Kind::kEmailCode;
        else if (state == telegram::AuthState::kNeedRegistration) {
          kind = telegram::Kind::kRegister;
          value += "\n" + std::string(second ? lv_textarea_get_text(second) : "");
        }
        scene->Send(kind, value);
        lv_textarea_set_text(first, "");
        ShowStatus(scene, "Checking with Telegram", "Keep this recovery connected to Wi-Fi.");
      }, true);
  lv_obj_set_pos(submit, 776, configure ? 1134 : 700);
  lv_obj_set_size(submit, 494, 118);
}

void ShowChats(TelegramScene *scene);

void SetChatKeyboard(TelegramScene *scene, bool visible) {
  if (!scene->keyboard || !scene->composer || !scene->list ||
      !scene->send_button || !scene->attach_button) return;
  scene->keyboard_visible = visible;
  const int composer_y = scene->landscape ? 170 : visible ? 1990 : 2780;
  lv_obj_set_y(scene->attach_button, composer_y);
  lv_obj_set_y(scene->composer, composer_y);
  lv_obj_set_y(scene->send_button, composer_y);
  lv_obj_set_height(scene->list, scene->landscape ? 1080 :
                    visible ? 1760 : 2520);
  if (visible) {
    lv_obj_remove_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(scene->keyboard);
  } else {
    lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(scene->composer, LV_STATE_FOCUSED);
  }
  lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_OFF);
}

struct AttachmentEntry {
  std::string name;
  std::string path;
  bool directory = false;
  uint64_t size = 0;
};

std::string ParentDirectory(const std::string &path) {
  if (path == "/sdcard") return path;
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos || slash < 7
      ? "/sdcard" : path.substr(0, slash);
}

void ShowAttachmentPicker(TelegramScene *scene, const std::string &requested) {
  const std::string path = requested.compare(0, 7, "/sdcard") == 0 &&
          requested.find("/../") == std::string::npos
      ? requested : "/sdcard";
  auto *overlay = lv_obj_create(scene->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &kModalMarker);
  lv_obj_set_align(overlay, LV_ALIGN_TOP_LEFT);
  const int status_height = StatusBarHeight();
  lv_obj_set_pos(overlay, 0, status_height);
  lv_obj_set_size(overlay, lv_obj_get_width(scene->screen),
                   lv_obj_get_height(scene->screen) - status_height);
  lv_obj_set_style_bg_color(overlay, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

  auto *close = Button(overlay, LV_SYMBOL_LEFT, [overlay] {
    lv_obj_delete_async(overlay);
  });
  lv_obj_set_pos(close, 24, 20);
  lv_obj_set_size(close, 120, 112);
  auto *title = Label(overlay, "Attach a file", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 174, 22);
  auto *where = Label(overlay, path.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(where, 174, 84);
  lv_obj_set_width(where, scene->landscape ? 2850 : 1190);
  lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);

  auto *list = lv_obj_create(overlay);
  Clear(list);
  lv_obj_set_pos(list, 24, 160);
  lv_obj_set_size(list, scene->landscape ? 3120 : 1392,
                  lv_obj_get_height(overlay) - 184);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);

  std::vector<AttachmentEntry> entries;
  if (path != "/sdcard")
    entries.push_back({"Up one folder", ParentDirectory(path), true, 0});
  if (DIR *directory = opendir(path.c_str())) {
    while (auto *item = readdir(directory)) {
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
      std::string child = path + "/" + item->d_name;
      if (child.size() >= telegram::kTextBytes) continue;
      struct stat info{};
      if (lstat(child.c_str(), &info) != 0 || S_ISLNK(info.st_mode)) continue;
      if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) continue;
      entries.push_back({item->d_name, std::move(child), S_ISDIR(info.st_mode),
                         static_cast<uint64_t>(std::max<off_t>(0, info.st_size))});
      if (entries.size() >= 300) break;
    }
    closedir(directory);
  }
  const size_t parent_rows = path == "/sdcard" ? 0 : 1;
  std::sort(entries.begin() + parent_rows, entries.end(),
            [](const AttachmentEntry &left, const AttachmentEntry &right) {
    if (left.directory != right.directory) return left.directory > right.directory;
    return left.name < right.name;
  });

  int y = 0;
  for (const auto &entry : entries) {
    auto *row = Button(list, "", [scene, overlay, entry] {
      if (entry.directory) {
        ShowAttachmentPicker(scene, entry.path);
        lv_obj_delete_async(overlay);
        return;
      }
      if (scene->Send(telegram::Kind::kSendFile, entry.path, scene->chat_id)) {
        if (scene->detail)
          lv_label_set_text_fmt(scene->detail, "Uploading %s", entry.name.c_str());
      }
      lv_obj_delete_async(overlay);
    });
    lv_obj_set_pos(row, 0, y);
    const int row_width = scene->landscape ? 3120 : 1392;
    lv_obj_set_size(row, row_width, 142);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    auto *icon = IconPlate(row,
        entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
        entry.directory ? kCyan : kAccent, kMainPanel, 76);
    lv_obj_set_pos(icon, 22, 32);
    auto *name = Label(row, entry.name.c_str(), &lv_font_montserrat_32, kText);
    lv_obj_set_pos(name, 126, 24);
    lv_obj_set_width(name, row_width - 280);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    const std::string meta = entry.directory ? "Folder" : Size(entry.size);
    auto *detail = Label(row, meta.c_str(), &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(detail, 126, 82);
    auto *arrow = Label(row, entry.directory ? LV_SYMBOL_RIGHT : LV_SYMBOL_PLUS,
                        &lv_font_montserrat_32,
                        entry.directory ? kMutedStrong : kCyan);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -34, 0);
    y += 142;
  }
  if (entries.empty()) {
    auto *empty = Label(list, "No attachable files in this folder",
                        &lv_font_montserrat_32, kMuted);
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 100);
  }
}

void OpenChat(TelegramScene *scene, int64_t id, const std::string &title) {
  scene->chat_id = id;
  scene->history_loading = true;
  lv_obj_clean(scene->surface);
  scene->keyboard = nullptr;
  scene->composer = nullptr;
  scene->send_button = nullptr;
  scene->attach_button = nullptr;
  auto *back = Button(scene->surface, LV_SYMBOL_LEFT, [scene] {
    scene->chat_id = 0;
    ShowChats(scene);
  });
  lv_obj_set_pos(back, 24, 20);
  lv_obj_set_size(back, 120, 112);
  scene->title = Label(scene->surface, title.c_str(),
                       &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->title, 174, 20);
  lv_obj_set_width(scene->title, 1160);
  lv_label_set_long_mode(scene->title, LV_LABEL_LONG_DOT);
  scene->detail = Label(scene->surface, "Loading message history...",
                        &lv_font_montserrat_24, kGreen);
  lv_obj_set_pos(scene->detail, 174, 88);
  scene->progress = nullptr;

  scene->list = lv_obj_create(scene->surface);
  Clear(scene->list);
  lv_obj_set_pos(scene->list, 16, 170);
  lv_obj_set_size(scene->list, scene->landscape ? 1536 : 1408,
                  scene->landscape ? 1080 : 2520);
  lv_obj_add_flag(scene->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scene->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_opa(scene->list, LV_OPA_TRANSP, 0);
  scene->item_y = 18;

  scene->attach_button = Button(scene->surface, LV_SYMBOL_PLUS, [scene] {
    SetChatKeyboard(scene, false);
    ShowAttachmentPicker(scene, "/sdcard");
  });
  lv_obj_set_pos(scene->attach_button, scene->landscape ? 1600 : 32,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_size(scene->attach_button, 132, 132);
  lv_obj_set_style_radius(scene->attach_button, 66, 0);

  scene->composer = Input(scene->surface, 2780, "Message");
  lv_obj_set_pos(scene->composer, scene->landscape ? 1750 : 182,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_width(scene->composer, scene->landscape ? 1190 : 1088);
  scene->send_button = Button(scene->surface, LV_SYMBOL_RIGHT, [scene] {
    const std::string text = lv_textarea_get_text(scene->composer);
    if (!text.empty() &&
        scene->Send(telegram::Kind::kSendText, text, scene->chat_id)) {
      lv_textarea_set_text(scene->composer, "");
      lv_label_set_text(scene->detail, "Sending...");
    }
  }, true);
  lv_obj_set_pos(scene->send_button, scene->landscape ? 2960 : 1288,
                 scene->landscape ? 170 : 2780);
  lv_obj_set_size(scene->send_button, 120, 132);
  lv_obj_set_style_radius(scene->send_button, 60, 0);

  scene->keyboard = lv_keyboard_create(scene->surface);
  StyleKeyboard(scene->keyboard);
  lv_obj_set_pos(scene->keyboard, scene->landscape ? 1600 : 16,
                 scene->landscape ? 330 : 2160);
  lv_obj_set_size(scene->keyboard, scene->landscape ? 1552 : 1408,
                  scene->landscape ? 900 : 800);
  lv_keyboard_set_textarea(scene->keyboard, scene->composer);
  lv_obj_add_flag(scene->keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(scene->composer, [](lv_event_t *event) {
    SetChatKeyboard(static_cast<TelegramScene *>(lv_event_get_user_data(event)), true);
  }, LV_EVENT_FOCUSED, scene);
  lv_obj_add_event_cb(scene->keyboard, [](lv_event_t *event) {
    auto *scene = static_cast<TelegramScene *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) {
      lv_obj_send_event(scene->send_button, LV_EVENT_CLICKED, nullptr);
      SetChatKeyboard(scene, false);
    } else if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
      SetChatKeyboard(scene, false);
    }
  }, LV_EVENT_ALL, scene);
  scene->Send(telegram::Kind::kOpenChat, {}, id);
}

void AddChat(TelegramScene *scene, const telegram::Message &message) {
  if (!scene->list) return;
  const std::string payload = message.text;
  const auto split = payload.find('\n');
  const std::string title = payload.substr(0, split);
  const std::string preview = split == std::string::npos
      ? "No recent message" : payload.substr(split + 1);
  auto *row = Button(scene->list, "", [scene, id = message.primary, title] {
    OpenChat(scene, id, title);
  });
  lv_obj_set_pos(row, 0, scene->item_y);
  const int row_width = scene->landscape ? 3136 : 1408;
  lv_obj_set_size(row, row_width, 164);
  lv_obj_set_style_radius(row, 0, 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  auto *avatar = IconPlate(row, LV_SYMBOL_ENVELOPE, kCyan, kMainPanel, 92);
  lv_obj_set_pos(avatar, 26, 35);
  auto *name = Label(row, title.c_str(), &lv_font_montserrat_32, kText);
  lv_obj_set_pos(name, 148, 28);
  lv_obj_set_width(name, row_width - (message.value ? 350 : 250));
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  auto *copy = Label(row, preview.c_str(), &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(copy, 148, 88);
  lv_obj_set_width(copy, row_width - 300);
  lv_label_set_long_mode(copy, LV_LABEL_LONG_DOT);
  if (message.value) {
    char count[16];
    snprintf(count, sizeof(count), "%u", message.value);
    auto *badge = lv_obj_create(row);
    Panel(badge, 32, kCyan);
    lv_obj_set_size(badge, 76, 58);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -34, 0);
    auto *number = Label(badge, count, &lv_font_montserrat_20, kCanvas);
    lv_obj_center(number);
  }
  auto *line = lv_obj_create(row);
  Clear(line);
  lv_obj_set_pos(line, 148, 162);
  lv_obj_set_size(line, row_width - 148, 1);
  lv_obj_set_style_bg_color(line, kMainLine, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  scene->item_y += 164;
}

void ShowChats(TelegramScene *scene) {
  lv_obj_clean(scene->surface);
  scene->chat_id = 0;
  scene->keyboard = nullptr;
  scene->composer = nullptr;
  scene->send_button = nullptr;
  scene->attach_button = nullptr;
  auto *home = Button(scene->surface, LV_SYMBOL_LEFT, [scene] {
    if (scene->callback) scene->callback(Action::kBack, scene->context);
  });
  lv_obj_set_pos(home, 24, 20);
  lv_obj_set_size(home, 120, 112);
  scene->title = Label(scene->surface, "Chats", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(scene->title, 174, 20);
  scene->detail = Label(scene->surface, "Connected securely",
                        &lv_font_montserrat_24, kGreen);
  lv_obj_set_pos(scene->detail, 174, 88);
  scene->progress = nullptr;
  auto *status = Kicker(scene->surface, "AERA TELEGRAM", kCyan);
  lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -32, 48);
  scene->list = lv_obj_create(scene->surface);
  Clear(scene->list);
  lv_obj_set_pos(scene->list, 16, 170);
  lv_obj_set_size(scene->list, scene->landscape ? 3136 : 1408,
                  scene->landscape ? 1080 : 2818);
  lv_obj_add_flag(scene->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(scene->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_style_bg_opa(scene->list, LV_OPA_TRANSP, 0);
  scene->item_y = 0;
  scene->Send(telegram::Kind::kLoadChats);
}

void AddMessage(TelegramScene *scene, const telegram::Message &message) {
  if (!scene->list || scene->chat_id != message.primary) return;
  std::string payload = message.text;
  const auto split = payload.find('\n');
  const std::string sender = payload.substr(0, split);
  const std::string text = split == std::string::npos ? "" : payload.substr(split + 1);
  auto *bubble = lv_obj_create(scene->list);
  Panel(bubble, 34, message.value ? Color(0x16495a) : kMainPanel);
  const int width = scene->landscape ? 1120 : 1010;
  lv_obj_set_pos(bubble,
      message.value ? (scene->landscape ? 396 : 370) : 20, scene->item_y);
  lv_obj_set_size(bubble, width, 160);
  auto *name = Label(bubble, sender.c_str(), &lv_font_montserrat_20,
                     message.value ? kCyan : kGreen);
  lv_obj_set_pos(name, 30, 22);
  auto *body = Label(bubble, text.c_str(), &lv_font_montserrat_24, kText);
  lv_obj_set_pos(body, 30, 64);
  lv_obj_set_width(body, width - 60);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_update_layout(body);
  const int bubble_height = std::clamp(lv_obj_get_height(body) + 98, 150, 620);
  lv_obj_set_height(bubble, bubble_height);
  scene->item_y += bubble_height + 20;
  if (!scene->history_loading)
    lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_ON);
}

void Poll(TelegramScene *scene) {
  for (int i = 0; i < 24 && scene->control >= 0; ++i) {
    telegram::Message message;
    const ssize_t count = recv(scene->control, &message, sizeof(message),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count < 0 && errno == EINTR) continue;
    if (count != static_cast<ssize_t>(sizeof(message)) ||
        !telegram::Valid(message, true)) {
      close(scene->control); scene->control = -1;
      ShowStatus(scene, "Telegram stopped", "The isolated runtime closed unexpectedly.");
      return;
    }
    if (message.kind == telegram::Kind::kState) {
      const auto state = static_cast<telegram::AuthState>(message.value);
      if (state == telegram::AuthState::kReady) ShowChats(scene);
      else ShowAuth(scene, state, message.text);
    } else if (message.kind == telegram::Kind::kError) {
      if (scene->detail) lv_label_set_text(scene->detail, message.text);
    } else if (message.kind == telegram::Kind::kChat) {
      if (!scene->chat_id) AddChat(scene, message);
    } else if (message.kind == telegram::Kind::kMessage) {
      AddMessage(scene, message);
    } else if (message.kind == telegram::Kind::kStatus) {
      if (scene->detail && scene->chat_id)
        lv_label_set_text(scene->detail, message.text);
    } else if (message.kind == telegram::Kind::kChatsDone && scene->detail) {
      lv_label_set_text(scene->detail, scene->item_y ? "Connected securely" :
          "No chats were returned by Telegram.");
    } else if (message.kind == telegram::Kind::kMessagesDone && scene->detail) {
      scene->history_loading = false;
      lv_label_set_text_fmt(scene->detail, "%u messages loaded securely",
                            message.value);
      if (scene->list)
        lv_obj_scroll_to_y(scene->list, scene->item_y, LV_ANIM_OFF);
    }
  }
}

void Launch(TelegramScene *scene) {
  if (scene->launched || !scene->preparation.verified) return;
  scene->launched = true;
  std::string error;
  if (!scene->process.Start(scene->preparation.directory, scene->control, error)) {
    ShowStatus(scene, "AERA Telegram could not start", error.c_str());
    return;
  }
  fcntl(scene->control, F_SETFL, fcntl(scene->control, F_GETFL) | O_NONBLOCK);
  ShowStatus(scene, "Starting AERA Telegram",
             "Launching TDLib inside its private network sandbox.");
}
}  // namespace

void BuildTelegramScene(lv_obj_t *screen, ActionCallback callback, void *context) {
  auto *scene = new TelegramScene;
  scene->existing_vault =
      access("/data/recovery/AERA/telegram/database", F_OK) == 0;
  scene->screen = screen;
  scene->landscape = lv_obj_get_width(screen) > lv_obj_get_height(screen);
  scene->callback = callback;
  scene->context = context;
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<TelegramScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  scene->surface = lv_obj_create(screen);
  Clear(scene->surface);
  lv_obj_set_align(scene->surface, LV_ALIGN_TOP_LEFT);
  const int status_height = StatusBarHeight();
  lv_obj_set_pos(scene->surface, 0, status_height);
  lv_obj_set_size(scene->surface, lv_obj_get_width(screen),
                  lv_obj_get_height(screen) - status_height);
  lv_obj_set_style_bg_color(scene->surface, kMainCanvas, 0);
  lv_obj_set_style_bg_opa(scene->surface, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(scene->surface, 0, 0);
  scene->navigation = nullptr;
  ShowStatus(scene, "Preparing AERA Telegram",
             "Verifying the signed TDLib runtime and expanding it into private RAM.");
  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, "telegram",
                              "app-runtime", "telegram");
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<TelegramScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) {
      if (scene->progress)
        lv_bar_set_value(scene->progress, scene->preparation.progress.load(), LV_ANIM_OFF);
      return;
    }
    if (!scene->preparation.verified) {
      if (scene->title) lv_label_set_text(scene->title, "AERA Telegram unavailable");
      if (scene->detail) lv_label_set_text(scene->detail, scene->preparation.error.c_str());
      return;
    }
    Launch(scene);
    Poll(scene);
    if (scene->launched && !scene->process.Running() && scene->control >= 0) {
      close(scene->control); scene->control = -1;
      ShowStatus(scene, "AERA Telegram closed", "Return Home to reopen it.");
    }
  }, 16, scene);
}
}  // namespace recovery_ui2
