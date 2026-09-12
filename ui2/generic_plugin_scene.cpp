/* SPDX-License-Identifier: Apache-2.0 */
#include "scene.hpp"

#include "browser/runtime.hpp"
#include "plugin_api/launcher.hpp"
#include "plugin_api/operations.hpp"
#include "plugin_api/protocol.hpp"
#include "plugin_api/session.hpp"
#include "plugins/plugin_manager.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace recovery_ui2 {
namespace {
using namespace design;
using namespace widgets;

std::mutex gSelectionMutex;
std::string gSelectedPlugin;

uint64_t NowMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct ButtonModel {
  uint32_t id = 0;
  uint32_t flags = 0;
  std::string title;
  std::string detail;
};

struct GenericScene {
  lv_obj_t *screen = nullptr;
  lv_obj_t *card = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *progress = nullptr;
  plugins::Plugin plugin;
  web::Preparation preparation;
  plugin_api::Process process;
  plugin_api::Session session;
  std::thread worker;
  lv_timer_t *timer = nullptr;
  std::string page_title;
  std::string page_body;
  std::vector<ButtonModel> buttons;
  uint64_t launched_ms = 0;
  uint64_t operation_started_ms = 0;
  uint32_t operation_request = 0;
  bool launched = false;
  bool page_pending = false;
  bool operation_pending = false;
  bool stopped = false;

  ~GenericScene() {
    if (timer) lv_timer_delete(timer);
    preparation.cancel.store(true);
    if (worker.joinable()) worker.join();
    if (session.Connected()) {
      session.Send(plugin_api::Kind::kLifecycle, 0,
                   static_cast<uint32_t>(plugin_api::Lifecycle::kStop));
      session.Send(plugin_api::Kind::kClose);
    }
    session.Close();
    process.Stop();
    web::RemoveRuntime(preparation.directory);
  }
};

void SetStatus(GenericScene *scene, const std::string &status) {
  if (scene->status) lv_label_set_text(scene->status, status.c_str());
}

bool ModalVisible(lv_obj_t *screen) {
  for (uint32_t index = 0; index < lv_obj_get_child_count(screen); ++index) {
    auto *child = lv_obj_get_child(screen, static_cast<int32_t>(index));
    if (lv_obj_get_user_data(child) == &kModalMarker &&
        !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return true;
  }
  return false;
}

void RenderPage(GenericScene *scene) {
  lv_obj_clean(scene->content);
  auto *title = Label(scene->content,
                      scene->page_title.empty() ? scene->plugin.name.c_str()
                                                : scene->page_title.c_str(),
                      &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 36, 30);
  lv_obj_set_width(title, std::max(400, lv_obj_get_width(scene->content) - 72));
  auto *body = Label(scene->content, scene->page_body.c_str(),
                     &lv_font_montserrat_32, kMutedStrong);
  lv_obj_set_pos(body, 36, 112);
  lv_obj_set_width(body, std::max(400, lv_obj_get_width(scene->content) - 72));
  lv_obj_set_style_text_line_space(body, 14, 0);
  lv_obj_update_layout(body);
  int y = 148 + std::max(80, static_cast<int>(lv_obj_get_height(body)));
  for (const auto &model : scene->buttons) {
    const bool disabled = (model.flags & plugin_api::kDisabled) != 0;
    auto action = [scene, id = model.id, disabled] {
      if (!disabled && scene->session.Connected())
        scene->session.Send(plugin_api::Kind::kAction, id);
    };
    auto *button = Button(scene->content, model.title.c_str(), action,
                          (model.flags & plugin_api::kPrimary) != 0);
    lv_obj_set_pos(button, 36, y);
    lv_obj_set_size(button, std::max(400, lv_obj_get_width(scene->content) - 72),
                    model.detail.empty() ? 112 : 146);
    if (disabled) lv_obj_add_state(button, LV_STATE_DISABLED);
    if (!model.detail.empty()) {
      auto *detail = Label(button, model.detail.c_str(),
                           &lv_font_montserrat_24,
                           (model.flags & plugin_api::kPrimary) ? kOnAccent
                                                                : kMuted);
      lv_obj_align(detail, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
    y += model.detail.empty() ? 136 : 170;
  }
  (void)y;
}

void ReplyOperation(GenericScene *scene, plugin_api::Operation operation,
                    uint32_t request) {
  std::string result;
  const bool success = plugin_api::RunOperation(scene->plugin, operation, result);
  scene->session.Send(plugin_api::Kind::kOperationResult, request,
                      success ? 1U : 0U, 0, nullptr, result.c_str());
  SetStatus(scene, result);
  scene->operation_pending = false;
  scene->operation_request = 0;
}

void RequestOperation(GenericScene *scene,
                      const plugin_api::Message &message) {
  if (message.request_id == 0 || message.value <
          static_cast<uint32_t>(plugin_api::Operation::kBackupSettings) ||
      message.value >
          static_cast<uint32_t>(
              plugin_api::Operation::kStartWifiMirror)) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Unsupported host operation.");
    return;
  }
  const auto operation = static_cast<plugin_api::Operation>(message.value);
  if (!plugin_api::OperationAllowed(scene->plugin, operation)) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Permission denied by the AERA host.");
    return;
  }
  if (scene->operation_pending) {
    scene->session.Send(plugin_api::Kind::kOperationResult,
                        message.request_id, 0, 0, nullptr,
                        "Another host operation is waiting for approval.");
    return;
  }
  scene->operation_pending = true;
  scene->operation_request = message.request_id;
  scene->operation_started_ms = NowMs();
  SetStatus(scene, "Waiting for your approval");
  Sheet(scene->screen, plugin_api::OperationTitle(operation),
        plugin_api::OperationPrompt(operation),
        [scene, operation, request = message.request_id] {
          ReplyOperation(scene, operation, request);
        });
}

void HandleMessage(GenericScene *scene, const plugin_api::Message &message) {
  switch (message.kind) {
    case plugin_api::Kind::kBeginPage:
      scene->page_title = message.title;
      scene->page_body = message.text;
      scene->buttons.clear();
      scene->page_pending = true;
      break;
    case plugin_api::Kind::kAddButton:
      if (!scene->page_pending || message.request_id == 0 ||
          message.title[0] == '\0' ||
          (message.flags & ~(plugin_api::kPrimary |
                             plugin_api::kDestructive |
                             plugin_api::kDisabled)) != 0 ||
          scene->buttons.size() >= plugin_api::kMaxButtons ||
          std::any_of(scene->buttons.begin(), scene->buttons.end(),
                      [&](const ButtonModel &button) {
                        return button.id == message.request_id;
                      })) {
        SetStatus(scene, "Plugin page exceeded the Host API limit");
        scene->session.Close();
        break;
      }
      scene->buttons.push_back({message.request_id, message.flags,
                                message.title, message.text});
      break;
    case plugin_api::Kind::kCommitPage:
      if (!scene->page_pending) {
        SetStatus(scene, "Plugin sent an invalid Host API sequence");
        scene->session.Close();
        break;
      }
      scene->page_pending = false;
      RenderPage(scene);
      SetStatus(scene, "Connected through AERA Host API 2");
      break;
    case plugin_api::Kind::kSetStatus:
      SetStatus(scene, message.text);
      break;
    case plugin_api::Kind::kRequestOperation:
      RequestOperation(scene, message);
      break;
    case plugin_api::Kind::kClose:
      scene->session.Close();
      scene->process.Stop();
      scene->stopped = true;
      SetStatus(scene, "Plugin closed");
      break;
    default:
      SetStatus(scene, "Plugin sent an invalid Host API sequence");
      scene->session.Close();
      break;
  }
}
}  // namespace

void SetSelectedPluginId(const std::string &id) {
  std::lock_guard<std::mutex> lock(gSelectionMutex);
  gSelectedPlugin = id;
}

std::string GetSelectedPluginId() {
  std::lock_guard<std::mutex> lock(gSelectionMutex);
  return gSelectedPlugin;
}

void BuildGenericPluginScene(lv_obj_t *screen, const std::string &id,
                             ActionCallback callback, void *context) {
  auto *scene = new GenericScene;
  scene->screen = screen;
  if (!plugins::FindInstalled(id, scene->plugin) ||
      !plugins::IsGeneric(scene->plugin)) {
    Header(screen, "Plugin unavailable",
           "The selected app is missing or does not support Host API 2.",
           callback, context);
    Navigation(screen, Action::kBackHome, callback, context, true);
    delete scene;
    return;
  }
  lv_obj_add_event_cb(screen, [](lv_event_t *event) {
    delete static_cast<GenericScene *>(lv_event_get_user_data(event));
  }, LV_EVENT_DELETE, scene);
  Header(screen, scene->plugin.name.c_str(),
         "Isolated app rendered by AERA Host API 2.", callback, context);
  const bool landscape = Landscape(screen);
  scene->card = lv_obj_create(screen);
  Panel(scene->card, 42, kMainPanel);
  lv_obj_set_pos(scene->card, 64, landscape ? 320 : 430);
  lv_obj_set_size(scene->card,
                  landscape ? lv_obj_get_width(screen) - 128 : 1312,
                  landscape ? lv_obj_get_height(screen) - 520 : 2250);
  scene->content = lv_obj_create(scene->card);
  Clear(scene->content);
  lv_obj_set_pos(scene->content, 24, 24);
  lv_obj_set_size(scene->content, lv_obj_get_width(scene->card) - 48,
                  lv_obj_get_height(scene->card) - 132);
  lv_obj_add_flag(scene->content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scene->content, LV_DIR_VER);
  scene->page_title = "Preparing " + scene->plugin.name;
  scene->page_body = "Verifying the plugin runtime and expanding it into private RAM.";
  RenderPage(scene);
  scene->progress = lv_bar_create(scene->card);
  lv_obj_set_pos(scene->progress, 36, lv_obj_get_height(scene->card) - 80);
  lv_obj_set_size(scene->progress, lv_obj_get_width(scene->card) - 72, 10);
  lv_obj_set_style_bg_color(scene->progress, kAccent, LV_PART_INDICATOR);
  scene->status = Label(scene->card, "Checking signed runtime",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_align(scene->status, LV_ALIGN_BOTTOM_LEFT, 36, -24);
  Navigation(screen, Action::kBackHome, callback, context, true);

  scene->worker = std::thread([scene] {
    web::PreparePluginRuntime(scene->preparation, scene->plugin.id.c_str(),
                              scene->plugin.type.c_str(),
                              scene->plugin.entry.c_str());
  });
  scene->timer = lv_timer_create([](lv_timer_t *timer) {
    auto *scene = static_cast<GenericScene *>(lv_timer_get_user_data(timer));
    if (!scene->preparation.done.load(std::memory_order_acquire)) {
      lv_bar_set_value(scene->progress, scene->preparation.progress.load(),
                       LV_ANIM_OFF);
      return;
    }
    if (!scene->preparation.verified) {
      SetStatus(scene, scene->preparation.error);
      return;
    }
    if (!scene->launched) {
      scene->launched = true;
      scene->launched_ms = NowMs();
      int control = -1;
      std::string error;
      if (!scene->process.Start(scene->preparation.directory, control, error) ||
          !scene->session.Adopt(control)) {
        SetStatus(scene, error.empty() ? scene->session.Status() : error);
        return;
      }
      lv_bar_set_value(scene->progress, 100, LV_ANIM_ON);
    }
    for (const auto &message : scene->session.Poll())
      HandleMessage(scene, message);
    if (scene->session.Connected() && !scene->session.Negotiated() &&
        NowMs() - scene->launched_ms > 5000) {
      scene->session.Close();
      SetStatus(scene, "Plugin handshake timed out");
    }
    if (scene->operation_pending &&
        NowMs() - scene->operation_started_ms > 60000) {
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          scene->operation_request, 0, 0, nullptr,
                          "Host operation approval timed out.");
      scene->operation_pending = false;
      scene->operation_request = 0;
    }
    if (scene->operation_pending && !ModalVisible(scene->screen)) {
      scene->session.Send(plugin_api::Kind::kOperationResult,
                          scene->operation_request, 0, 0, nullptr,
                          "Host operation cancelled by the user.");
      scene->operation_pending = false;
      scene->operation_request = 0;
      SetStatus(scene, "Operation cancelled");
    }
    if (!scene->stopped && !scene->process.Running()) {
      scene->stopped = true;
      scene->session.Close();
      SetStatus(scene, "Plugin process exited; its resources were reclaimed");
    }
  }, 16, scene);
}

}  // namespace recovery_ui2
