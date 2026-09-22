# AERA native translations

`catalog.json` is the source of truth for native AERA UI translations. English
UI text is also the stable lookup tag, which guarantees a readable English
fallback when a locale or individual translation is missing. The catalog uses
a fixed language order because LVGL's static translation pack stores values as
compact rows rather than repeating locale names for every string.

Regenerate the compiled catalog after editing translations:

```sh
python3 ui2/i18n/generate_catalog.py \
  --recovery . \
  --output ui2/i18n/catalog_generated.cpp
```

The generator emits only catalog entries that are still referenced by native
UI2 source. The legacy XML theme tree is no longer packaged; translations
belong only in this native catalog.

Host API 2 plugins receive the selected locale through `AERA_LOCALE`. Official
plugin metadata may also provide localized `name` and `description` fields in
its manifest. Both mechanisms retain English fallback behavior.
