// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

package twrp

import (
	"android/soong/android"
	"android/soong/cc"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// UI2 owns all visible recovery rendering. Only its runtime fallback fonts
// belong in /twres; the legacy XML page/theme bundle is deliberately omitted.
func copyAeraFonts(ctx android.BaseContext) bool {
	twRes := filepath.Join(ctx.Config().Getenv("OUT"), "recovery/root/twres")
	if err := os.RemoveAll(twRes); err != nil {
		fmt.Printf("Could not clear obsolete recovery UI resources: %v\n", err)
		return false
	}
	fontDest := filepath.Join(twRes, "fonts")
	if err := os.MkdirAll(fontDest, os.ModePerm); err != nil {
		fmt.Printf("Could not create AERA font directory: %v\n", err)
		return false
	}
	copyDir(getRecoveryAbsDir(ctx)+"ui2/fonts/assets/", fontDest)
	return true
}

func globalFlags(ctx android.BaseContext) []string {
	var cflags []string
	if getMakeVars(ctx, "AB_OTA_UPDATER") == "true" {
		cflags = append(cflags, "-DAB_OTA_UPDATER=1")
	}
	adaptiveResolution := strings.Trim(getMakeVars(ctx, "AERA_UI2_ADAPTIVE_RESOLUTION"), "\"")
	if adaptiveResolution == "1" || strings.EqualFold(adaptiveResolution, "true") {
		cflags = append(cflags, "-DAERA_UI2_ADAPTIVE_RESOLUTION=1")
	}
	for _, metric := range []string{
		"AERA_SCREEN_H", "AERA_STATUS_H", "AERA_STATUS_INDENT_LEFT", "AERA_STATUS_INDENT_RIGHT",
	} {
		value := strings.Trim(getMakeVars(ctx, metric), "\"")
		if parsed, err := strconv.Atoi(value); err == nil && parsed > 0 {
			cflags = append(cflags, fmt.Sprintf("-D%s=%d", metric, parsed))
		}
	}
	cflags = append(cflags, aeraUi2LanguageFlags(ctx)...)
	return cflags
}

func aeraUi2LanguageFlags(ctx android.BaseContext) []string {
	var cflags []string
	defaultLanguage := strings.Trim(getMakeVars(ctx, "AERA_DEFAULT_LANGUAGE"), "\"")
	if defaultLanguage == "" {
		defaultLanguage = "en"
	}
	validLanguage := true
	for _, r := range defaultLanguage {
		if !(r == '_' || r == '-' || r >= 'a' && r <= 'z' ||
			r >= 'A' && r <= 'Z' || r >= '0' && r <= '9') {
			validLanguage = false
			break
		}
	}
	if !validLanguage {
		defaultLanguage = "en"
	}
	cflags = append(cflags, fmt.Sprintf("-DAERA_DEFAULT_LANGUAGE=\"%s\"", defaultLanguage))
	if getMakeVars(ctx, "AERA_EXTRA_LANGUAGES") == "true" {
		cflags = append(cflags, "-DAERA_EXTRA_LANGUAGES=1")
	}
	return cflags
}

func globalSrcs(ctx android.BaseContext) []string {
	var srcs []string
	if getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD") != "" {
		srcs = append(srcs, getMakeVars(ctx, "TWRP_CUSTOM_KEYBOARD"))
	} else {
		srcs = append(srcs, "hardwarekeyboard.cpp")
	}
	if getMakeVars(ctx, "OF_ENABLE_WLAN") == "1" {
		srcs = append(srcs, "wlan.cpp")
	}
	return srcs
}

func aeraGuiBackendDefaults(ctx android.LoadHookContext) {
	type props struct {
		Target struct {
			Android struct {
				Cflags  []string
				Enabled *bool
			}
		}
		Cflags       []string
		Srcs         []string
		Include_dirs []string
	}
	p := &props{}
	p.Cflags = globalFlags(ctx)
	p.Srcs = globalSrcs(ctx)
	ctx.AppendProperties(p)
	legacyFlags := &props{}
	legacyFlags.Cflags = aeraGlobalFlags(ctx)
	ctx.AppendProperties(legacyFlags)
	if !copyAeraFonts(ctx) {
		os.Exit(-1)
	}
}

func init() {
	android.RegisterModuleType("aera_gui_backend_defaults", aeraGuiBackendDefaultsFactory)
	android.RegisterModuleType("aera_ui2_defaults", aeraUi2DefaultsFactory)
}

func aeraGuiBackendDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, aeraGuiBackendDefaults)
	return module
}

func aeraUi2Defaults(ctx android.BaseContext) []string {
	return aeraUi2LanguageFlags(ctx)
}

func aeraUi2DefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, func(ctx android.LoadHookContext) {
		type props struct {
			Cflags []string
		}
		ctx.AppendProperties(&props{Cflags: aeraUi2Defaults(ctx)})
	})
	return module
}
