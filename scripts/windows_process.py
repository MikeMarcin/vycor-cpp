# Copyright (c) 2026 The vycor-cpp Authors
# SPDX-License-Identifier: Apache-2.0
"""Native process/console primitives for integration tests (no ps/taskkill shell)."""
import ctypes
from ctypes import wintypes
import subprocess
import sys
import time

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
class ProcessEntry(ctypes.Structure):
    _fields_ = [('size', wintypes.DWORD), ('usage', wintypes.DWORD),
                ('pid', wintypes.DWORD), ('heap', ctypes.c_size_t),
                ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                ('parent', wintypes.DWORD), ('priority', wintypes.LONG),
                ('flags', wintypes.DWORD), ('exe', wintypes.WCHAR * 260)]
kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel.Process32NextW.argtypes = kernel.Process32FirstW.argtypes
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def children_of(pid):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        ok = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        children = []
        while ok:
            if entry.parent == pid:
                children.append(entry.pid)
            ok = kernel.Process32NextW(snapshot, ctypes.byref(entry))
        return children
    finally:
        kernel.CloseHandle(snapshot)


def alive(pid):
    handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return False
    try:
        return kernel.WaitForSingleObject(handle, 0) == 258
    finally:
        kernel.CloseHandle(handle)


def send_console(pid, event):
    # Attach in a separate process: the test runner's console stays untouched.
    subprocess.run([sys.executable, __file__, str(pid), str(event)], check=True,
                   timeout=10)


if __name__ == '__main__':
    kernel.FreeConsole()
    if not kernel.AttachConsole(int(sys.argv[1])):
        raise ctypes.WinError(ctypes.get_last_error())
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)(lambda event: True)
    if not kernel.SetConsoleCtrlHandler(callback, True):
        raise ctypes.WinError(ctypes.get_last_error())
    if not kernel.GenerateConsoleCtrlEvent(int(sys.argv[2]), 0):
        raise ctypes.WinError(ctypes.get_last_error())
    time.sleep(0.3)
