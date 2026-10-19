/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include "src/drivers/opengles/glad/include/glad/egl.h"
#include "src/drivers/opengles/glad/include/glad/gles2.h"
#include "src/drivers/opengles/lv_opengles_texture.h"
#include "src/drivers/opengles/lv_opengles_driver.h"
#include "src/draw/opengles/lv_draw_opengles.h"

namespace gpu_test {
inline bool enabled = false;
inline void *egl_library, *gles_library;
inline GLADapiproc Load(void *library, const char *name) {
  auto *symbol = dlsym(library, name);
  return symbol ? reinterpret_cast<GLADapiproc>(symbol)
                : reinterpret_cast<GLADapiproc>(eglGetProcAddress(name));
}
inline void Initialize() {
  enabled = std::getenv("AERA_TEST_GPU") != nullptr;
  if (!enabled) return;
  setenv("EGL_PLATFORM", "surfaceless", 1);
  egl_library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  gles_library = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  assert(egl_library && gles_library);
  glad_eglGetProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(dlsym(egl_library, "eglGetProcAddress"));
  auto get_display = reinterpret_cast<PFNEGLGETDISPLAYPROC>(dlsym(egl_library, "eglGetDisplay"));
  auto initialize = reinterpret_cast<PFNEGLINITIALIZEPROC>(dlsym(egl_library, "eglInitialize"));
  auto display = get_display(EGL_DEFAULT_DISPLAY);
  EGLint major, minor; assert(initialize(display, &major, &minor));
  assert(gladLoadEGLUserPtr(display, Load, egl_library));
  assert(eglBindAPI(EGL_OPENGL_ES_API));
  const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig config; EGLint count;
  assert(eglChooseConfig(display, attributes, &config, 1, &count) && count);
  const EGLint size[] = {EGL_WIDTH, 1440, EGL_HEIGHT, 3168, EGL_NONE};
  auto surface = eglCreatePbufferSurface(display, config, size);
  const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
  assert(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE);
  assert(eglMakeCurrent(display, surface, surface, context));
  assert(gladLoadGLES2UserPtr(Load, gles_library));
  std::fprintf(stderr, "GPU fixture: %s\n", glGetString(GL_RENDERER));
  lv_draw_opengles_set_enabled(true);
}
inline void Check(const char *stage) {
  auto error = glGetError();
  if (error) std::fprintf(stderr, "GL error 0x%x at %s\n", error, stage);
  assert(error == GL_NO_ERROR);
}
inline void Flush(lv_display_t *display, const lv_area_t *, uint8_t *) {
  if (lv_display_flush_is_last(display)) {
    Check("LVGL draw");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 1440, 3168);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    lv_opengles_render_display_texture(display, false, false);
    glFinish();
    Check("present");
  }
  lv_display_flush_ready(display);
}
}
