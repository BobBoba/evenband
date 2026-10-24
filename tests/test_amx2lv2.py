import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import amx2lv2

SAMPLE = "\r\n".join([
    "[Breakaway_preset]", "presetname=Unit Test", "num_bands=5",
    "mbagc_ratio=75", "mbagc_range=58",
    *[f"mbagc_threshold[{i}]={v}" for i, v in enumerate((50, 80, 20, 50, 50))],
    *[f"mbagc_atk[{i}]=50" for i in range(5)], *[f"mbagc_rel[{i}]=50" for i in range(5)],
    *[f"outmix[{i}]={v}" for i, v in enumerate((60, 50, 50, 50, 30))], ""])


class Convert(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "t.amx")
        with open(self.path, "w", encoding="latin-1", newline="") as f:
            f.write(SAMPLE)

    def test_rules(self):
        p = amx2lv2.convert(amx2lv2.parse(self.path))
        self.assertEqual(p["mode"], 1.0)
        self.assertAlmostEqual(p["power"], 75.0)
        self.assertAlmostEqual(p["range_boost"], 0.24 * 58)
        self.assertAlmostEqual(p["warmth"], -amx2lv2.CAL["k_threshold"] * 30)   # higher threshold lowers the band
        self.assertAlmostEqual(p["mids"], amx2lv2.CAL["k_threshold"] * 30)
        self.assertAlmostEqual(p["mix0"], 2.0)
        self.assertAlmostEqual(p["mix4"], -4.0)
        self.assertEqual(p["gate"], -90.0)          # no freeze_threshold in the sample
        self.assertAlmostEqual(p["atk0"], amx2lv2.CAL["atk50"])
        self.assertAlmostEqual(p["rel0"], amx2lv2.CAL["rel50"])

    def test_time_scale(self):
        faster = amx2lv2.tau(60.0, 1.0)
        slower = amx2lv2.tau(40.0, 1.0)
        self.assertLess(faster, 1.0)
        self.assertGreater(slower, 1.0)

    def test_fewer_bands_map_to_five(self):
        vals = {"num_bands": "3", "mbagc_threshold[0]": "10", "mbagc_threshold[1]": "50", "mbagc_threshold[2]": "90"}
        got = [amx2lv2.band_value(vals, "mbagc_threshold", 3, j, 50) for j in range(5)]
        self.assertEqual(got, [10, 10, 50, 90, 90])

    def test_bundle_output(self):
        bundle = os.path.join(self.dir, "b.lv2")
        self.assertEqual(amx2lv2.main(["x", self.dir, bundle]), 0)
        self.assertEqual(amx2lv2.main(["x", self.dir, bundle]), 0)     # idempotent
        with open(os.path.join(bundle, "manifest.ttl")) as f:
            self.assertEqual(f.read().count("seeAlso"), 1)
        self.assertTrue(os.path.exists(os.path.join(bundle, "presets", "unit-test.ttl")))


if __name__ == "__main__":
    unittest.main()
