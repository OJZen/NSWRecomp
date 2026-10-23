"""Small host contracts shared by the build and load-only tools."""
import os
import platform
import shlex
import subprocess
import sys


def lock_file(stream, *, shared=False):
    """Acquire a nonblocking lock, released when the caller closes the file.

    Windows conservatively serializes readers too. Lock the same byte regardless
    of the stream's append/read position, including when the file is empty.
    """
    if os.name == 'nt':
        import msvcrt
        stream.seek(0)
        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(stream, (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | fcntl.LOCK_NB)


def cpu_flags():
    # The CPU helpers use aligned 128-bit compare/exchange. Require CMPXCHG16B
    # on x86-64 instead of silently introducing an unresolved libatomic call.
    return ['-mcx16'] if platform.machine().lower() in ('amd64', 'x86_64') else []


def executable(name):
    return name + ('.exe' if os.name == 'nt' else '')


def library_suffix():
    return '.dll' if os.name == 'nt' else '.dylib' if sys.platform == 'darwin' else '.so'


def response_args(args):
    return subprocess.list2cmdline(args) if os.name == 'nt' else shlex.join(args)


def response_command(path):
    # LLVM's GNU response parser treats backslashes as escapes, even inside
    # quotes. Select Windows parsing explicitly for list2cmdline's output.
    return ['clang', *(['--rsp-quoting=windows'] if os.name == 'nt' else []), '@' + str(path)]


def disable_core_dumps():
    if os.name != 'nt':
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
