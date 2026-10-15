"""Offline tests for the SNES Libretro cheat matching helper."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "tools" / "install_libretro_cheats.py"
spec = importlib.util.spec_from_file_location("install_libretro_cheats", SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ImportLibretroTest(unittest.TestCase):
    def test_name_normalization(self):
        self.assertEqual(module.title_key("Super Mario World (USA) (Game Genie)"),
                         module.title_key("Super Mario World"))
        self.assertNotEqual(module.title_key("Super Mario World 2"),
                            module.title_key("Super Mario World"))

    def test_matching_does_not_overwrite_existing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            roms, db, output = [root / n for n in ("roms", "database", "cheats")]
            roms.mkdir()
            db.mkdir()
            (roms / "Super Mario World.sfc").write_bytes(b"rom")
            (db / "Super Mario World (USA).cht").write_text("cheat0_code = \"7E0DBE63\"\n")
            (db / "Super Mario World (Europe).cht").write_text("wrong region\n")
            done, missing = module.install(roms, db, output)
            self.assertEqual((done, missing), (1, 0))
            target = output / "Super Mario World.cht"
            self.assertIn("7E0DBE63", target.read_text())
            target.write_text("custom cheats")
            module.install(roms, db, output)
            self.assertEqual(target.read_text(), "custom cheats")


if __name__ == "__main__":
    unittest.main()
