"""Build the native Metal backend; generated bridges and objects stay under work/."""
import json
from pathlib import Path
import re
import shlex
import subprocess

from bounded import run
from build import signature, write_json
from load import Module, symbol_scope
from nvn_bridge import HEADERS, generate
from recompile import ROOT, VENDOR, Nso, digest, verify
from build_shaders import chain

GFX = ROOT / 'runtime/graphics'
OUTPUT = ROOT / 'work/graphics'
SOURCES = ('bridge.c', 'display.c', 'input.c', 'codec.c', 'movie.c', 'movie_video.c', 'movie_audio.c', 'frontend.m', 'shader_prep.m', 'render_prep.m', 'performance.c', 'gamepad.m', 'control_panel.m', 'nvn/hle_nvn.c', 'nvn/nvn_names.c', 'nvn/nvn_mtl.m',
           'nvn/nvn_refl.m', 'nvn/nvn_tex.c', 'tex/nvn_fmt_mtl.c', 'tex/nvn_bcdec.c',
           'tex/nvn_deswizzle.c', 'tex/nvn_deswizzle_gpu.m', 'include/nvn_dirty.c',
           'include/timeline.c', 'include/trace_writer.c')


def source_hashes():
    files = sorted(GFX.rglob('*')) + [ROOT / 'runtime/nvn_bridge.h', ROOT / 'tools/nvn_bridge.py', Path(__file__)]
    files += [HEADERS / 'nvn_protos.h', HEADERS / 'include/nvn.h', ROOT / 'third_party/zelda-mobile/core/third_party/bcdec/bcdec.h']
    files += sorted((ROOT / 'runtime').glob('*.h')) + sorted((VENDOR / 'runtime').glob('*.h'))
    return {str(p.relative_to(ROOT)): digest(p) for p in files if p.is_file()}


def build(exefs):
    for p in (OUTPUT, OUTPUT / 'include', OUTPUT / 'objects'):
        if p.is_symlink(): raise ValueError('graphics output must not be a symlink')
        p.mkdir(exist_ok=True)
    hashes = source_hashes()
    infos = verify(exefs)
    modules = [Module(name, int(info['base'], 0), Nso(exefs / name)) for name, info in infos.items()]
    # Match guest relocation lookup, including game allocator overrides of SDK defaults.
    exports = {name.decode(): owner.defined_value(symbol)
               for name, (owner, symbol) in symbol_scope(modules).items()
               if symbol[2] == 2 and owner.nso.text[0] <= symbol[5] < sum(owner.nso.text)}
    symbols = re.findall(r'^GUEST\((\w+)\)$', (GFX / 'guest_symbols.inc').read_text(), re.M)
    if not symbols or len(set(symbols)) != len(symbols) or any(name not in exports for name in symbols):
        raise ValueError('missing or duplicate SDK callback helper')
    sdk_source = OUTPUT / 'guest_symbols.c'
    sdk_source.write_text('#include <stdint.h>\n#include <string.h>\nuint64_t graphics_guest_export(const char *name) {\n' +
        ''.join(f'    if (!strcmp(name, "{name}")) return {exports[name]:#x}ull;\n' for name in symbols) + '    return 0;\n}\n')
    del modules
    identity = signature(hashes)
    for name, macro in (('dmsh_chain.h', 'DMSH_CHAIN'), ('pipeline_recipe_identity.h', 'ZELDA_PIPELINE_RECIPE_CONTRACT')):
        value = chain() if name == 'dmsh_chain.h' else identity
        (OUTPUT / 'include' / name).write_text(f'#define {macro} "{value}"\n')
    names = re.findall(r'"(nvn\w+)"', (GFX / 'nvn/nvn_names.c').read_text())
    names += re.findall(r'!strcmp\(name, "(nvn\w+)"\)', (GFX / 'nvn/hle_nvn.c').read_text())
    extra = (GFX / 'nvn/nvn_extra_protos.h').read_text()
    bridge, unsupported = generate((HEADERS / 'nvn_protos.h').read_text() + extra,
                                   (HEADERS / 'include/nvn.h').read_text() + extra, names, 'nvn_extra_protos.h')
    generated = OUTPUT / 'nvn_bridge.c'
    generated.write_text(bridge)
    flags = ['-std=gnu11', '-O2', '-flto=thin', '-g0', '-fno-strict-aliasing', '-fobjc-arc', '-Wno-deprecated-declarations']
    flags += shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'vpx', 'vorbis'], text=True, timeout=10))
    includes = [ROOT / 'runtime', VENDOR / 'runtime', GFX, GFX / 'include', GFX / 'nvn',
                HEADERS, HEADERS / 'include', OUTPUT / 'include', ROOT / 'third_party/zelda-mobile/core/third_party/bcdec']
    for path in includes: flags += ['-I', str(path)]
    compiler = subprocess.check_output(['clang', '--version'], text=True, timeout=10)
    context = signature(dict(sources=hashes, compiler=compiler, flags=flags, bridge=digest(generated), sdk=digest(sdk_source),
                             shader_chain=digest(OUTPUT / 'include/dmsh_chain.h')))
    objects, metrics = [], []
    for source in [*(GFX / name for name in SOURCES), generated, sdk_source]:
        obj = OUTPUT / 'objects' / (source.stem + '.o')
        receipt = obj.with_suffix('.json')
        if obj.is_symlink() or receipt.is_symlink(): raise ValueError('graphics object must not be a symlink')
        old = json.loads(receipt.read_text()) if receipt.is_file() else {}
        if old.get('context') != context or not obj.is_file() or old.get('sha256') != digest(obj):
            partial = obj.with_suffix('.partial')
            try:
                metrics.append(run(['clang', *flags, '-c', str(source), '-o', str(partial)], cwd=ROOT,
                                   log=ROOT / 'work/logs' / ('graphics-' + source.stem + '.log'), seconds=120))
                partial.replace(obj)
                write_json(receipt, dict(context=context, sha256=digest(obj)))
            finally:
                partial.unlink(missing_ok=True)
        objects.append(obj)
    result = dict(sources=hashes, context=context, unsupported_abi=unsupported, metrics=metrics,
                  objects={str(p.relative_to(ROOT)): digest(p) for p in objects})
    write_json(OUTPUT / 'result.json', result)
    return objects, result


def link_flags():
    return shlex.split('-flto=thin -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal '
                       '-framework MetalFX -framework ImageIO -framework UniformTypeIdentifiers -framework GameController') + shlex.split(
                           subprocess.check_output(['pkg-config', '--libs', 'vpx', 'vorbis'], text=True, timeout=10))
