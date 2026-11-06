"""--board raven / --touch handling of tools/build-xnu-image.sh (no build, no phone: the XNU_IMAGE_ARGS_ONLY hook)."""
import json, os, subprocess, sys, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkpayload

SH = os.path.join(HERE, 'build-xnu-image.sh')
BASE = ('debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin sweetusb=0 '
        'rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 ufsrw=1 rootrw=1 '
        'sweetfb_noconsole=1 sweetfb_hz=30')
# raven-xnu11-touch's boot-args (device/google/raven/hardware/touch-xnu-2026-11-06.txt: lock screen + touch, observed)
RAVEN_TOUCH = BASE + (' sweettouchmux=1 sweettouchclk=3 sweettouchcs=1 '
                      'sweettouchpoke=m:10c00640=10=0;d:100;m:10c0180c=f=5;')


def run(*a):
    env = dict(os.environ, XNU_IMAGE_ARGS_ONLY='1')
    return subprocess.run(['bash', SH, *a], capture_output=True, text=True, env=env)


def parsed(r):
    out = dict(line.split(': ', 1) for line in r.stdout.strip().split('\n'))
    return out['shim'].split(), out['boot-args']


class RavenImage(unittest.TestCase):
    def test_touch_reproduces_the_observed_boot_args_and_rails(self):
        r = run('--board', 'raven', '--kernel', '/k', '--cpus', '8', '--rw', '--gui', '--touch')
        self.assertEqual(r.returncode, 0, r.stderr)
        shim, args = parsed(r)
        self.assertEqual(args, RAVEN_TOUCH)
        self.assertEqual(shim, ['--board', 'raven', '--touch-probe=rails'])
        mkpayload.check_cmdline(args)                      # within the 607-byte boot-args budget

    def test_touch_args_come_from_board_json(self):
        with open(os.path.join(HERE, '..', 'device', 'google', 'raven', 'board.json')) as f:
            want = json.load(f)['touch_xnu_bootargs']
        _shim, args = parsed(run('--board', 'raven', '--kernel', '/k', '--touch'))
        self.assertTrue(args.endswith(' ' + want), args)
        self.assertEqual(args.count('sweettouch'), 4)

    def test_raven_without_touch_has_no_touch_driver_args(self):
        r = run('--board', 'raven', '--kernel', '/k', '--cpus', '8', '--rw', '--gui')
        self.assertEqual(r.returncode, 0, r.stderr)
        shim, args = parsed(r)
        self.assertEqual(args, BASE)
        self.assertEqual(shim, ['--board', 'raven'])

    def test_raven_needs_a_kernel(self):
        r = run('--board', 'raven')
        self.assertEqual(r.returncode, 2); self.assertIn('--kernel', r.stderr); self.assertNotIn('boot-args', r.stdout)

    def test_raven_refuses_options_never_tried_there(self):
        for extra in (['--usbnet'], ['--usbdma'], ['--dvfs', 'read'], ['--dvfs', '1000,0,0'], ['--gpu-handover'],
                      ['--wlan-hold'], ['--wlan-hold', '--wlanfw', '/f']):
            r = run('--board', 'raven', '--kernel', '/k', *extra)
            self.assertEqual(r.returncode, 2, extra); self.assertIn('never tried on raven', r.stderr, extra)
            self.assertNotIn('boot-args', r.stdout, extra)

    def test_touch_is_raven_only(self):
        r = run('--touch')
        self.assertEqual(r.returncode, 2); self.assertIn('raven only', r.stderr); self.assertNotIn('boot-args', r.stdout)

    def test_unknown_board(self):
        r = run('--board', 'oriole', '--kernel', '/k')
        self.assertEqual(r.returncode, 2); self.assertIn('--board', r.stderr)

    def test_bluejay_default_is_unchanged(self):
        r = run()
        self.assertEqual(r.returncode, 0, r.stderr)
        shim, args = parsed(r)
        self.assertEqual(shim, ['--board', 'bluejay'])
        self.assertEqual(args, BASE.replace('cpus=8', 'cpus=1').replace(' ufsrw=1 rootrw=1 sweetfb_noconsole=1 sweetfb_hz=30', ''))
        self.assertNotIn('sweettouch', args)


if __name__ == '__main__':
    unittest.main()
