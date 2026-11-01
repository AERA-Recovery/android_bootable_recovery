#!/usr/bin/env python3
"""Stage only the requested kernel versions and their matching provider catalog."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile


def package(source, output, kernels):
    selected = set(kernels)
    if 'all' in selected:
        if selected != {'all'}:
            raise ValueError("'all' cannot be combined with kernel versions")
        selected = set()
    if any(not re.fullmatch(r'[0-9]+\.[0-9]+', version) for version in selected):
        raise ValueError('kernel versions must use major.minor, for example 6.12')
    raw_catalog = (source / 'providers.json').read_bytes()
    catalog = json.loads(raw_catalog)
    if catalog.get('schema') != 1 or not catalog.get('providers'):
        raise ValueError('invalid root provider catalog')
    files = []
    for provider in catalog['providers']:
        provider_id = provider['id']
        if not re.fullmatch(r'[a-z0-9-]+', provider_id):
            raise ValueError('invalid provider ID')
        assets = provider['assets']
        available = {asset['kmi'].rsplit('-', 1)[-1] for asset in assets}
        missing = selected - available
        if missing:
            raise ValueError(f"{provider_id} has no modules for {', '.join(sorted(missing))}")
        provider['assets'] = [asset for asset in assets
                              if not selected or asset['kmi'].rsplit('-', 1)[-1] in selected]
        for asset in provider['assets']:
            name = asset['name']
            if Path(name).name != name or not name.endswith('.ko'):
                raise ValueError(f'invalid module filename: {name}')
            path = source / provider_id / name
            data = path.read_bytes()
            if len(data) != asset['size'] or hashlib.sha256(data).hexdigest() != asset['sha256']:
                raise ValueError(f'module does not match catalog: {path}')
            files.append((path, Path(provider_id) / name))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.aera-root-modules-', dir=output.parent) as tmp:
        stage = Path(tmp) / 'root-providers'
        stage.mkdir()
        for source_file, relative in files:
            destination = stage / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_file, destination)
        content = (json.dumps(catalog, indent=2) + '\n').encode() if selected else raw_catalog
        (stage / 'providers.json').write_bytes(content)
        if output.exists():
            shutil.rmtree(output)
        stage.rename(output)
    print(f"Packed {len(files)} root modules for {' '.join(sorted(selected)) or 'all kernels'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--kernels', nargs='*', default=[])
    args = parser.parse_args()
    try:
        package(args.source, args.output, args.kernels)
    except (KeyError, OSError, TypeError, ValueError) as error:
        parser.exit(1, f'Root module packaging failed: {error}\n')


if __name__ == '__main__':
    main()
