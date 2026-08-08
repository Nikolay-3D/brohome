import pathlib
import sys
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "app"))

from numbers_ru import parse_spoken_number  # noqa: E402


class RussianNumberTests(unittest.TestCase):
    def test_cardinal(self):
        self.assertEqual(parse_spoken_number("шестьдесят два"), 62)

    def test_ordinal(self):
        self.assertEqual(parse_spoken_number("тринадцатый"), 13)

    def test_channel_noise(self):
        self.assertEqual(parse_spoken_number("канал двадцать один"), 21)

    def test_unknown_text(self):
        self.assertIsNone(parse_spoken_number("тнт"))


if __name__ == "__main__":
    unittest.main()
