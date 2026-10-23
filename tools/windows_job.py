"""Windows Job Object supervision for bounded.py (no third-party dependencies).

The small Python gate cannot start the requested command until it belongs to the
job. EOF before assignment makes it exit. Descendants inherit the job, including
after their parent exits; closing our non-inheritable handle terminates them.
"""
import ctypes as C
from ctypes import wintypes as W
import os
from pathlib import Path
import subprocess
import sys
import time


class BasicLimits(C.Structure):
    _fields_ = [('process_time', C.c_int64), ('job_time', C.c_int64),
                ('flags', W.DWORD), ('min_working_set', C.c_size_t),
                ('max_working_set', C.c_size_t), ('active_processes', W.DWORD),
                ('affinity', C.c_size_t), ('priority', W.DWORD), ('scheduling', W.DWORD)]


class IoCounters(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ('read_ops', 'write_ops', 'other_ops',
                                              'read_bytes', 'write_bytes', 'other_bytes')]


class ExtendedLimits(C.Structure):
    _fields_ = [('basic', BasicLimits), ('io', IoCounters),
                ('process_memory', C.c_size_t), ('job_memory', C.c_size_t),
                ('peak_process_memory', C.c_size_t), ('peak_job_memory', C.c_size_t)]


class MemoryCounters(C.Structure):
    _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [
        (name, C.c_size_t) for name in ('peak_rss', 'rss', 'peak_paged', 'paged',
                                       'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile')]


def api(name, restype, *args):
    fn = getattr(C.WinDLL('kernel32', use_last_error=True), name)
    fn.restype, fn.argtypes = restype, args
    return fn


create_job = api('CreateJobObjectW', W.HANDLE, C.c_void_p, W.LPCWSTR)
set_info = api('SetInformationJobObject', W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD)
query_info = api('QueryInformationJobObject', W.BOOL, W.HANDLE, C.c_int, C.c_void_p, W.DWORD, C.c_void_p)
assign = api('AssignProcessToJobObject', W.BOOL, W.HANDLE, W.HANDLE)
terminate = api('TerminateJobObject', W.BOOL, W.HANDLE, W.UINT)
close = api('CloseHandle', W.BOOL, W.HANDLE)
open_process = api('OpenProcess', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
memory_info = api('K32GetProcessMemoryInfo', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD)
exit_code = api('GetExitCodeProcess', W.BOOL, W.HANDLE, C.POINTER(W.DWORD))


def checked(ok):
    if not ok:
        raise C.WinError(C.get_last_error())
    return ok


class Job:
    def __init__(self):
        self.handle = checked(create_job(None, None))
        try:
            limits = ExtendedLimits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            checked(set_info(self.handle, 9, C.byref(limits), C.sizeof(limits)))
        except BaseException:
            self.close()
            raise

    def start(self, command, *, cwd, env, output):
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--gate', *command],
                                   cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=output,
                                   stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            checked(assign(self.handle, int(process._handle)))
            process.stdin.write(b'G')
            process.stdin.close()
        except BaseException:
            process.stdin.close()
            process.kill()
            process.wait()
            raise
        return process

    def pids(self):
        capacity = 64
        while True:
            class Pids(C.Structure):
                _fields_ = [('assigned', W.DWORD), ('count', W.DWORD),
                            ('values', C.c_size_t * capacity)]
            value = Pids()
            if query_info(self.handle, 3, C.byref(value), C.sizeof(value), None):
                return list(value.values[:value.count])
            if C.get_last_error() != 234:  # ERROR_MORE_DATA
                checked(False)
            capacity = max(capacity * 2, value.assigned)

    def rss(self):
        total = 0
        for pid in self.pids():
            handle = open_process(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
            if not handle:
                if C.get_last_error() == 87:  # exited between enumeration and open
                    continue
                checked(False)
            try:
                counters = MemoryCounters()
                counters.cb = C.sizeof(counters)
                if not memory_info(handle, C.byref(counters), counters.cb):
                    code = W.DWORD()
                    if exit_code(handle, C.byref(code)) and code.value != 259:
                        continue
                    checked(False)
                total += counters.rss
            finally:
                close(handle)
        return total

    def stop(self):
        checked(terminate(self.handle, 1))
        deadline = time.monotonic() + 10
        while self.pids():
            if time.monotonic() >= deadline:
                raise RuntimeError('Windows job descendants did not exit after termination')
            time.sleep(0.01)

    def close(self):
        if self.handle:
            close(self.handle)
            self.handle = None


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] != '--gate':
        raise SystemExit('internal process gate')
    if sys.stdin.buffer.read(1) != b'G':
        raise SystemExit(1)
    # sys.exit truncates some Windows status values. Preserve all 32 bits.
    code = subprocess.call(sys.argv[2:], stdin=subprocess.DEVNULL)
    api('ExitProcess', None, W.UINT)(code & 0xffffffff)
