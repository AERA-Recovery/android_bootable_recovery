#include <string>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"
#include "../data.hpp"

GUIWlan::GUIWlan(xml_node<>* node)
	: GUIObject(node)
{
	mConnectedImg = NULL;
	mRenderX = mRenderY = mRenderW = mRenderH = 0;

	if (!node)
		return;

	xml_node<>* child;

	child = FindNode(node, "connected");
	if (!child) {
		LOGERR("GUIWlan: could not find <connected> node\n");
	} else {
		LOGINFO("GUIWlan: found <connected> node\n");
		mConnectedImg = LoadAttrImage(child, "img");
		if (mConnectedImg)
			LOGINFO("GUIWlan: connected image loaded\n");
		else
			LOGERR("GUIWlan: failed to load connected image resource\n");
	}

	LoadPlacement(FindNode(node, "placement"), &mRenderX, &mRenderY, &mRenderW, &mRenderH);
	SetPlacement(TOP_LEFT);
}

int GUIWlan::Render(void)
{
	if (!isConditionTrue())
		return 0;

	if (DataManager::GetIntValue("tw_wlan_connected") != 1)
		return 0;

	if (!mConnectedImg) {
		static bool warned = false;
		if (!warned) {
			LOGERR("GUIWlan: mConnectedImg is NULL\n");
			warned = true;
		}
		return 0;
	}

	if (!mConnectedImg->GetResource()) {
		static bool warned_res = false;
		if (!warned_res) {
			LOGERR("GUIWlan: mConnectedImg resource is NULL\n");
			warned_res = true;
		}
		return 0;
	}

	gr_blit(mConnectedImg->GetResource(), 0, 0, mRenderW, mRenderH, mRenderX, mRenderY);
	return 0;
}

int GUIWlan::Update(void)
{
	if (!isConditionTrue())
		return 0;

	static int last_state = -1;
	int current_state = DataManager::GetIntValue("tw_wlan_connected");

	if (current_state != last_state) {
		last_state = current_state;
		return 2;
	}

	return 0;
}
