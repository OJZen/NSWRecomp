"""Lifetime of a synthetic native test library, including Windows dependencies."""
from contextlib import contextmanager, ExitStack
import ctypes
import os
from pathlib import Path
import shutil


@contextmanager
def native_library(path):
    """Callers must stop using all exported functions before leaving the scope."""
    with ExitStack() as stack:
        if os.name == 'nt':
            compiler = shutil.which('clang')
            if compiler:
                stack.enter_context(os.add_dll_directory(str(Path(compiler).resolve().parent)))
        library = ctypes.CDLL(str(path))
        try:
            yield library
        finally:
            if os.name == 'nt':
                from _ctypes import FreeLibrary
                FreeLibrary(library._handle)
