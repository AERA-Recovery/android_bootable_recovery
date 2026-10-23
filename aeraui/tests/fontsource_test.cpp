/* SPDX-License-Identifier: Apache-2.0 */
#include "fonts/fontsource.hpp"
#include <cassert>
#include <iostream>

using namespace aeraui::fontsource;
int main() {
  const std::string catalog = R"([
    {"id":"roboto","family":"Roboto","category":"sans-serif","subsets":["latin","cyrillic"]},
    {"id":"abel","family":"Abel","category":"sans-serif","subsets":["latin"]},
    {"id":"mono","family":"Mono","category":"monospace","subsets":["latin"]},
    {"id":"bad/path","family":"Unsafe","category":"serif","subsets":["latin"]}
  ])";
  std::string error;
  std::vector<Family> families;
  assert(ParseCatalog(catalog, families, error));
  assert(families.size() == 3 && families.front().name == "Abel");
  assert(Filter(families, "ROB", "").size() == 1);
  assert(Filter(families, "", "monospace").size() == 1);
  assert(Filter(families, "no result", "").empty());
  const std::string details = R"({
    "id":"roboto","family":"Roboto","category":"sans-serif","subsets":["latin","cyrillic"],
    "license":"OFL-1.1","npmVersion":"5.3.1",
    "variants":{
      "400":{"normal":{
        "latin":{"url":{"ttf":"https://cdn.jsdelivr.net/fontsource/fonts/roboto@latest/latin-400-normal.ttf"}},
        "cyrillic":{"url":{"ttf":"https://cdn.jsdelivr.net/fontsource/fonts/roboto@latest/cyrillic-400-normal.ttf"}}}},
      "700":{"italic":{"latin":{"url":{"ttf":"https://cdn.jsdelivr.net/fontsource/fonts/roboto@latest/latin-700-italic.ttf"}}}},
      "900":{"normal":{"latin":{"url":{"ttf":"https://evil.invalid/not-a-font.ttf"}}}}
    }
  })";
  Details font;
  assert(ParseDetails(details, "roboto", font, error));
  assert(font.faces.size() == 3 && font.license == "OFL-1.1");
  auto en = FacesForLanguage(font, "en");
  auto ru = FacesForLanguage(font, "ru");
  assert(en.size() == 2 && en[0].weight == 400 && en[0].style == "normal");
  assert(ru.size() == 1 && ru[0].subset == "cyrillic");
  assert(en[0].url.find("@5.3.1/") != std::string::npos);
  assert(!ParseDetails(details, "different-id", font, error));
  assert(!ParseDetails("{}", "roboto", font, error));
  assert(!ParseCatalog("not json", families, error));
  assert(!ParseCatalog("[]", families, error));
  assert(!FontUrl("http://cdn.jsdelivr.net/fontsource/fonts/test.ttf"));
  assert(!FontUrl("https://cdn.jsdelivr.net.evil/fontsource/fonts/test.ttf"));
  assert(!FontUrl("https://cdn.jsdelivr.net/fontsource/fonts/../test.ttf"));
  assert(!TtfBytes("<html>error</html>"));
  assert(TtfBytes(std::string("\0\1\0\0", 4) + std::string(20, '\0')));
  assert(TtfBytes("OTTO1234567890123456"));
  std::cout << "Fontsource: catalog, filtering, variants, language, pinned URLs and rejection passed\n";
}
