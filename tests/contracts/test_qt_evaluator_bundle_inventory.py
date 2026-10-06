"""Guarded build inventory for the closed evaluator bundle."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "apps/tools/qt_evaluator_bundle_inventory.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("qt_evaluator_bundle_inventory", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class QtEvaluatorBundleInventoryContract(unittest.TestCase):
    def test_actual_ldd_shape_is_parsed_without_addresses_or_vdso(self):
        text = """linux-vdso.so.1 (0x00007fff)
libtrade_ngin.so => /owned/libtrade_ngin.so (0x00007f00)
libcrypto.so.3 => /usr/lib/libcrypto.so.3 (0x00007f01)
libc.so.6 => /usr/lib/libc.so.6 (0x00007f02)
/lib64/ld-linux-x86-64.so.2 (0x00007f03)
"""
        self.assertEqual(load_tool().parse_ldd(text), {
            "libtrade_ngin.so": Path("/owned/libtrade_ngin.so"),
            "libcrypto.so.3": Path("/usr/lib/libcrypto.so.3"),
            "libc.so.6": Path("/usr/lib/libc.so.6"),
            "ld-linux-x86-64.so.2": Path("/lib64/ld-linux-x86-64.so.2"),
        })

    def test_unresolved_duplicate_and_relative_dependencies_fail_closed(self):
        cases = [
            "libcrypto.so.3 => not found\n",
            "libcrypto.so.3 => /a/libcrypto.so.3 (0x01)\nlibcrypto.so.3 => /b/libcrypto.so.3 (0x02)\n",
            "libcrypto.so.3 => relative/libcrypto.so.3 (0x01)\n",
        ]
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                load_tool().parse_ldd(text)

    def test_inventory_binds_the_selected_evaluator_and_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            evaluator = root / "qt_evaluator"
            engine = root / "libtrade_ngin.so"
            evaluator.write_bytes(b"evaluator")
            engine.write_bytes(b"engine")
            crypto = root / "libcrypto.so.3"
            loader = root / "ld-linux-x86-64.so.2"
            crypto.write_bytes(b"crypto")
            loader.write_bytes(b"loader")
            dependencies = {
                "libtrade_ngin.so": engine,
                "libcrypto.so.3": crypto,
                "ld-linux-x86-64.so.2": loader,
            }
            value = load_tool().inventory(
                evaluator, engine, "a982e42", "GNU", "14.2.0", dependencies)
            self.assertEqual(value["metadata"]["evaluator_build"], "a982e42")
            rows = {row["name"]: row for row in value["artifacts"]}
            self.assertEqual(rows["qt_evaluator"]["source"], str(evaluator.resolve()))
            self.assertEqual(rows["qt_evaluator"]["role"], "executable")
            self.assertEqual(rows["libtrade_ngin.so"]["source"], str(engine.resolve()))
            self.assertEqual(rows["libtrade_ngin.so"]["role"], "engine")
            self.assertEqual(rows["ld-linux-x86-64.so.2"]["role"], "loader")


if __name__ == "__main__":
    unittest.main()
