#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check FlushDisplay against minui's consuming surface-ownership contract."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path,
    default=Path(__file__).resolve().parents[2]/'aeraui/core/engine.cpp')
args = parser.parse_args()
source = args.source.read_text(encoding='utf-8')
start = source.index('  static void FlushDisplay(')
end = source.index('\n  bool ConfigureDirectScanout()', start)
method = source[start:end]

prefix = r'''

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cassert>
using GGLuint = unsigned;
using gr_surface = void*;
struct GGLSurface { unsigned version, width, height, stride; unsigned char* data; int format; };
struct lv_display_t { void* user; };
struct lv_area_t {};
static int ready=0, blits=0;
static bool fail_scale=false;
void* lv_display_get_user_data(lv_display_t* d) { return d->user; }
void lv_display_flush_ready(lv_display_t*) { ++ready; }
bool lv_display_flush_is_last(lv_display_t*) { return true; }
constexpr int GGL_PIXEL_FORMAT_BGRA_8888=5, ANDROID_LOG_ERROR=6;
constexpr const char* kLogTag="AERA test";
int __android_log_print(int,const char*,const char*,...) { return 0; }
int gr_drm_present(gr_surface) { return 0; }
void gr_flip() {}
void gr_blit(gr_surface input,int,int,int,int,int,int) {
    assert(static_cast<GGLSurface*>(input)->data[0] == 42); ++blits;
}
void res_free_surface(gr_surface surface) { std::free(surface); }
int res_scale_surface(gr_surface source,gr_surface* result,float,float) {
    *result=nullptr;
    if (fail_scale) return -1;
    auto* scaled=static_cast<GGLSurface*>(std::malloc(sizeof(GGLSurface)));
    assert(scaled); *scaled=*static_cast<GGLSurface*>(source);
    *result=scaled;
    // This is the consuming contract in minuitwrp/resources.cpp.
    res_free_surface(source);
    return 0;
}
namespace recorder { void CapturePresentedFrame() {} }
struct Transform { bool scaled=true; bool IsScaled() const { return scaled; } };
struct Gpu { bool Present(lv_display_t*) { return true; } };
struct Impl {
    bool gpu_accelerated_=false,direct_scanout_=false;
    int width_=720,height_=1541,physical_width_=1080,physical_height_=2312;
    int submitted=0;
    Transform transform_;
    Gpu gpu_renderer_;
    GGLSurface *scanout_surface1_=nullptr,*scanout_surface2_=nullptr;
    gr_surface scanout1_=nullptr,scanout2_=nullptr;
    bool PresentScaledSoftware(uint8_t*) { return true; }
    void TrackSubmittedFrame() { ++submitted; }
'''
suffix = r'''

};
int main() {
    Impl self;
    lv_display_t display{&self}; lv_area_t area;
    uint8_t pixels[16]={42};
    for (int i=0;i<128;++i) {
        fail_scale=false; self.transform_.scaled=true;
        Impl::FlushDisplay(&display,&area,pixels);
        fail_scale=true;
        Impl::FlushDisplay(&display,&area,pixels);
        self.transform_.scaled=false;
        Impl::FlushDisplay(&display,&area,pixels);
    }
    assert(ready==384 && blits==384 && self.submitted==384);
    assert(pixels[0]==42);
    std::puts("384 scaled, failed-scale and unscaled flushes passed; borrowed pixels retained");
}
'''

with tempfile.TemporaryDirectory(prefix='aera-surface-test-') as directory:
    directory = Path(directory)
    cpp, executable = directory/'test.cpp', directory/'test'
    cpp.write_text(prefix+method+suffix)
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    subprocess.run(compiler+['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
        env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1'))
