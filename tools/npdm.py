"""Read bounded NPDM client-service access data; this is not signature verification."""
import hashlib
from pathlib import Path
import re
import struct

MAX_SIZE = 64 << 10
MAX_SERVICES = 128


def service_names(data):
    """Decode SAC entries, retaining client names and rejecting unsupported syntax."""
    clients = []
    offset = 0
    count = 0
    while offset < len(data):
        header = data[offset]
        length = (header & 7) + 1
        offset += 1
        count += 1
        if header & 0x78 or length > len(data) - offset or count > MAX_SERVICES:
            raise ValueError('invalid or oversized NPDM service access list')
        raw = data[offset:offset + length]
        offset += length
        if any(c < 33 or c > 126 or c == 44 for c in raw) or b'*' in raw[:-1]:
            raise ValueError('unsupported NPDM service name or wildcard')
        if not header & 0x80:
            name = raw.decode('ascii')
            if name not in clients:
                clients.append(name)
    return clients


def matches(pattern, name):
    return name.startswith(pattern[:-1]) if pattern.endswith('*') else pattern == name


def access_policy(data):
    if len(data) < 0x80 or len(data) > MAX_SIZE or data[:4] != b'META':
        raise ValueError('invalid NPDM META header or size')
    # This runtime implements the 64-bit instruction / 39-bit address-space model.
    if data[0xc] & 15 != 7:
        raise ValueError('NPDM requires an unsupported instruction/address-space model')
    tables = []
    ranges = []
    for field, header_size, magic_at, magic, sac_at in (
            (0x70, 0x40, 0, b'ACI0', 0x28), (0x78, 0x240, 0x200, b'ACID', 0x228)):
        offset, size = struct.unpack_from('<II', data, field)
        if offset < 0x80 or offset > len(data) or size < header_size or size > len(data) - offset:
            raise ValueError('NPDM access-control region is out of bounds')
        if any(offset < end and start < offset + size for start, end in ranges):
            raise ValueError('NPDM access-control regions overlap')
        ranges.append((offset, offset + size))
        region = data[offset:offset + size]
        if region[magic_at:magic_at + 4] != magic:
            raise ValueError('invalid NPDM access-control magic')
        start, length = struct.unpack_from('<II', region, sac_at)
        if start < header_size or start > size or length > size - start:
            raise ValueError('NPDM service access data is out of bounds')
        tables.append(service_names(region[start:start + length]))
    requested, permitted = tables
    for name in requested:
        if not any(matches(pattern, name) for pattern in permitted):
            raise ValueError('NPDM client-service access exceeds its ACID descriptor')
    return requested


def read_policy(path, expected_sha256):
    path = Path(path)
    if not expected_sha256 or not re.fullmatch('[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('basic services require --npdm-sha256 for the selected main.npdm')
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_SIZE:
        raise ValueError('NPDM input must be a regular file of at most 64 KiB')
    with path.open('rb') as stream:
        data = stream.read(MAX_SIZE + 1)
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected_sha256.lower():
        raise ValueError('NPDM fingerprint mismatch')
    names = access_policy(data)
    aci = struct.unpack_from('<I', data, 0x70)[0]
    program = struct.unpack_from('<Q', data, aci + 0x10)[0]
    return {'sha256': digest, 'client_services': names, 'program_id': f'{program:016x}', 'signature_verified': False}
