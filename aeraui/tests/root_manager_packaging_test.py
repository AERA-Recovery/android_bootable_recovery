#!/usr/bin/env python3
"""Exercise actual packaging rules in temporary fixtures, never Android out/."""
import os
import hashlib
import json
import shutil
import pathlib
import re
import subprocess
import tempfile
import unittest

RECOVERY = pathlib.Path(__file__).resolve().parents[2]
VENDOR = RECOVERY.parents[1] / 'vendor/recovery'


class RootPackagingTest(unittest.TestCase):
    def test_soong_ui_and_host_receive_the_same_feature_flag(self):
        source = (RECOVERY / 'aeraui/build/aeraui_defaults.go').read_text()
        for hook in ('aeraUiDefaults', 'globalFlags'):
            body = re.search(r'func ' + hook + r'\([^\n]+\{\n(.*?)\n\}', source, re.S)
            self.assertIsNotNone(body, hook)
            self.assertIn('aeraRootManagerFlags(ctx)', body.group(1), hook)
        self.assertIn('getMakeVars(ctx, "AERA_ENABLE_ROOT_MANAGER") == "1"', source)

    def test_android_kati_configuration_and_vendor_argument(self):
        kati = RECOVERY.parents[1] / 'prebuilts/build-tools/linux-x86/bin/ckati'
        if not kati.is_file():
            self.skipTest('Android Kati prebuilt is not available')
        source = (RECOVERY / 'aera_build.mk').read_text()
        bridge = source.split('LOCAL_CFLAGS += -DAERA_ENABLE_ROOT_MANAGER=', 1)[1]
        bridge = 'LOCAL_CFLAGS += -DAERA_ENABLE_ROOT_MANAGER=' + bridge.split('# mask read errors', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-root-kati-') as tmp:
            makefile = pathlib.Path(tmp) / 'fixture.mk'
            makefile.write_text('$(KATI_obsolete_export Exports are prohibited)\ninclude ' +
                                str(RECOVERY / 'aera_config.mk') + '\nAERA_VENDOR := vendor-hook.sh\n' +
                                bridge + '\n.PHONY: all\nall:\n\t@printf "%s\\n" $(AERA_VENDOR)\n')
            environment = os.environ.copy()
            environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
            for flag in ('1', '0', None):
                command = [str(kati), '-f', str(makefile), 'all']
                if flag is not None:
                    command.append('AERA_ENABLE_ROOT_MANAGER=' + flag)
                output = subprocess.check_output(command, env=environment, text=True)
                self.assertIn('AERA_ENABLE_ROOT_MANAGER=' + ('1' if flag == '1' else '0'), output)

    def test_incremental_provider_selection_and_disable(self):
        source = (RECOVERY / 'aera_build.mk').read_text()
        rules = source.split('# Root Manager is one optional feature;', 1)[1]
        rules = '# Root Manager is one optional feature;' + rules.split('# AERA Remote', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-root-packaging-') as tmp:
            root = pathlib.Path(tmp)
            assets = root / 'source/prebuilt/aera/root'
            assets.mkdir(parents=True)
            tools = root / 'source/tools'
            tools.mkdir(parents=True)
            shutil.copy2(RECOVERY / 'tools/package_root_modules.py', tools)
            catalog = {'schema': 1, 'providers': []}
            providers = ('kernelsu', 'next', 'sukisu')
            for provider in providers:
                entries = []
                (assets / provider).mkdir()
                for version in ('6.1', '6.12'):
                    name = 'android16-' + version + '_kernelsu.ko'
                    data = (provider + version).encode()
                    (assets / provider / name).write_bytes(data)
                    entries.append({'name': name, 'kmi': 'android16-' + version,
                                    'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                catalog['providers'].append({'id': provider, 'assets': entries})
            (assets / 'providers.json').write_text(json.dumps(catalog))
            makefile = root / 'fixture.mk'
            makefile.write_text('include ' + str(RECOVERY / 'aera_config.mk') + '\n' +
                                rules + '\n.PHONY: all\nall: $(AERA_ROOT_ASSETS_STAMP)\n')
            environment = os.environ.copy()
            environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
            environment.pop('AERA_ROOT_MODULE_KERNELS', None)
            cases = [('1', None, ['6.1', '6.12']), ('1', '6.12', ['6.12']),
                     ('1', '6.1', ['6.1']), ('1', '6.1 6.12', ['6.1', '6.12']),
                     ('1', 'all', ['6.1', '6.12']), ('0', '6.12', []),
                     ('1', '6.12', ['6.12']), (None, None, [])]
            base_command = ['make', '--no-print-directory', '-f', str(makefile), 'all',
                            'TARGET_ARCH=arm64', 'commands_TWRP_local_path=' + str(root / 'source'),
                            'TARGET_RECOVERY_ROOT_OUT=' + str(root / 'ramdisk')]
            for flag, selection, versions in cases:
                command = base_command.copy()
                if flag is not None:
                    command.append('AERA_ENABLE_ROOT_MANAGER=' + flag)
                if selection is not None:
                    command.append('AERA_ROOT_MODULE_KERNELS=' + selection)
                subprocess.run(command, env=environment, check=True, capture_output=True)
                output = root / 'ramdisk/system/etc/aera'
                staged = output / 'root-providers'
                self.assertEqual(staged.is_dir(), flag == '1')
                if flag == '1':
                    metadata = json.loads((staged / 'providers.json').read_text())
                    self.assertEqual(len(list(staged.rglob('*.ko'))), 3 * len(versions))
                    for provider in metadata['providers']:
                        self.assertEqual(sorted(asset['kmi'].rsplit('-', 1)[-1]
                                                for asset in provider['assets']), versions)
                        for asset in provider['assets']:
                            data = (staged / provider['id'] / asset['name']).read_bytes()
                            self.assertEqual(hashlib.sha256(data).hexdigest(), asset['sha256'])
                stamps = list(output.glob('.root-providers*.stamp'))
                self.assertEqual(len(stamps), 1)
                suffix = '-' + '_'.join(sorted(selection.split())) if flag == '1' and selection else ''
                self.assertEqual(stamps[0].name, '.root-providers-' +
                                 ('enabled' if flag == '1' else 'disabled') + suffix + '.stamp')
            for selection in ('6.99', '6.12.1', 'all 6.12'):
                result = subprocess.run(base_command + ['AERA_ENABLE_ROOT_MANAGER=1',
                                        'AERA_ROOT_MODULE_KERNELS=' + selection],
                                        env=environment, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, selection)
                self.assertIn('Root module packaging failed', result.stderr)

    def test_ksud_and_obsolete_addons(self):
        source = (VENDOR / 'AERA_A16.sh').read_text()
        block = source.split('  # Clear obsolete installer addons', 1)[1]
        block = '  # Clear obsolete installer addons' + block.split('  # enable the app manager?', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-ksud-packaging-') as tmp:
            root = pathlib.Path(tmp)
            binary = root / 'vendor/prebuilt/arm64/ksud'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'fixture ksud')
            ramdisk = root / 'ramdisk'
            (ramdisk / 'sbin').mkdir(parents=True)
            environment = os.environ.copy()
            environment.update(AERA_RAMDISK=str(ramdisk), AERA_VENDOR_PATH=str(root / 'vendor'),
                               TARGET_ARCH='arm64', RAMDISK_SBIN='sbin', CP='/bin/cp', GREEN='', NC='')
            for flag in ('1', '0', '1', None):
                if flag is None:
                    environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
                else:
                    environment['AERA_ENABLE_ROOT_MANAGER'] = flag
                legacy = ramdisk / 'AERA/Files/KernelSU'
                legacy.mkdir(parents=True)
                (legacy / 'old-installer.zip').write_bytes(b'obsolete')
                subprocess.run(['bash', '-e', '-c', block], env=environment, check=True,
                               stdout=subprocess.PIPE)
                self.assertFalse(legacy.exists())
                ksud = ramdisk / 'sbin/ksud'
                self.assertEqual(ksud.exists(), flag == '1')
                if flag == '1':
                    self.assertEqual(ksud.read_bytes(), binary.read_bytes())
                    self.assertTrue(os.access(ksud, os.X_OK))


if __name__ == '__main__':
    unittest.main()
