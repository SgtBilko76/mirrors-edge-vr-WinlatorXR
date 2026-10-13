#!/usr/bin/env python3
"""Cross-builds the VR shim as x86 on Linux with clang-cl, lld-link and an xwin sysroot.

The Windows build is src/build.ps1 and stays the reference for releases. This script exists
for the WinlatorXR (standalone Quest) flavour, which is developed on Linux beside the headset:

  src/build_linux.py --winlatorxr          d3d9.dll for WinlatorXR (no OpenXR loader needed)
  src/build_linux.py                       the desktop OpenXR flavour, for compile checks only
  src/build_linux.py --winlatorxr --push   also adb push d3d9.dll + mevr.ini to the headset

It generates the same files build.ps1 does (vr_defaults.inl, mevr.ini, build_identity.inl),
from the same inputs, so both flavours embed identical shipped defaults.

Machine-local paths come from the environment, never from this file:
  MEVR_OPENXR_INCLUDE  directory holding openxr/openxr.h (headers only; both flavours need it)
  MEVR_OPENXR_LIB      directory holding the Win32 openxr_loader.lib (desktop flavour only)
  XBUILD_LLVM          LLVM install with bin/clang-cl        (default ~/.local/opt/llvm)
  XBUILD_WINSYSROOT    xwin --winsysroot output              (default ~/.local/opt/winsysroot)
  MEVR_QUEST_GAME_DIR  game Binaries folder on the headset   (for --push)
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MINHOOK = os.path.join(ROOT, 'third_party', 'minhook')
OPT = os.path.expanduser('~/.local/opt')


def die(message):
    sys.stderr.write('build_linux: %s\n' % message)
    sys.exit(1)


def read_text(path):
    with open(path, 'r', encoding='utf-8', newline='') as handle:
        return handle.read()


def write_text(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write(text)


def generate_defaults():
    """vr_defaults.inl and mevr.ini, exactly as build.ps1 builds them."""
    ini = read_text(os.path.join(ROOT, 'mevr.ini.example'))
    edit = re.compile(r'^; Rename this file to  mevr\.ini  and leave it beside d3d9\.dll.*$', re.M)
    if not edit.search(ini):
        die('shipped defaults no longer match mevr.ini.example')
    # As in .NET, `.` also consumes the line's \r, so this one line ends in a bare \n there too.
    ini = edit.sub("; This IS mevr.ini. Keep it beside d3d9.dll in the game's Binaries folder.", ini)
    for notice in ('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'third_party/minhook/LICENSE.txt'):
        ini += '\r\n; ---- %s ----\r\n' % notice
        for line in re.split(r'\r?\n', read_text(os.path.join(ROOT, notice))):
            ini += '; %s\r\n' % line
    header = 'static const char kVrShippedDefaults[] =\r\n'
    for part in range(0, len(ini), 4096):
        header += 'R"MEVRDEFAULT(' + ini[part:part + 4096] + ')MEVRDEFAULT"\r\n'
    header += ';'
    write_text(os.path.join(HERE, 'vr_defaults.inl'), header)
    write_text(os.path.join(HERE, 'mevr.ini'), ini)


def generate_identity():
    """build_identity.inl: a hash over the sources, the same inputs build.ps1 hashes."""
    inputs = []
    for name in os.listdir(HERE):
        path = os.path.join(HERE, name)
        if os.path.isfile(path) and os.path.splitext(name)[1] in ('.cpp', '.inl', '.h', '.def') \
                and name != 'build_identity.inl':
            inputs.append(path)
    inputs.append(os.path.join(HERE, 'build.ps1'))
    for base, _, files in os.walk(MINHOOK):
        inputs += [os.path.join(base, f) for f in files if os.path.splitext(f)[1] in ('.c', '.h')]
    inputs.sort(key=lambda p: p.lower())
    lines = []
    for path in inputs:
        with open(path, 'rb') as handle:
            digest = hashlib.sha256(handle.read()).hexdigest().upper()
        lines.append(path[len(ROOT):].replace('\\', '/') + ':' + digest)
    build_id = hashlib.sha256('\n'.join(lines).encode('utf-8')).hexdigest().upper()
    write_text(os.path.join(HERE, 'build_identity.inl'),
               'static const char kMevrBuildId[] = "%s";' % build_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--winlatorxr', action='store_true', help='build the WinlatorXR (Quest) flavour')
    parser.add_argument('--out', default=None, help='output directory (default build/<flavour>)')
    parser.add_argument('--push', action='store_true', help='adb push the result to MEVR_QUEST_GAME_DIR')
    parser.add_argument('--push-ini', action='store_true', help='with --push, also overwrite mevr.ini')
    parser.add_argument('--compile-only', action='store_true', help='stop before linking')
    args = parser.parse_args()

    llvm = os.environ.get('XBUILD_LLVM', os.path.join(OPT, 'llvm'))
    sysroot = os.environ.get('XBUILD_WINSYSROOT', os.path.join(OPT, 'winsysroot'))
    clang_cl = os.path.join(llvm, 'bin', 'clang-cl')
    # A standalone lld-link first, as the Far Cry cross-build does: some LLVM bundles link
    # theirs against a system ICU that is not installed.
    lld_link = os.environ.get('XBUILD_LLD_LINK', os.path.join(OPT, 'llvm-compat', 'bin', 'lld-link'))
    if not os.path.exists(lld_link):
        lld_link = os.path.join(llvm, 'bin', 'lld-link')
    for tool in (clang_cl, lld_link):
        if not os.path.exists(tool):
            die('%s missing - install clang-cl/lld-link or set XBUILD_LLVM' % tool)
    if not os.path.isdir(sysroot):
        die('xwin sysroot %s missing - set XBUILD_WINSYSROOT' % sysroot)

    xr_include = os.environ.get('MEVR_OPENXR_INCLUDE')
    if not xr_include or not os.path.exists(os.path.join(xr_include, 'openxr', 'openxr.h')):
        die('set MEVR_OPENXR_INCLUDE to a directory containing openxr/openxr.h')
    xr_lib = os.environ.get('MEVR_OPENXR_LIB')
    if not args.winlatorxr and not args.compile_only and (not xr_lib or not os.path.exists(os.path.join(xr_lib, 'openxr_loader.lib'))):
        die('the desktop flavour links openxr_loader.lib: set MEVR_OPENXR_LIB (or build --winlatorxr)')

    flavour = 'winlatorxr' if args.winlatorxr else 'desktop'
    out = os.path.abspath(args.out or os.path.join(ROOT, 'build', flavour))
    obj = os.path.join(out, 'obj')
    os.makedirs(obj, exist_ok=True)

    generate_defaults()
    generate_identity()

    target = ['--target=i686-pc-windows-msvc', '/winsysroot', sysroot]
    cflags = target + ['/nologo', '/c', '/MD', '/O2', '/Z7', '/EHsc', '/W3', '/Brepro',
                       '/I' + os.path.join(MINHOOK, 'include'), '/I' + xr_include,
                       # MSVC accepts these; clang wants them spelled out
                       '-Wno-microsoft-goto', '-Wno-unused-command-line-argument',
                       '-Wno-format-security', '-Werror=format', '-Werror=format-extra-args',
                       '-Werror=format-insufficient-args', '-Werror=unused-function']
    if args.winlatorxr:
        cflags.append('/DMEVR_WINLATORXR=1')

    sources = [(os.path.join(HERE, 'd3d9.cpp'), ['/std:c++17'])]
    for name in ('buffer.c', 'hook.c', 'trampoline.c', os.path.join('hde', 'hde32.c')):
        sources.append((os.path.join(MINHOOK, 'src', name), ['-Wno-everything']))

    objects = []
    for source, extra in sources:
        target_obj = os.path.join(obj, os.path.splitext(os.path.basename(source))[0] + '.obj')
        command = [clang_cl] + cflags + extra + ['/Fo' + target_obj, source]
        print('compile', os.path.relpath(source, ROOT), flush=True)
        if subprocess.call(command) != 0:
            die('compile failed: %s' % source)
        objects.append(target_obj)
    if args.compile_only:
        print('compiled (%s), not linked' % flavour)
        return

    libs = ['user32.lib', 'shell32.lib', 'psapi.lib', 'advapi32.lib', 'kernel32.lib', 'gdi32.lib',
            'ole32.lib', 'uuid.lib']
    libpaths = []
    if args.winlatorxr:
        libs.append('ws2_32.lib')
    else:
        libs += ['openxr_loader.lib', 'd3d11.lib', 'dxgi.lib']
        libpaths.append('/LIBPATH:' + xr_lib)
    dll = os.path.join(out, 'd3d9.dll')
    command = [lld_link, '/nologo', '/DLL', '/MACHINE:X86', '/winsysroot:' + sysroot,
               '/DEF:' + os.path.join(HERE, 'd3d9.def'), '/OUT:' + dll, '/DEBUG:FULL',
               '/PDB:' + os.path.join(out, 'd3d9.pdb'), '/PDBALTPATH:d3d9.pdb', '/Brepro',
               '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF'] + libpaths + objects + libs
    print('link', os.path.relpath(dll, ROOT), flush=True)
    if subprocess.call(command) != 0:
        die('link failed')
    shutil.copyfile(os.path.join(HERE, 'mevr.ini'), os.path.join(out, 'mevr.ini'))
    print('built %s (%s)' % (os.path.relpath(dll, ROOT), flavour))

    if args.push:
        game = os.environ.get('MEVR_QUEST_GAME_DIR')
        if not game:
            die('--push needs MEVR_QUEST_GAME_DIR, e.g. /sdcard/Download/MirrorsEdge/Binaries')
        files = [dll] + ([os.path.join(out, 'mevr.ini')] if args.push_ini else [])
        for path in files:
            if subprocess.call(['adb', 'push', path, game + '/']) != 0:
                die('adb push failed for %s' % path)


if __name__ == '__main__':
    main()
