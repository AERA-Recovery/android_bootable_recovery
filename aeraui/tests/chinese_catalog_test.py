"""Keep Chinese interface coverage and printf arguments in sync with native UI."""
import ast
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / 'aeraui/localization/catalog'
FORMAT = re.compile(r'%(?:[0-9]+\$)?[-+#0 ]*(?:[0-9]+|\*)?(?:\.(?:[0-9]+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%]')


class ChineseCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = json.loads((CATALOG / 'catalog.json').read_text(encoding='utf-8'))['strings']

    def test_all_catalog_entries_have_both_chinese_locales_and_matching_arguments(self):
        for tag, translations in self.rows.items():
            for locale in ('zh_CN', 'zh_TW'):
                with self.subTest(tag=tag, locale=locale):
                    self.assertTrue(translations.get(locale))
                    self.assertEqual(FORMAT.findall(tag), FORMAT.findall(translations[locale]))

    def test_direct_native_ui_messages_have_chinese_translations(self):
        # Commands, paths, numeric formats and units must remain literal.
        literal_values = {'%u%%', '0 KB/s', '/sdcard', '%zu / %zu', '%02d',
                          '0.0 MB/s', 'adb sideload package.zip', 'adb pair'}
        literal = r'"(?:\\.|[^"\\])*"'
        patterns = [r'i18n::(?:Translate|Format)\(\s*((?:' + literal + r'\s*)+)',
                    r'\b(?:Label|Button|Header|Kicker|Sheet|Row|BindLabel)\([^,\n]+,\s*((?:' + literal + r'\s*)+)']
        for path in (ROOT / 'aeraui').rglob('*.cpp'):
            if any(part in path.parts for part in ('tests', 'localization', 'third_party')):
                continue
            source = path.read_text(encoding='utf-8')
            for pattern in patterns:
                for match in re.finditer(pattern, source):
                    tag = ''.join(ast.literal_eval(token) for token in re.findall(literal, match[1]))
                    if not re.search('[A-Za-z]', tag) or tag in literal_values:
                        continue
                    with self.subTest(path=str(path.relative_to(ROOT)), tag=tag):
                        self.assertTrue(tag in self.rows, f"Missing catalog entry: {tag}")
        for tag in ('Backup', 'Backups', 'Bootloader', 'Wipe', 'Root Manager',
                    'Plugin Manager', 'Slot %s', 'Theme Engine', 'Recovery Update'):
            for locale in ('zh_CN', 'zh_TW'):
                self.assertNotEqual(self.rows[tag][locale], tag)

    def test_compiled_catalog_is_regenerated(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'catalog.cpp'
            subprocess.run([sys.executable, str(CATALOG / 'generate_catalog.py'),
                            '--recovery', str(ROOT), '--output', str(output)],
                           check=True, capture_output=True)
            self.assertEqual(output.read_text(encoding='utf-8'),
                             (CATALOG / 'catalog_generated.cpp').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
