from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from nvn_bridge import HEADERS, generate

HEADER = '''#include <stdint.h>
typedef enum { NVNmodeA = 1 } NVNmode;
typedef struct { uint32_t value; } NVNseparateTextureHandle;
typedef struct { uint64_t value; } NVNseparateSamplerHandle;
typedef struct { char bytes[128]; } NVNopaque;
typedef void (*PFNNVNCALLBACKPROC)(void);
'''
PROTOTYPES = '''
typedef uint64_t (*NVNT_wide)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef float (*NVNT_floating)(float, float, float, float, float, float, float, float, float, float);
typedef uint64_t (*NVNT_mixed)(uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float, uint64_t, float);
typedef NVNseparateTextureHandle (*NVNT_small)(NVNseparateTextureHandle, int);
typedef NVNseparateSamplerHandle (*NVNT_handle64)(NVNseparateSamplerHandle);
typedef const void* (*NVNT_pointer)(uint32_t*, int);
typedef NVNmode (*NVNT_enum_value)(int);
typedef PFNNVNCALLBACKPROC (*NVNT_callback)(PFNNVNCALLBACKPROC);
typedef NVNopaque (*NVNT_unsupported)(void);
'''


class NvnBridgeTests(unittest.TestCase):
    def test_guest_tokens_and_callback_state_isolation(self):
        with tempfile.TemporaryDirectory(prefix='graphics-bridge-test-') as temp:
            binary = Path(temp) / 'check'
            subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                            '-I', str(ROOT / 'runtime/graphics/include'), '-I', str(ROOT / 'runtime/graphics/nvn'),
                            str(ROOT / 'runtime/graphics/bridge.c'), str(ROOT / 'tests/native_graphics_bridge.c'),
                            '-o', str(binary)], check=True, capture_output=True, timeout=30)
            for args, code in (([], 0), (['unbalanced'], 78)):
                result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)

    def test_real_api_header_compiles_with_explicit_unsupported_aggregates(self):
        source, unsupported = generate((HEADERS / 'nvn_protos.h').read_text(), (HEADERS / 'include/nvn.h').read_text())
        self.assertEqual(unsupported, ['nvnBufferBuilderGetMemoryPool', 'nvnTextureBuilderGetMemoryPool', 'nvnTextureGetMemoryPool'])
        with tempfile.TemporaryDirectory(prefix='nvn-header-test-') as temp:
            directory = Path(temp)
            path = directory / 'bridge.c'
            path.write_text(source)
            subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'third_party/lasrecomp/runtime'),
                            '-I', str(HEADERS), '-I', str(HEADERS / 'include'),
                            '-c', str(path), '-o', str(directory / 'bridge.o')], check=True, timeout=30, capture_output=True)

    def test_native_register_stack_pointer_and_small_struct_calls(self):
        source, unsupported = generate(PROTOTYPES, HEADER)
        self.assertEqual(unsupported, ['unsupported'])
        with tempfile.TemporaryDirectory(prefix='nvn-abi-test-') as temp:
            directory = Path(temp)
            (directory / 'nvn_protos.h').write_text(HEADER + PROTOTYPES)
            path = directory / 'bridge.c'
            path.write_text(source)
            binary = directory / 'check'
            subprocess.run(['clang', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-I', str(directory), '-I', str(ROOT / 'runtime'),
                            '-I', str(ROOT / 'third_party/lasrecomp/runtime'), str(path),
                            str(ROOT / 'tests/native_nvn_bridge.c'), '-o', str(binary)],
                           check=True, timeout=30, capture_output=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in ('unsupported', 'missing'):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 78, result.stderr)


if __name__ == '__main__':
    unittest.main()
