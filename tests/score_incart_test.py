import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from score_incart import score_beats


class BeatScoringTest(unittest.TestCase):
    def assert_counts(self, result, tp, fn, fp):
        self.assertEqual(
            (result["true_positives"], result["false_negatives"], result["false_positives"]),
            (tp, fn, fp),
        )

    def test_surplus_duplicate_detection_counts_as_false_positive(self):
        result = score_beats([100, 300], [100, 100], 0, 400)
        self.assert_counts(result, 1, 1, 1)
        self.assertEqual(result["sensitivity"], 0.5)
        self.assertEqual(result["positive_predictive_value"], 0.5)

    def test_matches_earliest_compatible_pair_to_preserve_both_matches(self):
        self.assert_counts(score_beats([100, 147], [110, 64], 0, 200), 2, 0, 0)

    def test_half_open_window_on_both_streams(self):
        self.assert_counts(score_beats([99, 100, 299, 300], [99, 100, 300], 100, 300), 1, 1, 0)
        self.assert_counts(score_beats([99, 100, 300], [99, 100, 299, 300], 100, 300), 1, 0, 1)

    def test_tolerance_on_both_sides(self):
        for delta in (-39, -38, 38, 39):
            with self.subTest(delta=delta):
                expected_tp = int(abs(delta) <= 38)
                self.assert_counts(
                    score_beats([100], [100 + delta], 0, 200),
                    expected_tp, 1 - expected_tp, 1 - expected_tp,
                )

    def test_undefined_ratios(self):
        self.assertIsNone(score_beats([], [100], 0, 200)["sensitivity"])
        self.assertIsNone(score_beats([100], [], 0, 200)["positive_predictive_value"])


if __name__ == "__main__":
    unittest.main()
