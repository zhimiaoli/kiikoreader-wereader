#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""把一次构建产物拼成 16 MiB 原始闪存镜像（只能从 0x0 写入）；不连接、不刷写设备。

Pack one build's artifacts into a 16 MiB raw flash image writable only from offset 0x0.
The tool never connects to or flashes a device.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parents[1]


def partitions(board):
    """读取板级分区表 CSV，返回 {名称: (偏移, 大小)}。/ Parse the board partition CSV."""
    result = {}
    text = (ROOT / 'boards' / board / 'partitions.csv').read_text(encoding='utf-8')
    for line in text.splitlines():
        fields = [value.strip() for value in line.split(',')]
        if len(fields) < 5 or not fields[0] or fields[0].startswith('#'):
            continue
        result[fields[0]] = (int(fields[3], 0), int(fields[4], 0))
    return result


def blobs(package):
    """包目录里必须有这四个镜像；缺一个就不能拼全量包。
    The package directory must hold all four images; a full-flash image needs every one."""
    names = {'bootloader': 'bootloader.bin', 'partition-table': 'partitions.bin',
             'otadata': 'ota_data_initial.bin', 'app': 'firmware.bin'}
    missing = [name for name in names.values() if not (package / name).is_file()]
    if missing:
        raise ValueError('package is missing: ' + ', '.join(missing))
    return {key: (package / name).read_bytes() for key, name in names.items()}


def esp_image_ok(data, chip_id, what, app=False):
    """校验 ESP 镜像头，避免把空文件或错芯片的镜像打进全量包。
    0xabcd5432 只出现在应用镜像的扩展头里，引导程序没有，所以只在 app=True 时要求。
    Validate the ESP image header so an empty or wrong-chip image cannot slip in. The
    0xabcd5432 marker only exists in the application extended header, not the bootloader."""
    if len(data) < 288 or data[0] != 0xe9:
        raise ValueError(f'{what}: not an ESP image')
    if app and struct.unpack_from('<I', data, 32)[0] != 0xabcd5432:
        raise ValueError(f'{what}: bad extended header marker')
    if struct.unpack_from('<H', data, 12)[0] != chip_id:
        raise ValueError(f'{what}: chip id does not match the board')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', default='metalio47')
    parser.add_argument('--package', type=Path, required=True,
                        help='目录，含 bootloader.bin/partitions.bin/ota_data_initial.bin/firmware.bin')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--size', type=lambda value: int(value, 0), default=0x1000000,
                        help='镜像总长，默认 0x1000000（16 MiB）')
    args = parser.parse_args()

    info = json.loads((ROOT / 'boards' / args.board / 'board.json').read_text(encoding='utf-8'))
    table = partitions(args.board)
    images = blobs(args.package)

    chip_id = info['image_chip_id']
    esp_image_ok(images['bootloader'], chip_id, 'bootloader')
    esp_image_ok(images['app'], chip_id, 'app', app=True)
    if struct.unpack_from('<H', images['partition-table'], 0)[0] != 0x50aa:
        raise ValueError('partitions.bin: bad partition table magic')

    # 分区表本身不是分区，固定 0x8000；其余地址取自板级 CSV，分区调整后不会悄悄错位。
    # The partition table is not a partition and lives at 0x8000; other offsets come from the
    # board CSV, so a partition change cannot silently misplace a region.
    layout = [('bootloader', info['bootloader_offset']),
              ('partition-table', 0x8000),
              ('otadata', table['otadata'][0]),
              ('app', table['ota_0'][0])]
    if info['bootloader_offset'] + len(images['bootloader']) > 0x8000:
        raise ValueError('bootloader overlaps the partition table at 0x8000')

    image = bytearray(b'\xff' * args.size)
    placed = []
    for key, offset in layout:
        data = images[key]
        if offset + len(data) > args.size:
            raise ValueError(f'{key} does not fit in a {args.size:#x} byte image')
        image[offset:offset + len(data)] = data
        placed.append({'part': key, 'offset': offset, 'size': len(data)})

    for name, (offset, size) in table.items():
        if offset + size > args.size:
            raise ValueError(f'partition {name} exceeds the flash size')

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    summary = {'board': args.board, 'version': info['version'], 'file': args.output.name,
               'size': len(image), 'sha256': digest, 'layout': placed,
               'file_declared_size': str(args.size), 'device_flashed': False}
    print(json.dumps(summary, ensure_ascii=False))
    return summary


if __name__ == '__main__':
    main()
