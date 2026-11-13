#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stacks of every thread of the foreground app, through ps5debug-NG's debugger (port 744).

  sudo python3 tools/ps5stacks.py listen            # in another shell first: the interrupt port 755
  python3 tools/ps5stacks.py dump ELF [-o OUT.txt]  # attach, per thread: suspend, registers, stack, resume; detach

ELF is the unstripped llvm-pie.elf of the build that runs. For each thread the 32 KiB above its
stack pointer are scanned for values that point into the app's code just after a call instruction
(the code itself cannot be read on the console; the ELF says what a call is), and those return
addresses are named, nearest first, like tools/symbolize-crash.py does for crash reports. A thread
blocked in the kernel shows rip in libkernel and its emulator callers below.

Status (2026-11-11): attaching works, but on a hung app the thread list fails (ps5debug-NG stops
the process and waits for it; the wait gave up and the session broke). For hangs use the app's own
watchdog instead: EDEN_HANG_STACKS=1 (headless/hang_stacks.h) and tools/symbolize-hang.py.
"""
import argparse
import importlib.util
import socket
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('ps5debug', HERE / 'ps5debug.py')
ps5debug = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps5debug)
spec = importlib.util.spec_from_file_location('symcrash', HERE / 'symbolize-crash.py')
symcrash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(symcrash)

ATTACH, DETACH, THREADS, SUSPEND, RESUME, GETREGS, THREAD_INFO = (
    0xBDBB0001, 0xBDBB0002, 0xBDBB0005, 0xBDBB0006, 0xBDBB0007, 0xBDBB0008, 0xBDBB0011)
CODE_BASE = 0x400000  # eboot+offset in crash reports
STACK_BYTES = 32 << 10


def listen():
    """Accepts the debugger's interrupt connection on 755 and drains it until it closes."""
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', 755))
    server.listen(1)
    print('listening on 755', flush=True)
    while True:
        conn, peer = server.accept()
        print('debugger connected from', peer, flush=True)
        while conn.recv(4096):
            pass
        print('debugger connection closed', flush=True)


def run(args):
    elf = Path(args.elf).read_bytes()
    start, at, size = symcrash.sections(elf)['.text']
    out = open(args.output, 'w') if args.output else sys.stdout
    client = ps5debug.Ps5Debug(args.host, timeout=30)
    pid = ps5debug.resolve_pid(client, 'app')
    client._request(ATTACH, struct.pack('<I', pid))
    client._status('attach')
    rows = []
    try:
        client._request(THREADS)
        client._status('thread list')
        count = client._u32()
        lwpids = struct.unpack(f'<{count}I', client._recv(4 * count))
        for lwpid in lwpids:
            client._request(THREAD_INFO, struct.pack('<I', lwpid))
            client._status('thread info')
            info = client._recv(40)
            name = info[8:40].split(b'\0')[0].decode(errors='replace')
            client._request(SUSPEND, struct.pack('<I', lwpid))
            client._status(f'suspend {lwpid}')
            try:
                client._request(GETREGS, struct.pack('<I', lwpid))
                client._status(f'registers of {lwpid}')
                regs = client._recv(176)
                rbp = struct.unpack_from('<Q', regs, 80)[0]
                rip, = struct.unpack_from('<Q', regs, 136)
                rsp, = struct.unpack_from('<Q', regs, 160)
                stack = b''
                for length in (STACK_BYTES, 8192, 2048):
                    try:
                        stack = client.read(pid, rsp, length)
                        break
                    except ps5debug.Ps5DebugError:
                        client.close()
                        client = ps5debug.Ps5Debug(args.host, timeout=30)
            finally:
                client._request(RESUME, struct.pack('<I', lwpid))
                client._status(f'resume {lwpid}')
            calls = []
            for (value,) in struct.iter_unpack('<Q', stack[:len(stack) // 8 * 8]):
                offset = value - CODE_BASE
                if 8 <= offset <= size and symcrash.after_call(elf[at + offset - 8:at + offset]):
                    calls.append(offset)
            rows.append((lwpid, name, rip, rsp, calls))
    finally:
        client._request(DETACH)
        client._status('detach')
        client.close()
    wanted = sorted({o - 1 for *_, calls in rows for o in calls} |
                    {rip - CODE_BASE for _, _, rip, _, _ in rows if 0 <= rip - CODE_BASE < size})
    names = {}
    if wanted:
        answer = subprocess.run(['llvm-symbolizer', '-e', args.elf, '-f', '-C', '--no-inlines',
                                 *[hex(start + a) for a in wanted]], capture_output=True, text=True).stdout
        blocks = [b.strip().split('\n')[0] for b in answer.strip().split('\n\n')]
        names = dict(zip(wanted, blocks))
    for lwpid, name, rip, rsp, calls in rows:
        where = names.get(rip - CODE_BASE, f'{rip:#x} (outside the app code)')
        print(f'== {name} (lwp {lwpid}) rip {where}', file=out)
        seen = []
        for offset in calls:
            if offset not in seen:
                seen.append(offset)
        for offset in seen[:args.depth]:
            print(f'     eboot+{offset:#x}  {names.get(offset - 1, "?")}', file=out)
    if args.output:
        out.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--host', default='192.168.0.190')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('listen')
    dumping = commands.add_parser('dump')
    dumping.add_argument('elf')
    dumping.add_argument('-o', '--output')
    dumping.add_argument('--depth', type=int, default=14)
    args = parser.parse_args()
    if args.command == 'listen':
        listen()
    else:
        run(args)


if __name__ == '__main__':
    main()
