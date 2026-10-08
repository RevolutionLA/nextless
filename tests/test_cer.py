import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools", "benchmark"))

from cer import normalize, tokenize, cer, wer, edit_distance  # noqa: E402


class Normalize(unittest.TestCase):
    def test_punctuation_and_spaces_vanish(self):
        self.assertEqual(normalize("你好，世界。 AB1 2!"), "你好世界ab12")

    def test_nfkc_folds_fullwidth(self):
        self.assertEqual(normalize("ＡＢＣ１２３"), "abc123")

    def test_empty(self):
        self.assertEqual(normalize(""), "")


class Tokens(unittest.TestCase):
    def test_cjk_is_one_token_per_char_latin_groups(self):
        self.assertEqual(tokenize("你好world 12"), ["你", "好", "world", "12"])

    def test_latin_cjk_boundary_splits_without_space(self):
        self.assertEqual(tokenize("你好world好"), ["你", "好", "world", "好"])


class Metrics(unittest.TestCase):
    def test_edit_distance(self):
        self.assertEqual(edit_distance("kitten", "sitting"), 3)
        self.assertEqual(edit_distance("", "abc"), 3)

    def test_cer_ignores_punctuation(self):
        self.assertEqual(cer("你好，世界。", "你好世界"), 0.0)

    def test_cer_value(self):
        self.assertAlmostEqual(cer("你好世界", "你好士界"), 0.25)

    def test_empty_reference_is_unscored_not_zero(self):
        self.assertIsNone(cer("", "随便"))
        self.assertIsNone(wer("", "anything"))

    def test_wer_mixed(self):
        self.assertAlmostEqual(wer("你好 openai", "你好 openai"), 0.0)
        # 3 ref tokens vs 4 hyp tokens: one substitution + one insertion.
        self.assertAlmostEqual(wer("你好 openai", "你好 open api"), 2.0 / 3.0)


if __name__ == "__main__":
    unittest.main()
