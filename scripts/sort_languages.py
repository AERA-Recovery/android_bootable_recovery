import os
import shutil

# Define which languages to move (omit .xml)
LANGUAGES_TO_MOVE = [
    'es_ES',
    'hu_HU',
    'zh_CN',
    "zh_TW",
    # Add more language files as needed
]

# For each language, specify the regular and medium font and scale to use
FONT_OVERRIDES = {
    "es_ES": {
        "regular": "Roboto-Regular.ttf",
        "medium": "Roboto-Medium.ttf",
        "scale": "100"
    },
    "hu_HU": {
        "regular": "Roboto-Regular.ttf",
        "medium": "Roboto-Medium.ttf",
        "scale": "100"
    },
    "zh_CN": {
        "regular": "DroidSansFallback.ttf",
        "medium": "DroidSansFallback.ttf",
        "scale": "100"
    },
    "zh_TW": {
        "regular": "DroidSansFallback.ttf",
        "medium": "DroidSansFallback.ttf",
        "scale": "100"
    },
}

# List of all font resource names to generate
FONT_RESOURCE_NAMES = [
    "status", "bigfont", "body1", "body2", "subhead", "title", "mainbutton", "button", "menu", "input_fail", "splash",
    "font_l", "font_m", "font_s", "font_b",
    "keylabel", "keylabel-bold", "keylabel-num", "keylabel-small", "keylabel-longpress",
    "Secondary", "Secondary-title", "caption", "clock", "info", "fixed", "orangefont"
]

# For these resource names, use the "regular" font, otherwise use "medium"
REGULAR_FONT_NAMES = {
    "body1", "subhead", "mainbutton", "keylabel", "keylabel-num", "keylabel-small", "keylabel-longpress", "clock"
}

SRC_DIR = "gui/theme/common/languages"
DST_DIR = "gui/theme/extra-languages/languages"

def generate_font_override_block(font_info):
    lines = ['      <!-- Font overrides - only change these if your language requires special characters -->\n']
    for name in FONT_RESOURCE_NAMES:
        if name in REGULAR_FONT_NAMES:
            filename = font_info["regular"]
        else:
            filename = font_info["medium"]
        scale = font_info["scale"]
        line = f'       <resource name="{name}" type="fontoverride" filename="{filename}" scale="{scale}"/>\n'
        lines.append(line)
    return ''.join(lines)

def patch_font_override(xml_path, font_info):
    with open(xml_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    new_lines = []
    inserted = False
    font_override_block = generate_font_override_block(font_info)
    for line in lines:
        if not inserted and "<resources>" in line:
            new_lines.append(line)
            new_lines.append(font_override_block)
            inserted = True
        else:
            new_lines.append(line)

    with open(xml_path, "w", encoding="utf-8") as f:
        f.writelines(new_lines)

def main():
    for lang in LANGUAGES_TO_MOVE:
        lang_file = lang + ".xml"
        src = os.path.join(SRC_DIR, lang_file)
        dst = os.path.join(DST_DIR, lang_file)
        if not os.path.exists(src):
            print(f"Source file not found: {src}")
            continue

        shutil.move(src, dst)
        print(f"Moved {lang_file} to extra-languages.")

        font_info = FONT_OVERRIDES.get(lang)
        if font_info:
            patch_font_override(dst, font_info)
            print(f"Patched {lang_file} with font overrides.")
        else:
            print(f"No font override defined for {lang_file}, skipping patch.")

if __name__ == "__main__":
    main()