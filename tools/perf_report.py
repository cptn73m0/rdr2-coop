#!/usr/bin/env python3
"""E4 NPC replication performance report.

Parses the game log (rdr2-coop.log) and/or server_console.log looking for
`perf: npc host ...`, `perf: npc guest ...`, legacy `perf: actor ...` lines,
counters and failure/warn/timeout markers. Produces p50/p95/max tables,
counters, optional baseline deltas and PASS/FAIL verdict.

Only the Python standard library is used.

Exit codes:
  0 = PASS (perf data found, no threshold violations)
  1 = FAIL (perf data found, threshold/issue violation)
  2 = no perf data / unreadable input (report not possible)
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

# Acceptance thresholds from E4 (milliseconds unless stated otherwise).
THRESHOLDS = {
    "tick_p95_ms": 2.0,
    "component_p95_ms": 1.5,
    "warn_lines": 0,
    "timeout_rate": 0.005,       # 0.5%
    "appearance_fail_rate": 0.01,  # 1%
    "delete_failed": 0,
}

TOKEN_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_/]*)=(-?\d+(?:\.\d+)?)")
PERF_NPC_RE = re.compile(r"perf:\s*npc\s+(host|guest)\b(.*)$", re.IGNORECASE)
PERF_ACTOR_RE = re.compile(r"perf:\s*actor\b(.*)$", re.IGNORECASE)
PERF_WARN_RE = re.compile(r"perf:\s*npc\s+warn\b", re.IGNORECASE)

# Metrics whose p95 is checked against component_p95_ms, including the guest
# side names (spawn/appearance/motion/weapons) and legacy scan/apply aliases.
COMPONENT_METRICS = {
    "discover", "capture", "apply", "build",
    "spawn", "appearance", "motion", "weapons",
}
LEGACY_ALIASES = {"scan": "discover", "apply": "apply", "spawn": "spawn"}
COUNTER_KEYS = {
    "actors", "spawns", "frames", "despawns", "bytes", "bytes/s",
    "proxies", "appearance_applied", "appearance_failed", "timeouts",
    "appliedFrames", "appliedframes", "despawn", "failed",
}


class Metric:
    """Per-direction metric samples from 10-second perf lines."""

    def __init__(self, name: str):
        self.name = name
        self.avg_samples: list[float] = []
        self.p95_samples: list[float] = []
        self.max_samples: list[float] = []

    def add(self, avg, p95=None, mx=None):
        if avg is not None and is_number(avg):
            self.avg_samples.append(float(avg))
        if p95 is not None and is_number(p95):
            self.p95_samples.append(float(p95))
        if mx is not None and is_number(mx):
            self.max_samples.append(float(mx))
        elif p95 is not None and is_number(p95) and mx is None:
            # Legacy/one-line metrics without a separate max field.
            self.max_samples.append(float(p95))

    def has_data(self) -> bool:
        return bool(self.avg_samples or self.p95_samples or self.max_samples)

    def p50(self):
        # Median of the per-window average values: good central estimate.
        return percentile(self.avg_samples, 50.0)

    def p95(self):
        # Conservative acceptance value: the worst p95 reported by any window.
        if self.p95_samples:
            return max(self.p95_samples)
        return percentile(self.avg_samples, 95.0)

    def max(self):
        if self.max_samples:
            return max(self.max_samples)
        return max(self.avg_samples + self.p95_samples) if (
            self.avg_samples or self.p95_samples) else None

    def as_dict(self):
        return {
            "n": max(len(self.avg_samples), len(self.p95_samples)),
            "p50": round_or_none(self.p50()),
            "p95": round_or_none(self.p95()),
            "max": round_or_none(self.max()),
        }


def is_number(value) -> bool:
    try:
        number = float(value)
        return math.isfinite(number)
    except (TypeError, ValueError):
        return False


def round_or_none(value, digits=2):
    if value is None:
        return None
    return round(float(value), digits)


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * (pct / 100.0)
    low = int(MathFloor(rank))
    high = min(low + 1, len(ordered) - 1)
    frac = rank - low
    return ordered[low] * (1.0 - frac) + ordered[high] * frac


def MathFloor(value):
    # tiny helper kept Python-version-independent.
    return int(value // 1) if value >= 0 else -int((-value) // 1) - 1


class PerfReport:
    def __init__(self, log_path: Path, label=None):
        self.path = log_path
        self.label = label or log_path.name
        self.directions = {"host": {}, "guest": {}, "legacy": {}}
        self.counters = {"host": {}, "guest": {}, "legacy": {}}
        self.warn_lines = 0
        self.warn_samples = []
        self.timeout_values = []
        self.applied_frames = 0
        self.guest_spawns = 0
        self.appearance_failed_counters = 0
        self.appearance_applied_counters = 0
        self.appearance_failed_logs = 0
        self.appearance_success_logs = 0
        self.delete_failed = 0
        self.mount_failed = 0
        self.scenario_failed = 0
        self.generic_errors = 0
        self.issue_samples = []
        self.lines = 0
        self.perf_lines = 0

    # -- parsing ---------------------------------------------------------
    def parse(self):
        try:
            with self.path.open("r", encoding="utf-8-sig",
                                errors="replace") as fh:
                for raw in fh:
                    self.lines += 1
                    line = raw.rstrip("\r\n")
                    self._parse_line(line)
        except OSError as exc:
            raise ReportError(f"cannot read log {self.path}: {exc}") from exc
        return self

    def _metric(self, direction: str, name: str) -> Metric:
        metrics = self.directions[direction]
        if name not in metrics:
            metrics[name] = Metric(name)
        return metrics[name]

    def _parse_line(self, line: str):
        if "perf:" in line:
            self.perf_lines += 1
            if PERF_WARN_RE.search(line):
                self.warn_lines += 1
                if len(self.warn_samples) < 10:
                    self.warn_samples.append(line.strip())
            match = PERF_NPC_RE.search(line)
            if match:
                self._parse_perf_npc(match.group(1).lower(), match.group(2))
            else:
                match = PERF_ACTOR_RE.search(line)
                if match:
                    self._parse_perf_actor(match.group(1))
        self._parse_issues(line)

    def _parse_perf_npc(self, direction: str, body: str):
        sections = [part.strip() for part in body.split("|")]
        for section_index, section in enumerate(sections):
            tokens = tokens_of(section)
            if not tokens:
                continue
            if section_index == 0 and "tick" in tokens:
                # tick / p95 / max: a compact timing bucket.
                self._metric(direction, "tick").add(
                    tokens.get("tick"), tokens.get("p95"), tokens.get("max"))
                continue
            for name, value in tokens.items():
                if name in ("p95", "max", "tick", "ceil", "budget"):
                    continue
                metric_name = LEGACY_ALIASES.get(name, name)
                if metric_name in COMPONENT_METRICS or metric_name in (
                        "scan", "apply", "spawn", "appearance", "motion",
                        "weapons", "discover", "capture", "build"):
                    self._metric(direction, metric_name).add(
                        value, tokens.get("p95"), tokens.get("max"))
            # Counter-only line: every token is a counter or a metric marker.
            counters = {key: value for key, value in tokens.items()
                        if key not in ("p95", "max", "tick", "ceil", "budget")}
            self._add_counters(direction, counters)

    def _parse_perf_actor(self, body: str):
        sections = [part.strip() for part in body.split("|")]
        for section in sections:
            tokens = tokens_of(section)
            if not tokens:
                continue
            # First token with a name is the metric name (scan/spawn/apply).
            metric_name = None
            for key in tokens:
                if key in ("p95", "max", "tick"):
                    continue
                metric_name = key
                break
            if metric_name is None:
                continue
            alias = LEGACY_ALIASES.get(metric_name, metric_name)
            self._metric("legacy", alias).add(
                tokens.get(metric_name), tokens.get("p95"), tokens.get("max"))
            counters = {key: value for key, value in tokens.items()
                        if key not in (metric_name, "p95", "max", "tick")}
            self._add_counters("legacy", counters)

    def _add_counters(self, direction: str, tokens: dict):
        bucket = self.counters[direction]
        for key, value in tokens.items():
            if key in ("p95", "max", "tick", "ceil", "budget"):
                continue
            # Do not mistake timing-metric names for counters.
            if key in COMPONENT_METRICS or key in LEGACY_ALIASES or \
                    key in ("scan", "apply", "spawn", "tick"):
                continue
            if not is_number(value):
                continue
            numerical = float(value)
            if key in ("bytes/s", "bytes"):
                key = "bytes/s"
            if direction == "guest" and key == "applied":
                key = "appearance_applied"
            if direction == "guest" and key == "failed":
                key = "appearance_failed"
            item = bucket.setdefault(key, {"samples": [], "sum": 0.0,
                                           "max": 0.0, "last": 0.0})
            item["samples"].append(numerical)
            item["sum"] += numerical
            item["max"] = max(item["max"], numerical)
            item["last"] = numerical
            if key == "timeouts":
                self.timeout_values.append(numerical)
            if key in ("frames", "appliedFrames", "appliedframes"):
                self.applied_frames += max(0, int(round(numerical)))
            if direction == "guest" and key == "spawns":
                self.guest_spawns += max(0, int(round(numerical)))
            if key == "appearance_failed":
                self.appearance_failed_counters += max(0, int(round(numerical)))
            if key == "appearance_applied":
                self.appearance_applied_counters += max(
                    0, int(round(numerical)))

    def _parse_issues(self, line: str):
        lower = line.lower()
        if "delete failed" in lower:
            self.delete_failed += 1
            self._issue(line)
        if "mount failed" in lower:
            self.mount_failed += 1
            self._issue(line)
        if "scenario failed" in lower or "scenario apply" in lower and "state=failed" in lower:
            self.scenario_failed += 1
            self._issue(line)
        if ("appearance" in lower or "asset capture" in lower) and \
                "state=failed" in lower:
            self.appearance_failed_logs += 1
            self._issue(line)
        elif ("appearance" in lower or "asset capture" in lower) and \
                ("applied" in lower or "state=applied" in lower or
                 "valid=" in lower):
            self.appearance_success_logs += 1
        if "timeouts=" in lower:
            # Already collected via token parsing; keep a sample for context.
            if "actor:" in lower or "perf: npc" in lower:
                self._issue_sample(line)
            if "perf:" not in lower:
                # Legacy `actor: proxies=... appliedFrames=... timeouts=...`.
                self._add_counters("legacy", tokens_of(line))
        if "error" in lower and "perf:" not in lower:
            self.generic_errors += 1

    def _issue(self, line: str):
        if len(self.issue_samples) < 20:
            self.issue_samples.append(line.strip())

    def _issue_sample(self, line: str):
        if len(self.issue_samples) < 20:
            self.issue_samples.append(line.strip())

    # -- summaries -------------------------------------------------------
    def timeout_rate(self):
        total_timeouts = sum(max(0.0, value) for value in self.timeout_values)
        if total_timeouts == 0:
            return 0.0
        # Guest counters do not include frame counts in the v16 E4 line; use
        # guest spawn attempts there. Legacy actor lines expose appliedFrames.
        if self.guest_spawns > 0:
            denominator = float(self.guest_spawns) + total_timeouts
        else:
            denominator = max(1.0, float(self.applied_frames) + total_timeouts)
        return total_timeouts / denominator

    def appearance_fail_rate(self):
        failed = self.appearance_failed_counters + self.appearance_failed_logs
        applied = self.appearance_applied_counters + self.appearance_success_logs
        if failed == 0:
            return 0.0
        return failed / max(1.0, failed + applied)

    def report_data(self):
        directions = {}
        for direction, metrics in self.directions.items():
            if not metrics:
                continue
            directions[direction] = {
                "metrics": {name: metric.as_dict()
                            for name, metric in sorted(metrics.items())},
                "counters": counter_summary(self.counters.get(direction, {})),
            }
        return {
            "log": str(self.path),
            "label": self.label,
            "lines": self.lines,
            "perf_lines": self.perf_lines,
            "directions": directions,
            "issues": {
                "warn_lines": self.warn_lines,
                "warn_samples": self.warn_samples,
                "timeouts": round_or_none(sum(self.timeout_values), 0),
                "timeout_rate": round_or_none(self.timeout_rate(), 5),
                "appearance_failed": self.appearance_failed_counters +
                                     self.appearance_failed_logs,
                "appearance_fail_rate": round_or_none(
                    self.appearance_fail_rate(), 5),
                "delete_failed": self.delete_failed,
                "mount_failed": self.mount_failed,
                "scenario_failed": self.scenario_failed,
                "generic_errors": self.generic_errors,
                "samples": self.issue_samples,
            },
        }

    def has_data(self):
        return any(self.directions[d] for d in self.directions) or \
            any(self.counters[d] for d in self.counters)


def tokens_of(text: str) -> dict:
    result = {}
    for match in TOKEN_RE.finditer(text):
        result[match.group(1)] = match.group(2)
    return result


def counter_summary(bucket: dict) -> dict:
    summary = {}
    for key, item in sorted(bucket.items()):
        values = item["samples"]
        summary[key] = {
            "n": len(values),
            "last": round_or_none(item["last"], 2),
            "max": round_or_none(item["max"], 2),
            "avg": round_or_none(sum(values) / len(values), 2) if values else None,
            "sum": round_or_none(item["sum"], 2),
        }
    return summary


def evaluate(report: PerfReport, baseline: PerfReport = None):
    """Return (passed, checks, recommendations)."""
    data = report.report_data()
    checks = []
    recommendations = []

    def add_check(name, ok, limit, actual, detail=None):
        checks.append({
            "name": name, "ok": bool(ok), "limit": limit,
            "actual": round_or_none(actual), "detail": detail,
        })
        if not ok:
            recommendations.append(
                f"{name}: actual={fmt(actual)} limit={limit}")

    # Tick p95: absolute <=2 ms; with baseline, the added tick p95 <=2 ms
    # is evaluated as current p95 - baseline p95.
    saw_tick_direction = False
    for direction in ("host", "guest"):
        metrics = report.directions.get(direction, {})
        metric = metrics.get("tick")
        if metric is None or not metric.has_data():
            if metrics:
                add_check(f"{direction}_tick_p95_added_ms", False,
                          THRESHOLDS["tick_p95_ms"], None,
                          "missing tick metric for this direction")
            continue
        saw_tick_direction = True
        current = metric.p95()
        base = (baseline.directions.get(direction, {}).get("tick")
                if baseline is not None else None)
        if baseline is not None and base is not None and base.has_data():
            actual = current - base.p95()
            detail = (f"added vs baseline: {fmt(base.p95())} -> "
                      f"{fmt(current)} ms")
        else:
            actual = current
            detail = "absolute (no baseline tick)"
        add_check(f"{direction}_tick_p95_added_ms", actual <=
                  THRESHOLDS["tick_p95_ms"],
                  THRESHOLDS["tick_p95_ms"], actual, detail)
    if not saw_tick_direction:
        add_check("tick_metrics_present", False, "host/guest tick",
                  None, "legacy perf: actor only; new perf: npc tick needed")
        recommendations.append(
            "No host/guest `perf: npc ... tick=` metrics: enable npc_debug=1 "
            "with the E4-instrumented build; legacy `perf: actor` only "
            "reports scan/spawn/apply.")

    # Component timings: p95 <=1.5 ms.
    for direction in ("host", "guest", "legacy"):
        metrics = report.directions.get(direction, {})
        for name, metric in sorted(metrics.items()):
            if name == "tick" or name not in COMPONENT_METRICS:
                continue
            if not metric.has_data():
                continue
            add_check(f"{direction}_{name}_p95_ms",
                      metric.p95() <= THRESHOLDS["component_p95_ms"],
                      THRESHOLDS["component_p95_ms"], metric.p95())

    add_check("perf_npc_warn_lines", report.warn_lines <=
              THRESHOLDS["warn_lines"], THRESHOLDS["warn_lines"],
              report.warn_lines)
    timeout_rate = report.timeout_rate()
    add_check("timeout_rate", timeout_rate <= THRESHOLDS["timeout_rate"],
              THRESHOLDS["timeout_rate"], timeout_rate)
    appearance_rate = report.appearance_fail_rate()
    add_check("appearance_fail_rate",
              appearance_rate <= THRESHOLDS["appearance_fail_rate"],
              THRESHOLDS["appearance_fail_rate"], appearance_rate)
    add_check("delete_failed", report.delete_failed <=
              THRESHOLDS["delete_failed"], THRESHOLDS["delete_failed"],
              report.delete_failed)

    if report.scenario_failed:
        recommendations.append(
            f"scenario failed lines={report.scenario_failed} (informational; "
            "E3a scenario failures are counted separately)")
    if report.mount_failed:
        recommendations.append(
            f"mount failed lines={report.mount_failed} (check E2b)")
    if report.generic_errors:
        recommendations.append(
            f"generic ERROR lines={report.generic_errors} (inspect log)")

    passed = all(check["ok"] for check in checks)
    if not checks:
        passed = False
        recommendations.append("no perf metrics found: enable npc_debug=1 and "
                               "rerun with npc_actor=1")
    # Data sanity: generated by caller.
    data["checks"] = checks
    data["thresholds"] = dict(THRESHOLDS)
    data["passed"] = passed
    data["recommendations"] = recommendations
    return passed, data


def fmt(value, digits=2):
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}"


def print_human(data, baseline_data=None):
    print(f"E4 NPC perf report: {data['label']} ({data['log']})")
    print(f"  lines={data['lines']} perf_lines={data['perf_lines']}")
    if baseline_data:
        print(f"  baseline: {baseline_data['label']} ({baseline_data['log']})")
    for direction in ("host", "guest", "legacy"):
        section = data["directions"].get(direction)
        if not section:
            continue
        print(f"\n[{direction.upper()}]")
        metrics = section["metrics"]
        if metrics:
            print(f"  {'metric':<12} {'n':>4} {'p50':>8} {'p95':>8} {'max':>8}")
            for name, values in metrics.items():
                print(f"  {name:<12} {values['n']:>4} "
                      f"{fmt(values['p50']):>8} {fmt(values['p95']):>8} "
                      f"{fmt(values['max']):>8}")
            if baseline_data:
                base_dir = baseline_data["directions"].get(direction, {})
                base_metrics = base_dir.get("metrics", {})
                if base_metrics:
                    print("  deltas vs baseline (p95):")
                    for name in metrics:
                        if name in base_metrics:
                            delta = (metrics[name]["p95"] or 0.0) - (
                                base_metrics[name]["p95"] or 0.0)
                            print(f"    {name:<12} {delta:+.2f} ms")
        counters = section.get("counters", {})
        if counters:
            rendered = []
            for key, value in counters.items():
                if key in ("sum", "max", "last", "avg"):
                    continue
                rendered.append(f"{key}={value.get('max', value.get('last'))}")
                if len(rendered) >= 10:
                    break
            if rendered:
                print("  counters: " + " ".join(rendered))

    issues = data["issues"]
    print("\n[ISSUES]")
    print(f"  warn_lines={issues['warn_lines']} "
          f"timeouts={issues['timeouts']} "
          f"timeout_rate={fmt(issues['timeout_rate'], 5)} "
          f"appearance_failed={issues['appearance_failed']} "
          f"appearance_fail_rate={fmt(issues['appearance_fail_rate'], 5)}")
    print(f"  delete_failed={issues['delete_failed']} "
          f"mount_failed={issues['mount_failed']} "
          f"scenario_failed={issues['scenario_failed']} "
          f"generic_errors={issues['generic_errors']}")
    if issues["samples"]:
        print("  samples:")
        for sample in issues["samples"][:5]:
            print(f"    {sample[:160]}")

    print("\n[CHECKS]")
    for check in data["checks"]:
        status = "PASS" if check["ok"] else "FAIL"
        print(f"  {status:<4} {check['name']:<28} "
              f"actual={fmt(check['actual'])} limit={check['limit']}")
    print("\nVERDICT: " + ("PASS" if data["passed"] else "FAIL"))
    if data["recommendations"]:
        print("RECOMMENDATIONS:")
        for rec in data["recommendations"]:
            print(f"  - {rec}")


class ReportError(Exception):
    pass


def build_parser():
    ap = argparse.ArgumentParser(
        description="E4 NPC replication performance report (p50/p95/max, "
                    "counters, baseline deltas, PASS/FAIL thresholds).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--log", required=True,
                    help="game log (rdr2-coop.log) or server_console.log")
    ap.add_argument("--baseline", default=None,
                    help="optional baseline log (e.g. npc_actor=0 run)")
    ap.add_argument("--json", action="store_true",
                    help="print machine-readable JSON only")
    ap.add_argument("--label", default=None,
                    help="label shown in the report (default: file name)")
    return ap


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass
    args = build_parser().parse_args(argv)
    log_path = Path(args.log)
    if not log_path.is_file():
        print(f"perf_report: log not found: {log_path}", file=sys.stderr)
        return 2
    try:
        report = PerfReport(log_path, args.label).parse()
    except ReportError as exc:
        print(f"perf_report: {exc}", file=sys.stderr)
        return 2
    baseline = None
    if args.baseline:
        baseline_path = Path(args.baseline)
        if not baseline_path.is_file():
            print(f"perf_report: baseline not found: {baseline_path}",
                  file=sys.stderr)
            return 2
        try:
            baseline = PerfReport(baseline_path, "baseline").parse()
        except ReportError as exc:
            print(f"perf_report: {exc}", file=sys.stderr)
            return 2
        if not baseline.has_data():
            print("perf_report: baseline has no perf data", file=sys.stderr)
            baseline = None

    if not report.has_data():
        print(f"perf_report: no perf: npc/actor lines in {log_path}",
              file=sys.stderr)
        return 2

    passed, data = evaluate(report, baseline)
    if args.baseline and baseline is not None:
        data["baseline"] = baseline.report_data()
    if args.json:
        print(json.dumps(data, indent=2, ensure_ascii=False))
    else:
        baseline_data = data.get("baseline") if "baseline" in data else None
        print_human(data, baseline_data)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
