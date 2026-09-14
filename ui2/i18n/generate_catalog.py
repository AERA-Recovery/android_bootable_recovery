#!/usr/bin/env python3
"""Generate the compact native AERA catalog."""

import argparse
import ast
import json
import pathlib
import re


LOCALES = [
    "en", "ar_SA", "bg_BG", "bn_BD", "ca_ES", "cs_CZ", "de_DE",
    "el_GR", "es-ES", "fa_IR", "fr_FR", "he_IL", "hi_IN", "hu",
    "id_ID", "it_IT", "ja_JP", "ko_KR", "nl_NL", "no_NO", "pl_PL",
    "pt_BR", "pt_PT", "ro_RO", "ru", "sr_Cyrl", "th_TH", "tr_TR",
    "uk_UA", "vi_VN", "zh_CN", "zh_TW",
]


def source_literals(ui2):
    values = set()
    for path in ui2.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        if "third_party" in path.parts or path.name == "catalog_generated.cpp":
            continue
        source = path.read_text(encoding="utf-8", errors="ignore")
        sequences = re.findall(r'(?:"(?:\\.|[^"\\])*"\s*)+', source)
        for sequence in sequences:
            try:
                value = "".join(
                    ast.literal_eval(token)
                    for token in re.findall(r'"(?:\\.|[^"\\])*"', sequence)
                ).strip()
            except (SyntaxError, ValueError):
                continue
            if value and any(character.isalpha() for character in value):
                values.add(value)
    return values


def quote(value):
    return json.dumps(value, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recovery", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument(
        "--catalog",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("catalog.json"),
    )
    args = parser.parse_args()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    if catalog.get("schema") != 1 or catalog.get("languages") != LOCALES:
        raise ValueError("catalog.json has an unsupported schema or language order")
    rows = catalog.get("strings", {})
    # catalog.json is already generated from the visible recovery surface plus
    # runtime-plugin messages that arrive over the host protocol. Those plugin
    # strings intentionally have no literal in this repository.
    keys = sorted(rows)

    lines = [
        "/*",
        " * Generated from ui2/i18n/catalog.json.",
        " * Do not edit manually; run ui2/i18n/generate_catalog.py.",
        " */",
        "#include <cstddef>",
        "",
        "namespace recovery_ui2::i18n {",
        "",
        "extern const char *const kCatalogLanguages[] = {",
        '    "en",',
        "#ifdef AERA_EXTRA_LANGUAGES",
    ]
    lines.extend(f"    {quote(locale)}," for locale in LOCALES[1:])
    lines.extend([
        "#endif",
        "    nullptr,",
        "};",
        "",
        "extern const char *const kCatalogTags[] = {",
    ])
    lines.extend(f"    {quote(key)}," for key in keys)
    lines.extend([
        "    nullptr,",
        "};",
        "",
        "extern const char *const kCatalogTranslations[] = {",
    ])
    for key in keys:
        lines.append(f"    {quote(key)},")
        lines.append("#ifdef AERA_EXTRA_LANGUAGES")
        for locale in LOCALES[1:]:
            lines.append(f"    {quote(rows[key].get(locale, key) or key)},")
        lines.append("#endif")
    lines.extend([
        "};",
        "",
        "}  // namespace recovery_ui2::i18n",
        "",
    ])
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {len(keys)} translated AERA source strings.")


if __name__ == "__main__":
    main()
