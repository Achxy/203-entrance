"""Evidence checks for one continuous healthy operating window."""
import statistics


_LIGHTING_TRUE = ("compiled", "configured", "active", "bluetooth", "storage_healthy",
                  "worker_running", "worker_stack_sample_valid")
_LIGHTING_FALSE = ("provisioning", "paused", "quarantined", "resource_blocked")
_LIGHTING_METRICS = ("worker_stack_min_free_bytes", "free_heap", "largest_block")
_LIGHTING_TIME_COUNTERS = ("dns_replies", "ntp_replies", "malformed", "foreign_peer",
                           "rate_limited", "clock_refused", "io_errors")
_LIGHTING_TIME_STATES = {"off", "waiting_ap", "socket_budget_refused",
                         "dns_configuration_refused", "bind_failed", "waiting_trusted_clock", "ready"}


def _unsigned(value):
    return value if type(value) is int and 0 <= value <= 0xffffffff else None


def lighting_sample(status):
    """Keep only typed runtime telemetry; never serialize credentials or unknown fields."""
    status = status if type(status) is dict else {}
    result = {key: status[key] if type(status.get(key)) is bool else None
              for key in _LIGHTING_TRUE + _LIGHTING_FALSE}
    result.update({key: _unsigned(status.get(key)) for key in _LIGHTING_METRICS})
    provision = status.get("provision_result")
    result["provision_result"] = provision if type(provision) is int and -0x80000000 <= provision <= 0xffffffff else None
    for key in ("sessions", "completed"):
        if key in status:
            result[key] = _unsigned(status[key])
    clock = status.get("local_time")
    clock = clock if type(clock) is dict else {}
    state = clock.get("state")
    result["local_time"] = {"state": state if type(state) is str and state in _LIGHTING_TIME_STATES else None}
    result["local_time"].update({key: _unsigned(clock.get(key)) for key in _LIGHTING_TIME_COUNTERS})
    return result


def _lighting_healthy(sample):
    for key in _LIGHTING_TRUE:
        if sample[key] is not True:
            raise ValueError(f"Lighting {key} is not true during soak")
    for key in _LIGHTING_FALSE:
        if sample[key] is not False:
            raise ValueError(f"Lighting {key} is not false during soak")
    if sample["provision_result"] != 0:
        raise ValueError("Lighting credentials/provisioning are not valid")
    for key in _LIGHTING_METRICS + ("sessions", "completed"):
        if key in sample and sample[key] is None:
            raise ValueError(f"Invalid lighting {key} telemetry")
    if any(value is None for value in sample["local_time"].values()):
        raise ValueError("Invalid lighting local_time telemetry")


class SoakWindow:
    def __init__(self, seconds, *, replay=False, min_loops=0, lighting=False):
        if seconds <= 0 or min_loops < 0:
            raise ValueError("Soak duration must be positive and minimum loops nonnegative")
        self.required = seconds
        self.replay = replay or min_loops > 0
        self.min_loops = min_loops
        self.lighting = lighting
        self.rows = []
        self.start = self.last = None
        self.failed_sample = None

    def observe(self, sample, now):
        if not self.lighting:
            return self._observe(sample, now)
        sample = dict(sample, lighting=lighting_sample(sample.get("lighting")))
        try:
            _lighting_healthy(sample["lighting"])
            return self._observe(sample, now)
        except (ValueError, KeyError, TypeError):
            self.failed_sample = dict(sample, elapsed_s=round(now - self.start, 3) if self.start is not None else 0)
            raise

    def _observe(self, sample, now):
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
            for key in ("version", "slot", "settings_generation", "clip_sha256", "panel", "animation_slot"):
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
        if self.lighting:
            observed = self.rows + ([self.failed_sample] if self.failed_sample is not None else [])
            report["lighting"] = {"required": True, "observed_samples": len(observed)}
            for source, destination in (("worker_stack_min_free_bytes", "minimum_worker_stack_free_bytes"),
                                         ("free_heap", "minimum_free_heap"),
                                         ("largest_block", "minimum_largest_block")):
                values = [r["lighting"][source] for r in observed if r["lighting"][source] is not None and
                          (source != "worker_stack_min_free_bytes" or r["lighting"]["worker_stack_sample_valid"] is True)]
                report["lighting"][destination] = min(values) if values else None
            if self.failed_sample is not None:
                report["failed_sample"] = self.failed_sample
                if not report["errors"]:
                    report["errors"].append("A lighting soak sample failed validation")
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
                  animation_slot=display.get("animation_slot", 1),
                  settings_generation=status["network"]["settings_generation"],
                  clip_sha256=display["sha256"],
                  panel={k: display[k] for k in ("profile", "rotation", "inverted")},
                  display={k: display[k] for k in ("mode", "ready", "rendered")})
    result["display"].update({k: display["replay"][k] for k in ("frame", "loops")})
    if display["error"] or display["suspended"] or status["ota"]["busy"]:
        raise ValueError("Display error or update interrupted the operating window")
    return result
