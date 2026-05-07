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
	mFont = NULL;
	mRenderX = mRenderY = mRenderW = mRenderH = 0;
	mBatteryGap = 18;
	mFollowBattery = false;

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
	child = FindNode(node, "placement");
	if (child) {
		mFollowBattery = LoadAttrInt(child, "followBattery", 0) != 0;
		mBatteryGap = LoadAttrIntScaleX(child, "gap", mBatteryGap);
	}

	mFont = LoadAttrFont(FindNode(node, "font"), "resource");
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

	int renderX = mRenderX;
	if (mFollowBattery && mFont && mFont->GetResource()) {
		int batteryStyle = DataManager::GetIntValue("style_battery");
		int batteryIcon = DataManager::GetIntValue("enable_battery");
		int batteryCharge = DataManager::GetIntValue("charging_now");
		int batteryW = 26;
		int batteryPadding = 28;
		int chargingLeft = 0;

		if (batteryStyle == 1) {
			batteryW = 48;
			batteryPadding = 16;
		} else if (batteryStyle == 2) {
			batteryW = 63;
			batteryPadding = 10;
			if (batteryCharge == 1)
				chargingLeft = 26;
		}

		std::string batteryText = batteryIcon == 0 ? DataManager::GetStrValue("tw_battery_charge") :
			DataManager::GetStrValue("tw_battery") + "%";
		int batteryTextW = twrpTruetype::gr_ttf_measureEx(batteryText.c_str(), mFont->GetResource());
		int batteryGroupW = batteryTextW;
		if (batteryIcon == 1)
			batteryGroupW += batteryPadding + batteryW + chargingLeft;

		renderX = mRenderX - batteryGroupW - mBatteryGap - mRenderW;
	}

	gr_blit(mConnectedImg->GetResource(), 0, 0, mRenderW, mRenderH, renderX, mRenderY);
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
