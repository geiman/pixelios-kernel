import os
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from tools import adt, mkadt  # noqa: E402


class SweetTouchCmu(unittest.TestCase):
    def test_cmu_props_come_from_soc_json(self):
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="bluejay")))[0]
        p = next(dict((k, v) for k, _f, v in c.props) for c in root.children
                 if dict((k, v) for k, _f, v in c.props)["name"] == b"sweet-touch\0")
        self.assertEqual(p["cmu-reg"], mkadt.u64(0x10C00000, 0x10000))
        self.assertEqual(struct.unpack("<I", p["cmu-mux-usi"])[0], 0x640)
        self.assertEqual(struct.unpack("<I", p["cmu-gate-ipclk"])[0], 0x202C)
        self.assertEqual(struct.unpack("<I", p["cmu-gate-val-bit"])[0], 21)


class SecTsBoard(unittest.TestCase):
    """raven's S6SY79x (touch_protocol sec_ts): shim macros yes, /sweet-touch (the FTM5 XNU driver) no."""

    def test_raven_has_no_sweet_touch_node(self):
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board="raven")))[0]
        names = [dict((k, v) for k, _f, v in c.props)["name"] for c in root.children]
        self.assertNotIn(b"sweet-touch\0", names)

    def test_macros(self):
        from tools import boards
        raven = boards._touch_macros(boards.BOARDS["raven"])
        self.assertIn("#define TOUCH_PROTO_SEC_TS 1", raven)
        self.assertFalse([m for m in raven if "CHIP_ID" in m])
        bluejay = boards._touch_macros(boards.BOARDS["bluejay"])
        self.assertNotIn("#define TOUCH_PROTO_SEC_TS 1", bluejay)
        self.assertIn("#define TOUCH_CHIP_ID0 0x36UL", bluejay)

    def test_sec_ts_with_chip_id_is_refused(self):
        from tools import boards
        v = dict(boards.BOARDS["raven"], touch_chip_id=(0x36, 0x48))
        with self.assertRaises(ValueError):
            boards._touch_macros(v)
        with self.assertRaises(ValueError):
            boards._touch_macros(dict(boards.BOARDS["raven"], touch_protocol="ftm6"))

    def test_bad_protocol_fails_at_the_header_entry_point(self):
        # review finding 4: c_header must not silently skip the touch macros for a misspelled / mistyped protocol
        from tools import boards
        orig = boards.BOARDS["raven"]
        try:
            for bad in ("sec-ts", "SEC_TS", 1, None, ["sec_ts"]):
                boards.BOARDS["raven"] = dict(orig, touch_protocol=bad)
                with self.assertRaises(ValueError, msg=repr(bad)):
                    boards.c_header("raven")
            boards.BOARDS["raven"] = dict(orig, touch_protocol="ftm5")   # ftm5 without a chip id
            with self.assertRaises(ValueError):
                boards.c_header("raven")
            boards.BOARDS["raven"] = orig
            self.assertIn("#define TOUCH_PROTO_SEC_TS 1", boards.c_header("raven"))
        finally:
            boards.BOARDS["raven"] = orig


if __name__ == "__main__":
    unittest.main()
