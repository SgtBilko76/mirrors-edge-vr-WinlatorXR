#!/usr/bin/env python3
"""Plays WinlatorXR's side of XrAPI on a Linux desktop, for testing the WinlatorXR flavour under Wine.

Wine maps Z:\\ to /, so the mod's Z:\\tmp\\xr is this machine's /tmp/xr. This script writes the
`system` file WinlatorXR would, waits for the mod's `version` file, then streams tracking packets
to UDP 127.0.0.1:7872 at the headset rate while printing every mode change the mod replies with on
7278. The head slowly yaws and nods so a frozen view is distinguishable from a live one.

  tools/fake-xrapi.py                     Quest 3-like FOV, 2048x1152 virtual screen
  tools/fake-xrapi.py --screen 1920x1080 --seconds 600
  tools/fake-xrapi.py --press 30:RA       hold A from 30 s to 30.5 s (button names below)

While it runs, writing button names to <dir>/press (e.g. `echo RA > /tmp/xr/press`) holds each
for 0.3 s (or `LY:1.3` for 1.3 s), one after another; `LStickUp` and friends also move that
analog stick fully.

It checks the protocol, not the picture: the reply's mode, stereo layout and FOV, and how many
packets were sent. Whether a frame looks right still needs the headset.
"""
import argparse
import math
import os
import select
import socket
import time

BUTTONS = ['LGrip', 'LMenu', 'LStick', 'LStickLeft', 'LStickRight', 'LStickUp', 'LStickDown',
           'LTrigger', 'LX', 'LY', 'RA', 'RB', 'RGrip', 'RStick', 'RStickLeft', 'RStickRight',
           'RStickUp', 'RStickDown', 'RTrigger']


def quat_yaw_pitch(yaw, pitch):
    # yaw about +Y, then pitch about +X (OpenXR: -Z forward)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    return (cy * sp, sy * cp, -sy * sp, cy * cp)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dir', default='/tmp/xr')
    ap.add_argument('--screen', default='2048x1152')
    ap.add_argument('--fov', default='108.9x103.4', help='per-eye FOV in degrees, as a Quest 3 reports')
    ap.add_argument('--ipd', type=float, default=0.063)
    ap.add_argument('--hz', type=float, default=72.0)
    ap.add_argument('--seconds', type=float, default=300.0)
    ap.add_argument('--still', action='store_true', help='keep the head still')
    ap.add_argument('--press', action='append', default=[], help='SECONDS:BUTTON, held for 0.5 s')
    args = ap.parse_args()

    fov_x, fov_y = (float(v) for v in args.fov.split('x'))
    presses = []
    for p in args.press:
        at, name = p.split(':')
        presses.append((float(at), BUTTONS.index(name)))

    os.makedirs(args.dir, exist_ok=True)
    for name in ('version', 'vr'):
        try:
            os.remove(os.path.join(args.dir, name))
        except FileNotFoundError:
            pass
    with open(os.path.join(args.dir, 'system'), 'w') as f:
        f.write('Meta\nQuest 3 (fake-xrapi)\n14\n2026-09-01\n%s\n' % args.screen)
    print('wrote %s/system; waiting for the mod to write %s/version ...' % (args.dir, args.dir), flush=True)

    reply = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    reply.bind(('127.0.0.1', 7278))
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    start = time.time()
    version = None
    sent = 0
    last_state = None
    sync = 0
    queue = []          # (button index, until) from the press file
    held_until = 0.0
    period = 1.0 / args.hz
    next_at = time.time()
    while time.time() - start < args.seconds:
        if version is None:
            path = os.path.join(args.dir, 'version')
            if os.path.exists(path):
                version = open(path).read().strip()
                print('[%6.1fs] mod requested XrAPI %s; streaming at %.0f Hz' %
                      (time.time() - start, version, args.hz), flush=True)
                t0 = time.time()
        if version is not None:
            t = time.time() - t0
            yaw = 0 if args.still else 0.5 * math.sin(t * 0.4)
            pitch = 0 if args.still else 0.2 * math.sin(t * 0.25)
            hq = quat_yaw_pitch(yaw, pitch)
            buttons = ['F'] * len(BUTTONS)
            for at, index in presses:
                if at <= t < at + 0.5:
                    buttons[index] = 'T'
            control = os.path.join(args.dir, 'press')
            if os.path.exists(control):
                names = open(control).read().split()
                os.remove(control)
                for n in names:
                    name, _, hold = n.partition(':')
                    if name in BUTTONS:
                        queue.append((BUTTONS.index(name), float(hold) if hold else 0.3))
            now = time.time()
            if queue and now >= held_until + 0.3:
                current, hold = queue.pop(0)
                held_until = now + hold
                print('[%6.1fs] press %s' % (now - start, BUTTONS[current]), flush=True)
            sticks = [0.0, 0.0, 0.0, 0.0]
            if now < held_until:
                buttons[current] = 'T'
                axis = {'LStickLeft': (0, -1), 'LStickRight': (0, 1), 'LStickUp': (1, 1), 'LStickDown': (1, -1),
                        'RStickLeft': (2, -1), 'RStickRight': (2, 1), 'RStickUp': (3, 1), 'RStickDown': (3, -1)}
                if BUTTONS[current] in axis:
                    i, v = axis[BUTTONS[current]]
                    sticks[i] = float(v)
            fields = [0, 0, 0, 1, sticks[0], sticks[1], -0.2, 1.4, -0.3,
                      0, 0, 0, 1, sticks[2], sticks[3], 0.2, 1.4, -0.3,
                      hq[0], hq[1], hq[2], hq[3], 0.0, 1.7, 0.0,
                      args.ipd, fov_x, fov_y]
            line = 'fake-xrapi ' + ' '.join('%.5f' % v for v in fields)
            line += ' %d %s 1.70000 0 0 0 1 0 0 0 1 TT' % (sync, ''.join(buttons))
            out.sendto(line.encode(), ('127.0.0.1', 7872))
            sync = (sync + 1) % 256
            sent += 1
        while True:
            wait = max(0.0, next_at - time.time())
            ready, _, _ = select.select([reply], [], [], wait)
            if not ready:
                break
            data = reply.recv(512).decode(errors='replace').split()
            state = tuple(data[2:4]) + tuple(data[4:6])
            if state != last_state:
                last_state = state
                mode_vr = {'0': 'off', '1': 'VR', '2': 'screen', '3': 'screen+head'}.get(data[2], data[2])
                mode_3d = {'0': 'flat', '1': 'side-by-side', '2': 'alternate-eye'}.get(data[3], data[3])
                print('[%6.1fs] mod -> mode %s / %s, fov %s x %s (sent %d packets)' %
                      (time.time() - start, mode_vr, mode_3d, data[4], data[5], sent), flush=True)
        next_at += period
    print('done: %d packets sent' % sent)


if __name__ == '__main__':
    main()
