"""Evidence checks for one continuous healthy operating window."""
import statistics


class SoakWindow:
    def __init__(self, seconds, *, replay=False, min_loops=0):
        if seconds <= 0 or min_loops < 0:
            raise ValueError("Soak duration must be positive and minimum loops nonnegative")
        self.required = seconds
        self.replay = replay or min_loops > 0
        self.min_loops = min_loops
        self.rows = []
        self.start = self.last = None

    def observe(self, sample, now):
        uptime = sample.get("uptime_ms")
        if type(uptime) is not int or uptime < 0:
            raise ValueError("Invalid health uptime")
        if sample.get("trial") is not False:
            raise ValueError("Soak firmware is in an unconfirmed trial")
        if sample.get("services") is not True:
            if not self.rows and uptime < 30000:
                return False  # Startup does not count toward the healthy window.
            raise ValueError("Unhealthy services during soak")
        display = sample.get("display", {})
        if self.replay and (display.get("ready") is not True or display.get("mode") != "replay"):
            raise ValueError("Continuous replay is not running")
        if self.rows:
            first, previous = self.rows[0], self.rows[-1]
            if now <= self.last or now - self.last > 45:
                raise ValueError("Stale or missing soak telemetry")
            if uptime <= previous["uptime_ms"]:
                raise ValueError("Unexpected reset or stale device uptime")
            for key in ("version", "slot", "settings_generation", "clip_sha256", "panel"):
                if sample.get(key) != first.get(key):
                    raise ValueError(f"Unexpected {key} change during soak")
            if self.replay:
                if display["rendered"] <= previous["display"]["rendered"]:
                    raise ValueError("Replay stopped rendering")
                if display["loops"] < previous["display"]["loops"]:
                    raise ValueError("Replay loop counter restarted")
        else:
            self.start = now
        self.last = now
        self.rows.append(dict(sample, elapsed_s=round(now - self.start, 3)))
        return (now - self.start >= self.required and
                uptime - self.rows[0]["uptime_ms"] >= self.required * 1000)

    def report(self, *, complete=False, error=None):
        report = {"required_s": self.required, "duration_s": self.rows[-1]["elapsed_s"] if self.rows else 0,
                  "samples": self.rows, "passed": False, "errors": [error] if error else []}
        if not complete:
            return report
        if (not self.rows or report["duration_s"] < self.required or
                self.rows[-1]["uptime_ms"] - self.rows[0]["uptime_ms"] < self.required * 1000):
            report["errors"].append("A complete continuous operating window was not observed")
            return report
        settled = [r for r in self.rows if r["elapsed_s"] >= 60] or self.rows
        window = max(1, min(20, len(settled) // 3))
        first = statistics.median(r["free_heap"] for r in settled[:window])
        last = statistics.median(r["free_heap"] for r in settled[-window:])
        report["heap"] = {"first_settled_median": first, "last_settled_median": last,
                          "delta": last - first, "minimum_observed": min(r["free_heap"] for r in self.rows),
                          "largest_block_end": self.rows[-1]["largest_block"]}
        if last < first - 2048:
            report["errors"].append("Settled heap decreased by more than 2 KiB; investigate")
        if self.replay:
            loops = self.rows[-1]["display"]["loops"] - self.rows[0]["display"]["loops"]
            report["replay_loops_observed"] = loops
            if loops < self.min_loops:
                report["errors"].append("Insufficient complete replay loop boundaries observed")
        report["passed"] = not report["errors"]
        return report


def http_sample(status):
    if not status["settings_healthy"]:
        raise ValueError("Settings became unhealthy")
    display = status["display"]
    result = {k: status[k] for k in ("version", "slot", "uptime_ms", "free_heap", "minimum_heap", "largest_block", "reset_reason")}
    result.update(services=status["ota"]["healthy"], trial=status["ota"]["trial"],
                  settings_generation=status["network"]["settings_generation"],
                  clip_sha256=display["sha256"],
                  panel={k: display[k] for k in ("profile", "rotation", "inverted")},
                  display={k: display[k] for k in ("mode", "ready", "rendered")})
    result["display"].update({k: display["replay"][k] for k in ("frame", "loops")})
    if display["error"] or display["suspended"] or status["ota"]["busy"]:
        raise ValueError("Display error or update interrupted the operating window")
    return result
