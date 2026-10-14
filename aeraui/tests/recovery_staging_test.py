import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT.parents[1]

# The fixtures retain the install rules from AERA recovery e04e0f5d:
# Android.mk (file_contexts_text) and prebuilt/Android.mk (nano_twrp/bash_fox).

# Pause Nano after it creates a terminfo subdirectory. Let the other real
# install command remove it, then resume a real cp into the missing directory.
# This makes the race from the failed parallel build reproducible.
COPY_WRAPPER = '''#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import sys
import time
args = sys.argv[1:]
owner = os.environ.get('STAGE_OWNER', '')
markers = Path(os.environ['MARKERS'])
tool = Path(sys.argv[0]).name
collision = os.environ['COLLIDE'] == '1'
def wait(name):
    limit = time.monotonic() + 8
    while not (markers / name).exists():
        if time.monotonic() > limit:
            raise RuntimeError('Missing race barrier: ' + name)
        time.sleep(0.01)
if tool == 'cp' and 'external/libncurses/lib/terminfo' in args:
    (markers / ('writer-' + owner)).touch()
    if collision and owner == 'nano_twrp':
        destination = Path(args[-1]) / 'terminfo/a'
        destination.mkdir(parents=True, exist_ok=True)
        (markers / 'nano-copy-started').touch()
        wait('common-removed-directory')
        status = subprocess.run(['/bin/cp', 'external/libncurses/lib/terminfo/a/ansi',
                                 str(destination / 'ansi')]).returncode
        (markers / 'nano-copy-finished').touch()
        sys.exit(status)
    if collision and owner == 'file_contexts_text':
        wait('nano-copy-finished')
if tool == 'rm' and owner == 'file_contexts_text' and args[-1].endswith('/terminfo'):
    if collision:
        wait('nano-copy-started')
    subprocess.run(['/bin/rm'] + args, check=True)
    (markers / 'common-removed-directory').touch()
    sys.exit(0)
sys.exit(subprocess.run(['/bin/' + tool] + args).returncode)
'''


class RecoveryStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.checkout = self.base / 'aera/bootable/recovery'
        shutil.copytree(ROOT / 'fixtures/recovery-staging', self.checkout)
        for path in self.checkout.rglob('*'):
            if path.is_file():
                path.chmod(0o644)

    def tearDown(self):
        self.temp.cleanup()

    def repair(self):
        for name, modules in (('Android.mk', ('file_contexts_text',)),
                              ('prebuilt/Android.mk', ('nano_twrp', 'bash_fox'))):
            actual = (SOURCE / name).read_text()
            fixture = (self.checkout / name).read_text()
            for module in modules:
                marker = 'LOCAL_MODULE := ' + module
                begin = actual.rfind('include $(CLEAR_VARS)', 0, actual.index(marker))
                end = actual.index('include $(BUILD_PHONY_PACKAGE)', actual.index(marker))
                end += len('include $(BUILD_PHONY_PACKAGE)')
                old_begin = fixture.rfind('include $(CLEAR_VARS)', 0, fixture.index(marker))
                old_end = fixture.index('include $(BUILD_PHONY_PACKAGE)', fixture.index(marker))
                old_end += len('include $(BUILD_PHONY_PACKAGE)')
                fixture = fixture[:old_begin] + actual[begin:end] + fixture[old_end:]
            (self.checkout / name).write_text(fixture)

    def parallel_install(self, patched, nano=True, bash=True):
        if patched:
            self.repair()
        root = self.base / 'install'
        root.mkdir()
        data = {'external/libncurses/lib/terminfo/a/ansi': 'ANSI database',
                'external/libncurses/lib/terminfo/x/xterm-256color': 'Xterm database',
                'out/etc/nano/nanorc': 'Nano configuration',
                'external/bash/etc/bashrc': 'ro.lineage.device\nexport TERM=xterm\nkeep-bash-config\n',
                'soong/.intermediates/system/sepolicy/file_contexts.concat.tmp/android_common/gen/file_contexts.concat.tmp': 'contexts',
                'out/obj/ETC/file_contexts.bin_intermediates/file_contexts.bin': 'compiled contexts'}
        for name, contents in data.items():
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(contents)
        (root / 'recovery/root').mkdir(parents=True)
        (root / 'external/libncurses/lib/terminfo/x/xterm').symlink_to('xterm-256color')
        shutil.copytree(self.checkout, root / 'recovery-source', ignore=shutil.ignore_patterns('.git'))
        (root / 'clear.mk').write_text('LOCAL_MODULE :=\nLOCAL_POST_INSTALL_CMD :=\n')
        (root / 'phony.mk').write_text(
            '$(LOCAL_MODULE): PRIVATE_COMMAND := $(LOCAL_POST_INSTALL_CMD)\n'
            '$(LOCAL_MODULE): export STAGE_OWNER := $(LOCAL_MODULE)\n'
            '.PHONY: $(LOCAL_MODULE)\n$(LOCAL_MODULE):\n\t@$(PRIVATE_COMMAND)\n')
        (root / 'Makefile').write_text(
            '.DEFAULT_GOAL := all\nSHELL := /bin/bash\n.SHELLFLAGS := -ec\n'
            'CLEAR_VARS := clear.mk\nBUILD_PHONY_PACKAGE := phony.mk\n'
            'SOONG_OUT_DIR := soong\nPRODUCT_OUT := out\n'
            'TARGET_OUT_SYSTEM_EXT_ETC := out/etc\nTARGET_RECOVERY_ROOT_OUT := recovery/root\n'
            'include recovery-source/Android.mk\ninclude recovery-source/prebuilt/Android.mk\n'
            'all: file_contexts_text $(if $(filter true,$(TW_EXCLUDE_NANO)),,nano_twrp) '
            '$(if $(filter 1,$(AERA_BUILD_BASH)),bash_fox)\n')
        wrappers = root / 'wrappers'
        wrappers.mkdir()
        for name in ('cp', 'rm'):
            executable = wrappers / name
            executable.write_text(COPY_WRAPPER)
            executable.chmod(0o755)
        markers = root / 'markers'
        markers.mkdir()
        environment = dict(os.environ, PATH=str(wrappers) + os.pathsep + os.environ['PATH'],
                           MARKERS=str(markers), COLLIDE='0' if patched else '1')
        result = subprocess.run(['make', '--no-print-directory', '-j3',
                                 'TW_EXCLUDE_NANO=' + ('false' if nano else 'true'),
                                 'AERA_BUILD_BASH=' + ('1' if bash else '0')],
                                cwd=root, env=environment, capture_output=True, text=True, timeout=20)
        return result, root, markers

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('make'), 'Native parallel Make install')
    def test_original_parallel_install_loses_directory_during_copy(self):
        result, _, _ = self.parallel_install(False, bash=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No such file or directory', result.stderr)
        self.assertIn('nano_twrp', result.stderr)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('make'), 'Native parallel Make install')
    def test_single_owner_keeps_terminal_database_nano_and_bash(self):
        result, root, markers = self.parallel_install(True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name for p in markers.glob('writer-*')}, {'writer-file_contexts_text'})
        self.assertEqual((root / 'recovery/root/system/etc/nano/nanorc').read_text(), 'Nano configuration')
        self.assertEqual((root / 'recovery/root/system/etc/bash/bashrc').read_text(),
                         'ro.product.device\nkeep-bash-config\n')
        self.assertEqual(os.readlink(root / 'recovery/root/sbin/bash'), '/system/bin/bash')
        self.check_database(root)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('make'), 'Native parallel Make install')
    def test_database_is_staged_without_optional_nano_or_bash(self):
        result, root, _ = self.parallel_install(True, nano=False, bash=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_database(root)

    def check_database(self, root):
        source = root / 'external/libncurses/lib/terminfo'
        target = root / 'recovery/root/system/etc/terminfo'
        source_files = {p.relative_to(source) for p in source.rglob('*') if not p.is_dir()}
        self.assertEqual({p.relative_to(target) for p in target.rglob('*') if not p.is_dir()}, source_files)
        for name in source_files:
            if (source / name).is_symlink():
                self.assertEqual(os.readlink(target / name), os.readlink(source / name))
            else:
                self.assertEqual((target / name).read_bytes(), (source / name).read_bytes())


if __name__ == '__main__':
    unittest.main()
