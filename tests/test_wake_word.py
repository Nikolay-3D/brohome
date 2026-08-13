import pathlib
import sys
import types
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

# The wake-word helpers do not use these optional runtime packages. Minimal
# stubs let this unit test run without installing the full audio server stack.
speech_recognition = types.ModuleType("speech_recognition")
speech_recognition.Recognizer = object
speech_recognition.AudioFile = object
sys.modules.setdefault("speech_recognition", speech_recognition)
vosk = types.ModuleType("vosk")
vosk.KaldiRecognizer = object
vosk.Model = object
sys.modules.setdefault("vosk", vosk)

from bro_home_wifi_v3 import adc_raw_to_pcm, command_after_wake_word, has_wake_word  # noqa: E402


class WakeWordTests(unittest.TestCase):
    def test_exact_wake_words(self):
        for text in ("бро", "брок", "броу", "эй бро время"):
            with self.subTest(text=text):
                self.assertTrue(has_wake_word(text))

    def test_tv_words_do_not_wake(self):
        for text in (
            "программа передач",
            "транспортного комплекса",
            "президент говорил",
            "это было вчера",
            "говорят профессии",
            "старший брат",
        ):
            with self.subTest(text=text):
                self.assertFalse(has_wake_word(text))

    def test_wake_word_must_not_be_a_substring(self):
        self.assertFalse(has_wake_word("брови"))
        self.assertFalse(has_wake_word("брокколи"))

    def test_extract_command(self):
        self.assertEqual(command_after_wake_word("эй, бро, который час"), "который час")
        self.assertEqual(command_after_wake_word("программа про погоду"), "")

    def test_loud_microphone_signal_is_limited(self):
        raw = b"\x00\x40\x00\xc0"
        pcm = adc_raw_to_pcm(raw)
        samples = [
            int.from_bytes(pcm[index:index + 2], "little", signed=True)
            for index in range(0, len(pcm), 2)
        ]
        self.assertEqual(samples, [-30000, 30000])

    def test_quiet_microphone_signal_gets_full_gain(self):
        raw = (31768).to_bytes(2, "little") + (33768).to_bytes(2, "little")
        pcm = adc_raw_to_pcm(raw)
        samples = [
            int.from_bytes(pcm[index:index + 2], "little", signed=True)
            for index in range(0, len(pcm), 2)
        ]
        self.assertEqual(samples, [-3000, 3000])


if __name__ == "__main__":
    unittest.main()
