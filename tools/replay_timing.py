"""Validate and summarize the ESP32-S3 replay timing records."""

from bisect import bisect_right
from itertools import accumulate, groupby

RATE_HZ = 257
RING_CAPACITY = 64
TASKS = ("input_replay", "worker_1", "worker_2", "fusion")


def release_us(index):
    return index * 1_000_000 // RATE_HZ


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"count": len(ordered), "p95_us": ordered[(95 * len(ordered) + 99) // 100 - 1],
            "max_us": ordered[-1]}


class TimingReport:
    def __init__(self, start, end, detailed):
        self.start = start
        self.end = end
        self.detailed = detailed
        self.header = self.cpu = None
        self.frames = []
        self.beats = []

    def accept(self, reply):
        tag, *values = reply.split()
        fields = tuple(map(int, values))
        if any(value < 0 for value in fields):
            raise ValueError(f"negative timing field: {reply!r}")
        if tag == b"TIMING" and len(fields) == 5 and self.header is None:
            self.header = fields
        elif tag == b"CPU" and len(fields) == len(TASKS) and self.cpu is None:
            self.cpu = fields
        elif tag == b"FRAME" and len(fields) == 4:
            self.frames.append(fields)
        elif tag == b"BEAT" and len(fields) == 2:
            self.beats.append(fields)
        else:
            raise ValueError(f"invalid timing line: {reply!r}")

    def summarize(self, result, fused, fusion_report, configuration):
        if self.header is None:
            raise ValueError("board did not report timing")
        start, end, detailed, stopped, cpu_complete = self.header
        if (start, end, detailed) != (self.start, self.end, self.detailed):
            raise ValueError("board reported a different scored window or timing mode")
        _, status, published, *_ = result.split()
        published = int(published)
        complete = status == b"OK"
        begin_us, end_us = release_us(start), release_us(end)
        duration_us = end_us - begin_us
        stopped_us = stopped - 1
        if cpu_complete != (self.cpu is not None) or cpu_complete != (stopped_us >= end_us):
            raise ValueError("CPU record is inconsistent with reaching the scheduled end")
        summary = {"status": status.decode("ascii"), "configuration": configuration,
                   "window": [start, end], "detailed": bool(detailed), "published": published,
                   "replay_stopped_us": stopped_us, "input_underruns": int(status == b"INPUT_UNDERRUN"),
                   "ring_overflows": int(status == b"RING_OVERFLOW")}
        if self.cpu is not None:
            if configuration == "A":
                per_core = (self.cpu[0] + self.cpu[3], self.cpu[1] + self.cpu[2])
            elif configuration == "B":
                per_core = (self.cpu[0] + self.cpu[2] + self.cpu[3], self.cpu[1])
            else:
                per_core = self.cpu
            if max(per_core) > duration_us or sum(self.cpu) > 2 * duration_us:
                raise ValueError("CPU counters exceed the scheduled capacity")
            cpu_seconds_per_second = {task: value * RATE_HZ / ((end - start) * 1_000_000)
                                      for task, value in zip(TASKS, self.cpu)}
            summary["cpu_us"] = dict(zip(TASKS, self.cpu))
            summary["pipeline_cpu_us"] = sum(self.cpu)
            summary["cpu_seconds_per_ecg_second"] = cpu_seconds_per_second
            summary["pipeline_cpu_seconds_per_ecg_second"] = sum(cpu_seconds_per_second.values())

        if not detailed:
            if self.frames or self.beats:
                raise ValueError("timing-disabled run emitted detailed records")
            return summary

        if [row[0] for row in self.frames] != list(range(min(published, end))):
            raise ValueError("timing report has incomplete or unordered frame coverage")
        events = []
        frame_latency = []
        worker_latency = [[], []]
        previous = [-1, -1, -1]
        deadline_misses = unresolved = carried_completions = 0
        for index, released, first, second in self.frames:
            released, first, second = released - 1, first - 1, second - 1
            scheduled = release_us(index)
            if released < max(scheduled, previous[0]) or released > stopped_us:
                raise ValueError(f"invalid publication timestamp for frame {index}")
            previous[0] = released
            for worker, stamp in enumerate((first, second), 1):
                if stamp < 0:
                    if complete:
                        raise ValueError(f"missing worker {worker} completion for frame {index}")
                    continue
                if stamp < max(released, previous[worker]):
                    raise ValueError(f"invalid worker {worker} completion for frame {index}")
                previous[worker] = stamp
                if index >= start:
                    worker_latency[worker - 1].append(stamp - scheduled)

            finished = max(first, second) if min(first, second) >= 0 else None
            events.append((released, 1))
            if finished is not None:
                events.append((finished, -1))

            if index >= start:
                deadline = release_us(index + 1)
                if finished is not None:
                    frame_latency.append(finished - scheduled)
                    deadline_misses += finished >= deadline
                    carried_completions += finished >= end_us
                elif deadline <= stopped_us:
                    deadline_misses += 1
                else:
                    unresolved += 1

        # Backlog at a timestamp includes all publications and completions at that microsecond.
        stamps = [(stamp, sum(delta for _, delta in same_stamp))
                  for stamp, same_stamp in groupby(sorted(events), key=lambda event: event[0])]
        times = [stamp for stamp, _ in stamps]
        backlogs = [0, *accumulate(delta for _, delta in stamps)]
        if min(backlogs) < 0 or max(backlogs) > RING_CAPACITY:
            raise ValueError("timing records imply an impossible ring backlog")
        left = bisect_right(times, begin_us)
        right = bisect_right(times, min(end_us - 1, stopped_us))
        peak = max(backlogs[left:right + 1], default=0) if stopped_us >= begin_us else None
        end_backlog = None
        if cpu_complete:
            # At end, include completions exactly at the boundary and no new release.
            end_backlog = backlogs[right] - sum(
                stamp == end_us and delta == -1 for stamp, delta in events
            )

        beat_latency = []
        previous_beat = -1
        first_reporting_frame = None if fusion_report is None else sum(fusion_report[:2])
        for location, encoded in self.beats:
            stamp = encoded - 1
            cutoff = location if first_reporting_frame is None else location + first_reporting_frame
            if not start <= location < end or location <= previous_beat or stamp < release_us(cutoff):
                raise ValueError(f"invalid scored beat location or reporting timestamp: {location}")
            previous_beat = location
            beat_latency.append(stamp - release_us(location))
        if complete and [location for location, _ in self.beats] != [
            location for location in fused if start <= location < end
        ]:
            raise ValueError("scored beat locations disagree with the complete fusion report")

        summary.update(frame_latency=distribution(frame_latency),
                       worker_latency=[distribution(values) for values in worker_latency],
                       beat_reporting_latency=distribution(beat_latency),
                       deadline_misses=deadline_misses, deadlines_unresolved=unresolved,
                       peak_backlog=peak, end_backlog=end_backlog,
                       frames_completed_at_or_after_end=carried_completions,
                       beats_reported_at_or_after_end=sum(stamp - 1 >= end_us for _, stamp in self.beats))
        return summary
