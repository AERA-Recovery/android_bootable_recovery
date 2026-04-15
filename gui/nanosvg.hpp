/*
	Copyright (C) 2020-2023 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.
	
	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
	
	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with OrangeFox.  If not, see <http://www.gnu.org/licenses/>.

	This file is neeeded because of twinstall being moved to Soong,
	such that conditions from Android.mk etc, are not being processed.
	So we use boolean functions here to do that job.
	
*/

#ifndef NANOSVG_HPP
#define NANOSVG_HPP

#include "minuitwrp/truetype.hpp"

extern "C" {
#include "minuitwrp/minui.h"
}
/*
#define NANOSVG_ALL_COLOR_KEYWORDS	// Include full list of color keywords.
#define NANOSVG_IMPLEMENTATION		// Expands implementation
#include "nanosvg.h"
#include "nanosvgrast.h"
*/

int res_create_svg_surface(const char* name, float scale, gr_surface* pSurface);

#endif
