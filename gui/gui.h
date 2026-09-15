/*
        Copyright 2012 bigbiff/Dees_Troy TeamWin
        This file is part of TWRP/TeamWin Recovery Project.

        TWRP is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation, either version 3 of the License, or
        (at your option) any later version.

        TWRP is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _GUI_HEADER
#define _GUI_HEADER

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int gui_init();
int gui_loadResources();
int gui_loadCustomResources();
int gui_is_recovery_ui2_active();
int gui_start();
int gui_startPage(const char* page_name, const int allow_comands, int stop_on_page_done);
void gui_print(const char *fmt, ...);
void gui_print_color(const char *color, const char *fmt, ...);
void gui_set_FILE(FILE* f);

// FOX CLI progress mirroring (see gui/console.cpp). begin()/end() bracket a fox
// command; overall()/item() mirror the overall and current-item percentages
// onto the foxout stream when active.
void gui_fox_progress_begin();
void gui_fox_progress_end();
void gui_fox_progress_overall(const int percent);
void gui_fox_progress_item(const int percent);
void gui_fox_progress_detail(const char* phase, const int percent, const char* label,
                             unsigned long long current_bytes, unsigned long long total_bytes,
                             unsigned long long bytes_per_second, unsigned long long eta_seconds,
                             unsigned long long current_files, unsigned long long total_files,
                             const char* size_text, const char* file_text);

void set_scale_values(float w, float h);
int scale_theme_x(int initial_x);
int scale_theme_y(int initial_y);
int scale_theme_min(int initial_value);
float get_scale_w();
float get_scale_h();

#ifdef __cplusplus
}
#endif

#endif  // _GUI_HEADER
