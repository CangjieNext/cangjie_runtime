import pathlib
import subprocess
import unittest


CASE_DIR = pathlib.Path(__file__).resolve().parent.parent
NORMALIZE_SIZE = CASE_DIR / "script" / "normalize_byte_size.sh"


class Case27SizeUnitTests(unittest.TestCase):
    def normalize(self, value: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(NORMALIZE_SIZE), value],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_plain_bytes_are_unchanged(self) -> None:
        result = self.normalize("5368709120")

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "5368709120")

    def test_mb_uses_binary_units(self) -> None:
        result = self.normalize("64mb")

        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "67108864")

    def test_gb_uses_binary_units_case_insensitively(self) -> None:
        for value in ("5gb", "5GB"):
            with self.subTest(value=value):
                result = self.normalize(value)

                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout.strip(), "5368709120")

    def test_invalid_unit_is_rejected(self) -> None:
        result = self.normalize("5gib")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected bytes, mb, or gb", result.stderr)


if __name__ == "__main__":
    unittest.main()
