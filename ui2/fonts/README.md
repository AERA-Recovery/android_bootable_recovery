# AERA font fallbacks

LVGL's built-in Montserrat fonts cover the compact ASCII UI. AERA chains
FreeType fallbacks from `ui2/fonts/assets` for translated text and filenames. Fallback
faces are created lazily for each UI size so the full multilingual set does
not inflate native code or eagerly consume recovery memory.

`aera_fallback.cpp` keeps the active locale's script early in the chain while
retaining coverage for every language shown by the language selector. Missing
font files degrade to the preceding font instead of preventing recovery from
starting.
