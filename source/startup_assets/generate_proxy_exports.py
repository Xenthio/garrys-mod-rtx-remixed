"""Pin a D3D9 startup proxy export table to the user's exact renderer binary."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def read_exports(data):
    if data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    optional = pe + 24
    if machine != 0x8664 or struct.unpack_from('<H', data, optional)[0] != 0x20b:
        raise ValueError('The startup proxy supports PE32+ x64 renderers only')
    ranges = []
    for index in range(sections):
        offset = optional + optional_size + 40 * index
        virtual_size, address, raw_size, raw = struct.unpack_from('<IIII', data, offset + 8)
        ranges.append((address, max(virtual_size, raw_size), raw, raw_size))

    def physical(rva):
        for address, size, raw, raw_size in ranges:
            if address <= rva < address + size:
                if rva - address >= raw_size:
                    raise ValueError('Export RVA lies outside stored section bytes')
                return raw + rva - address
        raise ValueError(f'Unmapped export RVA: {rva:x}')

    export_rva, export_size = struct.unpack_from('<II', data, optional + 112)
    directory = physical(export_rva)
    base, count, named, funcs, names, ordinals = struct.unpack_from('<IIIIII', data, directory + 16)
    if count > 4096 or count != named:
        raise ValueError('Refuse renderer with unnamed exports: explicit audit required')
    funcs, names, ordinals = physical(funcs), physical(names), physical(ordinals)
    result = []
    seen = set()
    for index in range(named):
        name_offset = physical(struct.unpack_from('<I', data, names + index * 4)[0])
        end = data.index(b'\0', name_offset)
        name = data[name_offset:end].decode('ascii')
        if not name.replace('_', '').isalnum():
            raise ValueError(f'Unsupported export spelling {name!r}')
        relative_ordinal = struct.unpack_from('<H', data, ordinals + index * 2)[0]
        if relative_ordinal >= count or relative_ordinal in seen:
            raise ValueError('Duplicate or invalid export ordinal')
        seen.add(relative_ordinal)
        function_rva = struct.unpack_from('<I', data, funcs + relative_ordinal * 4)[0]
        if not function_rva or export_rva <= function_rva < export_rva + export_size:
            raise ValueError('Renderer already contains forwarded/empty exports; audit required')
        result.append({'name': name, 'ordinal': base + relative_ordinal, 'rva': function_rva})
    if not {'Direct3DCreate9', 'Direct3DCreate9Ex', 'remixapi_InitializeLibrary'} <= {e['name'] for e in result}:
        raise ValueError('Renderer does not expose the expected D3D9/Remix API')
    return sorted(result, key=lambda e: e['ordinal'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('renderer', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).parent)
    args = parser.parse_args()
    data = args.renderer.read_bytes()
    exports = read_exports(data)
    own = ['AstraStartupStatusJson', 'AstraDisableStartupMap']
    if set(own) & {e['name'] for e in exports}:
        raise ValueError('Input is already an Astra proxy')
    lines = ['LIBRARY d3d9', 'EXPORTS']
    for item in exports:
        name, ordinal = item['name'], item['ordinal']
        target = '' if name in ('Direct3DCreate9', 'Direct3DCreate9Ex') else f'=d3d9_astra_renderer.{name}'
        lines.append(f'    {name}{target} @{ordinal}')
    for ordinal, name in enumerate(own, max(e['ordinal'] for e in exports) + 1):
        lines.append(f'    {name} @{ordinal}')
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'proxy_exports.def').write_text('\n'.join(lines) + '\n', encoding='ascii')
    (args.output / 'proxy_renderer_exports.json').write_text(json.dumps({
        'version': 1, 'renderer_sha256': hashlib.sha256(data).hexdigest(),
        'renderer_bytes': len(data), 'original_name': 'd3d9_astra_renderer.dll',
        'exports': exports, 'own_exports': own}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'exports': len(exports), 'renderer_sha256': hashlib.sha256(data).hexdigest()}))


if __name__ == '__main__':
    main()
