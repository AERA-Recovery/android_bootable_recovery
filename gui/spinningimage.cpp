/*
	This rotates ONE normal image/SVG resource by calling gr_blit_rotated()
	inside minui.

	XML:

	<object type="spinningimage">
		<placement x="..." y="..." placement="..."/>
		<offset x="0" y="0"/>
		<image resource="wlan_loading"/>
		<speed render="2"/>
		<rotate speed="540" clockwise="1" start="0"/>
	</object>

	rotate speed = degrees per second.
	So speed="540" means 1.5 rotations per second on every build.
*/

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "../twcommon.h"
}

#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

GUISpinningImage::GUISpinningImage(xml_node<>* node) : GUIObject(node)
{
	xml_node<>* child;

	mImage = NULL;

	mRender = 2;
	mClockwise = 1;
	mDegreesPerSecond = 360;
	mAngle = 0.0;

	mOffsetX = 0;
	mOffsetY = 0;

	mLoggedMissingImage = false;

	if (!node)
		return;

	mImage = LoadAttrImage(FindNode(node, "image"), "resource");

	if (!mImage)
		mImage = LoadAttrImage(FindNode(node, "resource"), "name");

	LoadPlacement(FindNode(node, "placement"), &mRenderX, &mRenderY, NULL, NULL, &mPlacement);

	/*
	 * XML:
	 * <speed render="2"/>
	 *
	 * FPS is intentionally not read here.
	 * The object updates at the GUI framerate, TW_FRAMERATE.
	 */
	child = FindNode(node, "speed");
	if (child) {
		mRender = LoadAttrInt(child, "render", mRender);
	}

	/*
	 * XML:
	 * <rotate speed="540" clockwise="1" start="0"/>
	 *
	 * speed = degrees per second.
	 */
	child = FindNode(node, "rotate");
	if (child) {
		mDegreesPerSecond = LoadAttrInt(child, "speed", mDegreesPerSecond);
		mClockwise = LoadAttrInt(child, "clockwise", mClockwise);
		mAngle = static_cast<double>(LoadAttrInt(child, "start", static_cast<int>(mAngle)));
	}

	child = FindNode(node, "offset");
	if (child) {
		mOffsetX = LoadAttrInt(child, "x", mOffsetX);
		mOffsetY = LoadAttrInt(child, "y", mOffsetY);
	}

	if (mDegreesPerSecond < 1)
		mDegreesPerSecond = 1;

	while (mAngle < 0.0)
		mAngle += 360.0;

	while (mAngle >= 360.0)
		mAngle -= 360.0;

	if (mImage && mImage->GetResource()) {
		mRenderW = mImage->GetWidth();
		mRenderH = mImage->GetHeight();

		if (mPlacement != TOP_LEFT && mPlacement != BOTTOM_LEFT) {
			if (mPlacement == CENTER)
				mRenderX -= (mRenderW / 2);
			else
				mRenderX -= mRenderW;
		}

		if (mPlacement != TOP_LEFT && mPlacement != TOP_RIGHT) {
			if (mPlacement == CENTER)
				mRenderY -= (mRenderH / 2);
			else
				mRenderY -= mRenderH;
		}

		SetPlacement(TOP_LEFT);
	}
}

GUISpinningImage::~GUISpinningImage()
{
}

int GUISpinningImage::Render(void)
{
	if (!isConditionTrue())
		return 0;

	if (!mImage || !mImage->GetResource()) {
		if (!mLoggedMissingImage) {
			gui_print("GUISpinningImage: image resource missing or not loaded\n");
			mLoggedMissingImage = true;
		}
		return 0;
	}

	if (mRenderW <= 0 || mRenderH <= 0)
		return 0;

	gr_blit_rotated(
		mImage->GetResource(),
		0,
		0,
		mRenderW,
		mRenderH,
		mRenderX + mOffsetX,
		mRenderY + mOffsetY,
		static_cast<int>(mAngle + 0.5)
	);

	return 0;
}

int GUISpinningImage::Update(void)
{
	if (!isConditionTrue())
		return 0;

	if (!mImage || !mImage->GetResource())
		return 0;

	const double frame_rate = TW_FRAMERATE > 0 ? static_cast<double>(TW_FRAMERATE) : 60.0;
	const double degrees_this_frame = static_cast<double>(mDegreesPerSecond) / frame_rate;

	if (mClockwise)
		mAngle += degrees_this_frame;
	else
		mAngle -= degrees_this_frame;

	while (mAngle < 0.0)
		mAngle += 360.0;

	while (mAngle >= 360.0)
		mAngle -= 360.0;

	if (mRender == 2)
		return 2;

	Render();
	return 1;
}
