#!/usr/bin/env python3
"""Write local Vulkan manifests for the available 64/32-bit layer libraries."""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def prepare(destination, platform=None):
    platform = platform or ('windows' if sys.platform == 'win32' else 'linux')
    if platform not in ('linux', 'windows'):
        raise ValueError('platform must be linux or windows')
    destination = pathlib.Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    template = json.loads((ROOT / 'src/layer/VkLayer_dlss_nr.json').read_text())
    written = []
    filenames = (('nr_layer.dll', 'nr_layer32.dll') if platform == 'windows'
                 else ('libnr_layer.so', 'libnr_layer32.so'))
    for arch, filename, manifest_name in (
            ('64', filenames[0], 'VkLayer_dlss_nr.json'),
            ('32', filenames[1], 'VkLayer_dlss_nr32.json')):
        library = ROOT / 'work' / filename
        if not library.exists():
            continue
        manifest = dict(template, layer=dict(template['layer'], library_path=str(library), library_arch=arch))
        target = destination / manifest_name
        target.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
        written.append(target)
    return written


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=pathlib.Path)
    parser.add_argument('--platform', choices=('linux', 'windows'),
                        help='default: the current operating system')
    args = parser.parse_args()
    written = prepare(args.destination, args.platform)
    if not written:
        parser.exit(1, 'No matching layer library found in work/; build it first.\n')
    for path in written:
        print(path)
