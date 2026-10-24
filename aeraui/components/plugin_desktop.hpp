/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl.h>
#include <functional>
namespace aeraui {
// Optional API 2 shared-frame surface. Never launches code or resolves paths.
// Host owns size-sealed memory and the input socket. Caller closes returned FDs.
bool OpenPluginDesktop(lv_obj_t *parent, int &frame, int &input,
                       std::function<void()> on_close, unsigned width=960, unsigned height=540);
inline bool ValidDesktopResolution(unsigned w,unsigned h) {
  return (w==640&&h==360)||(w==960&&h==540)||(w==1280&&h==720)||(w==1920&&h==1080);
}
void ClosePluginDesktop();
bool PluginDesktopHandlePointer(int slot, int x, int y, bool down);
bool PluginDesktopHandleMouse(int kind, int x, int y, int button, bool down,
                              int dx, int dy);
bool PluginDesktopHandleKey(unsigned key, int value);
void PluginDesktopReleaseKeyboard();
}
