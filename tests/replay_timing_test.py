import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from replay_timing import TimingReport, distribution, release_us
from uart_replay import FRAME_BYTES, triplet_crc, verify_result


def frame(report, index, first, second):
    report.accept(f"FRAME {index} {release_us(index) + 1} {first + 1} {second + 1}".encode())


class ReplayTimingTest(unittest.TestCase):
    def test_half_open_window_and_full_latency_after_end(self):
        report = TimingReport(1, 3, True)
        report.accept(b"TIMING 1 3 1 16001 1")
        report.accept(b"CPU 20 30 40 50")
        frame(report, 0, 200, release_us(1) + 20)
        frame(report, 1, release_us(2), release_us(2) - 1)
        frame(report, 2, release_us(2) + 50, release_us(3) + 100)
        report.accept(f"BEAT 2 {release_us(3) + 101}".encode())
        summary = report.summarize(b"RESULT OK 5", [0, 2, 3], (1, 0), "A")

        self.assertEqual(summary["frame_latency"]["count"], 2)
        self.assertEqual(summary["frame_latency"]["p95_us"], release_us(3) - release_us(2) + 100)
        self.assertEqual(summary["worker_latency"][0]["p95_us"], release_us(2) - release_us(1))
        self.assertEqual(summary["deadline_misses"], 2)
        self.assertEqual(summary["peak_backlog"], 2)
        self.assertEqual(summary["end_backlog"], 1)
        self.assertEqual(summary["frames_completed_at_or_after_end"], 1)
        self.assertEqual(summary["beats_reported_at_or_after_end"], 1)
        self.assertAlmostEqual(summary["pipeline_cpu_seconds_per_ecg_second"], 140 * 257 / 2e6)

    def test_overflow_and_underrun_are_distinct(self):
        overflow = TimingReport(0, 64, True)
        overflow.accept(f"TIMING 0 64 1 {release_us(64) + 1} 1".encode())
        overflow.accept(b"CPU 12 10 0 0")
        for index in range(64):
            frame(overflow, index, release_us(index) + 10, -1)
        summary = overflow.summarize(b"RESULT RING_OVERFLOW 64", [], None, "A")
        self.assertEqual(summary["ring_overflows"], 1)
        self.assertEqual(summary["input_underruns"], 0)
        self.assertEqual(summary["deadline_misses"], 64)
        self.assertEqual(summary["peak_backlog"], 64)
        self.assertEqual(summary["end_backlog"], 64)
        self.assertIsNone(summary["frame_latency"])

        underrun = TimingReport(0, 64, True)
        underrun.accept(f"TIMING 0 64 1 {release_us(1) + 10} 0".encode())
        frame(underrun, 0, 50, 60)
        summary = underrun.summarize(b"RESULT INPUT_UNDERRUN 1", [], None, "A")
        self.assertEqual(summary["input_underruns"], 1)
        self.assertEqual(summary["ring_overflows"], 0)
        self.assertEqual(summary["deadline_misses"], 0)
        self.assertIsNone(summary["end_backlog"])
        self.assertNotIn("cpu_us", summary)

    def test_measured_normal_abort_retains_verified_transport_result(self):
        payload = bytes(range(2 * FRAME_BYTES))
        first, second = (triplet_crc(payload, worker, 1) for worker in range(2))
        reply = (f"RESULT INPUT_UNDERRUN 1 {release_us(1)} {release_us(1)} 0 "
                 f"1 0 {first:08x} 1 0 {second:08x}").encode()
        self.assertFalse(verify_result(reply, "normal", payload, 2, measured=True))
        self.assertTrue(verify_result(reply, "underrun", payload, 1, measured=True))
        with self.assertRaises(ValueError):
            verify_result(reply, "normal", payload, 2)

    def test_p95_nearest_rank_and_cpu_capacity(self):
        self.assertEqual(distribution(list(range(1, 21)))["p95_us"], 19)
        report = TimingReport(0, 1, False)
        report.accept(f"TIMING 0 1 0 {release_us(1) + 1} 1".encode())
        report.accept(b"CPU 3000 2000 1000 100")
        with self.assertRaisesRegex(ValueError, "scheduled capacity"):
            report.summarize(b"RESULT OK 1", [], None, "B")


if __name__ == "__main__":
    unittest.main()
