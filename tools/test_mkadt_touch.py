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


def touch_props(board, **over):
    """the /sweet-touch properties mkadt builds for board (None if there is no node); over replaces board.json keys"""
    from tools import boards
    orig = boards.BOARDS[board]
    try:
        if over:
            boards.BOARDS[board] = dict(orig, **over)
            for k, v in over.items():
                if v is None:
                    del boards.BOARDS[board][k]
        root = adt.parse(adt.serialize(mkadt.build(seed=b"\0" * 64, board=board)))[0]
    finally:
        boards.BOARDS[board] = orig
    for c in root.children:
        p = dict((k, v) for k, _f, v in c.props)
        if p["name"] == b"sweet-touch\0":
            return p
    return None


class SecTsBoard(unittest.TestCase):
    """raven's S6SY79x (touch_protocol sec_ts): shim macros, and /sweet-touch with protocol "sec_ts" (xnu patch 0146)."""

    def test_raven_node_selects_sec_ts(self):
        p = touch_props("raven")
        self.assertIsNotNone(p)
        self.assertEqual(p["protocol"], b"sec_ts\0")
        self.assertNotIn("chip-id", p)
        self.assertEqual(p["max-coords"], mkadt.u32(1439, 3119))
        self.assertEqual(p["screen"], mkadt.u32(1440, 3120))
        self.assertEqual(p["spi-fb-delay"], mkadt.u32(0))   # review 1 follow-up: raven's DT feedback delay, not bluejay's 1

    def test_bluejay_node_has_no_protocol(self):
        # patch 0146: no "protocol" property = FTM5; bluejay's node must stay as it was
        p = touch_props("bluejay")
        self.assertNotIn("protocol", p)
        self.assertNotIn("spi-fb-delay", p)
        self.assertEqual(p["chip-id"], mkadt.u32(0x36, 0x48))

    def test_mkadt_refuses_bad_touch_config(self):
        with self.assertRaises(ValueError):
            touch_props("raven", touch_chip_id=["0x36", "0x48"])   # sec_ts with a chip id
        with self.assertRaises(ValueError):
            touch_props("raven", touch_protocol="sec-ts")
        for bad in (4, -1, "0", True, 1.0):
            with self.assertRaises(ValueError, msg=repr(bad)):
                touch_props("raven", touch_spi_fb_delay=bad)
        # ftm5 (the default) without a chip id names no touch IC: no node at all, as on sweet
        self.assertIsNone(touch_props("bluejay", touch_protocol="ftm5", touch_chip_id=None))
        self.assertIsNone(touch_props("sweet"))

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
