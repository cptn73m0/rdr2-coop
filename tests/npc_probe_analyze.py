#!/usr/bin/env python3
"""E-1 probe log analyzer for RDR2 CO-OP NPC replication research.

Parses probe lines emitted by the client in the E-1 read-only probe mode
(``npc_debug = 1``, ``npc_actor = 0``):

    probe: model=0x... metaOutfit=0x... equipped=N mount=0x... weapon=0x...
           scenarioAny=N dynScenario=0x... pointType=0x... pedType=N human=N mission=N

The analyzer streams the client log (rdr2-coop.log), keeps only ``probe:``
lines, computes distributions and gives a PASS/PARTIAL/FAIL/... verdict for
the E-1 hypotheses that decide the E1 implementation path (see
docs/NPC_PROBE.md):

  H1  meta outfit hash is readable for human NPCs (metaOutfit != 0)
  H2  ``equipped`` (result of _IS_META_PED_OUTFIT_EQUIPPED) agrees with hash
  H3  dynamic scenario / scenario point detection covers actual scenarios
  H4  human vs animal pool ratio is observable
  H5  mount / weapon information is present (informational)
  H6  human flag agrees with the model name from rdr2-hashdb Peds.json

Stdlib only (Python 3.8+). Expected to be run manually on a copy of a probe
log; it is not part of the headless server CI suite.

Usage:
    python tests/npc_probe_analyze.py "E:\\Games\\Red Dead Redemption 2\\scripts\\rdr2-coop.log"
    python tests/npc_probe_analyze.py rdr2-coop.log --top 12
    python tests/npc_probe_analyze.py rdr2-coop.log --json > probe_report.json
    python tests/npc_probe_analyze.py rdr2-coop.log --strict  # E1 gate exit code

Exit codes:
    0  analysis completed
    1  --strict and a gating hypothesis (H1/H2/H3/H6) FAILed
    2  no probe data / input could not be read
"""

import argparse
import collections
import json
import sys
from pathlib import Path

PROBE_MARKER = "probe:"

# Canonical field order of the E-1 probe contract (docs/NPC_PROBE.md).
CANONICAL_FIELDS = (
    "model",
    "metaOutfit",
    "equipped",
    "mount",
    "weapon",
    "scenarioAny",
    "dynScenario",
    "pointType",
    "pedType",
    "human",
    "mission",
)

# Fields that are printed as 0x-hex in the log (parsed as unsigned-ish ints,
# int(value, 0) accepts 0x... and decimal alike; we do not force unsigned so
# a broken producer is still visible in the output instead of being hidden).
HEX_FIELDS = frozenset(("model", "metaOutfit", "mount", "weapon",
                        "dynScenario", "pointType"))

_BOOL_LIKE = {"true": 1, "false": 0, "yes": 1, "no": 0}
_KEY_ALIASES = {name.lower(): name for name in CANONICAL_FIELDS}

DEFAULT_HASHDB = (Path(__file__).resolve().parents[1]
                  / "third_party" / "rdr2-hashdb" / "Peds.json")

# Verdict thresholds (kept in one place so docs and code cannot drift).
H1_PASS = 0.90          # >= 90% of unique human peds have metaOutfit != 0
H1_PARTIAL = 0.10       # 10..90% = partial, < 10% = fail
H2_PASS = 0.95          # >= 95% of equipped checks agree with hash != 0
H3_COVERAGE_PASS = 0.80 # >= 80% of scenario peds have dyn or point hash
H3_COVERAGE_PARTIAL = 0.30
H6_PASS = 0.05          # <= 5% human/name mismatches
H6_WARN = 0.20

# RDR2 weapon hash that means "no weapon in hands" (WEAPON_UNARMED); it must
# not be counted as an armed NPC in weapon statistics.
WEAPON_UNARMED = 0xA2719263

# ePedType values observed/proposed for human peds (hypothesis, see docs).
# 4/5 are the values emitted by the probe for human males/females in the
# 23:00 run; 0..3 are intentionally left unlabelled until more data exists.
PED_TYPE_LABELS = {
    4: "human male (hypothesis)",
    5: "human female (hypothesis)",
}
PED_TYPE_GENDERS = {4: "male", 5: "female"}

# Pool-composition heuristics for scan-summary lines. Scan lines carry no
# coordinates/timestamps, so "location segments" are inferred from the
# human/animal composition of consecutive ~2 s windows.
POOL_WINDOW_SECONDS = 2.0
POOL_WILD_MAX_SHARE = 0.20   # human share < 20%  -> wilderness/animals
POOL_TOWN_MIN_SHARE = 0.50   # human share >= 50% -> town/humans

GATING_VERDICTS = ("H1", "H2", "H3", "H6")

HUMAN_CLASSES = frozenset((
    "ambient_male", "ambient_female", "gang", "unique", "story",
    "cutscene", "multiplayer", "event", "mission_actor", "other_human",
))
ANIMAL_CLASSES = frozenset(("animal", "horse"))


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

def parse_probe_line(line):
    """Parse one log line; return a dict of canonical fields or None.

    Tolerant to: log prefixes/timestamps before ``probe:``, token order,
    extra whitespace/tabs, extra unknown tokens and ``true/false`` values
    for boolean-like fields. Malformed known fields make the whole record
    invalid (return None) so bad data is not silently counted.

    Summary lines (``probe: scan humans=... logged=... newCombos=... unique=...``)
    are NOT ped records and return None here; use parse_probe_summary_line().
    """
    if not isinstance(line, str):
        return None
    pos = line.find(PROBE_MARKER)
    if pos < 0:
        return None
    payload = line[pos + len(PROBE_MARKER):].strip()
    if not payload:
        return None
    if payload.lower().startswith("scan "):
        return None

    record = {}
    for token in payload.replace("\t", " ").split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        key = key.strip().rstrip(",;")
        value = value.strip().rstrip(",;")
        canonical = _KEY_ALIASES.get(key.lower())
        if canonical is None:
            continue
        lowered = value.lower()
        try:
            if lowered in _BOOL_LIKE:
                number = _BOOL_LIKE[lowered]
            else:
                try:
                    number = int(value, 0)
                except ValueError:
                    # Tolerate decimal values with a leading zero ("08"),
                    # which int(value, 0) rejects as a malformed octal.
                    number = int(value, 10)
        except (TypeError, ValueError):
            return None
        record[canonical] = number

    if "model" not in record:
        return None
    return record


def parse_probe_summary_line(line):
    """Parse the per-window summary line ``probe: scan humans=... ...``.

    The client emits it after logging the candidate peds of each 2 s window
    (task-38 added pool composition counters):
        probe: scan humans=13 animals=14 total=27 logged=12 newCombos=2 unique=37
    Returns a dict with keys humans/animals/total/logged/new_combos/unique
    (keys not present in older client builds are simply absent) or None.
    """
    if not isinstance(line, str):
        return None
    pos = line.find(PROBE_MARKER)
    if pos < 0:
        return None
    payload = line[pos + len(PROBE_MARKER):].strip()
    if not payload.lower().startswith("scan"):
        return None
    aliases = {
        "humans": "humans",
        "animals": "animals",
        "total": "total",
        "logged": "logged",
        "newcombos": "new_combos",
        "unique": "unique",
    }
    summary = {}
    for token in payload[4:].replace("\t", " ").split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        key = key.strip().rstrip(",;")
        value = value.strip().rstrip(",;")
        canonical = aliases.get(key.lower())
        if canonical is None:
            continue
        try:
            try:
                number = int(value, 0)
            except ValueError:
                number = int(value, 10)
        except (TypeError, ValueError):
            return None
        summary[canonical] = number
    if "humans" not in summary:
        return None
    return summary


def unique_signature(record):
    """Throttle-independent identity approximation (no ped handle in contract)."""
    return tuple(record.get(field) for field in CANONICAL_FIELDS)


def read_probe_file(path):
    """Stream a log file; return (records, file_meta)."""
    records = []
    summaries = []
    meta = {
        "lines": 0,
        "probe_marker_lines": 0,
        "invalid_probe_lines": 0,
        "probe_summary_lines": 0,
        "summaries": summaries,
    }
    with Path(path).open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            meta["lines"] += 1
            if PROBE_MARKER not in raw:
                continue
            meta["probe_marker_lines"] += 1
            summary = parse_probe_summary_line(raw)
            if summary is not None:
                meta["probe_summary_lines"] += 1
                summaries.append(summary)
                continue
            record = parse_probe_line(raw)
            if record is None:
                meta["invalid_probe_lines"] += 1
            else:
                records.append(record)
    return records, meta


# --------------------------------------------------------------------------
# Model / hashdb classification
# --------------------------------------------------------------------------

def load_hashdb(path=None):
    """Load model-hash -> name map from rdr2-hashdb Peds.json.

    Returns None when the file is missing/unreadable (analysis still works,
    model names/classes just become unavailable).
    """
    target = Path(path) if path is not None else DEFAULT_HASHDB
    try:
        data = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    result = {}
    for key, value in data.items():
        try:
            result[int(str(key), 10)] = str(value)
        except (TypeError, ValueError):
            continue
    return result or None


def model_class(name):
    """Map a rdr2-hashdb ped name to a coarse class used by the analyzer."""
    if not name:
        return "unnamed"
    lowered = name.lower()
    if lowered.startswith("a_c_horse_"):
        return "horse"
    if lowered.startswith("a_c_"):
        return "animal"
    if lowered.startswith(("p_c_", "male_skeleton")):
        return "creature"
    if lowered.startswith(("western_saddle", "motherhubbard_saddle",
                           "charro_saddle")):
        return "saddle_prop"
    if lowered.startswith("a_m_"):
        return "ambient_male"
    if lowered.startswith("a_f_"):
        return "ambient_female"
    if lowered.startswith(("g_m_", "g_f_")):
        return "gang"
    if lowered.startswith(("u_m_", "u_f_")):
        return "unique"
    if lowered.startswith(("s_m_", "s_f_")):
        return "story"
    if lowered.startswith("cs_"):
        return "cutscene"
    if lowered.startswith("mp_"):
        return "multiplayer"
    if lowered.startswith(("re_", "rc_")):
        return "event"
    if lowered.startswith(("msp_", "mes_", "rcsp_", "rces_", "mbh_", "gc_",
                           "charro_", "casp_", "loansharking_", "shack_")):
        return "mission_actor"
    if lowered.startswith("player_"):
        return "player"
    return "other_human"


def class_is_human(cls):
    return cls in HUMAN_CLASSES


def class_is_animal(cls):
    return cls in ANIMAL_CLASSES


def model_gender(name):
    """Best-effort gender from a rdr2-hashdb ped model name.

    Gender-coded prefixes (a_m_/a_f_/g_m_/g_f_/u_m_/u_f_/s_m_/s_f_) and
    MALE(S)/FEMALE(S) tokens are used; everything else is "unknown". This is
    used only to cross-check the pedType 4/5 -> male/female hypothesis.
    """
    if not name:
        return "unknown"
    upper = name.upper()
    if upper.startswith(("A_F_", "G_F_", "U_F_", "S_F_")):
        return "female"
    if upper.startswith(("A_M_", "G_M_", "U_M_", "S_M_")):
        return "male"
    if "FEMALES" in upper:
        return "female"
    if "MALES" in upper:
        return "male"
    return "unknown"


def model_name(hashdb, model_hash):
    if not hashdb:
        return None
    return hashdb.get(int(model_hash))


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------

def _is_nonzero(record, field):
    return bool(record.get(field, 0))


def _human_flag(record):
    """Return 'human', 'animal' or 'unknown' for the human= field."""
    value = record.get("human")
    if value is None:
        return "unknown"
    return "human" if value else "animal"


def _ratio(numerator, denominator):
    if not denominator:
        return None
    return float(numerator) / float(denominator)


def _counter_top(counter, top, hashdb=None):
    rows = []
    for value, count in counter.most_common(top):
        name = None
        cls = None
        if hashdb is not None:
            name = model_name(hashdb, value) if value else None
            if name is not None:
                cls = model_class(name)
        rows.append({
            "value": int(value),
            "count": int(count),
            "name": name,
            "class": cls,
        })
    return rows


# --------------------------------------------------------------------------
# Pool composition from scan-summary lines (humans=/animals=/total=)
# --------------------------------------------------------------------------

def pool_bucket(humans, total):
    """Heuristic location bucket for one scan window (no coords in the log)."""
    if total <= 0:
        return "empty"
    share = float(humans) / float(total)
    if share < POOL_WILD_MAX_SHARE:
        return "wild"
    if share < POOL_TOWN_MIN_SHARE:
        return "mixed"
    return "town"


def _shares(values):
    if not values:
        return {"min": None, "avg": None, "max": None}
    return {
        "min": min(values),
        "avg": sum(values) / float(len(values)),
        "max": max(values),
    }


def pool_stats(summaries):
    """Aggregate pool counters from ``probe: scan humans= animals= total=``.

    Returns totals over all complete windows plus bucket/segment breakdowns.
    The scan line has no coordinates/timestamps, so location/session split is
    inferred from the human/animal composition of consecutive windows.
    """
    windows = []
    for item in summaries:
        if not isinstance(item, dict):
            continue
        if not all(key in item for key in ("humans", "animals", "total")):
            continue
        windows.append({
            "humans": int(item.get("humans", 0)),
            "animals": int(item.get("animals", 0)),
            "total": int(item.get("total", 0)),
        })

    humans_sum = sum(window["humans"] for window in windows)
    animals_sum = sum(window["animals"] for window in windows)
    total_sum = sum(window["total"] for window in windows)
    human_shares = [float(window["humans"]) / window["total"]
                    for window in windows if window["total"] > 0]
    animal_shares = [float(window["animals"]) / window["total"]
                     for window in windows if window["total"] > 0]

    result = {
        "windows": len(windows),
        "humans_sum": humans_sum,
        "animals_sum": animals_sum,
        "total_sum": total_sum,
        "human_share": _ratio(humans_sum, total_sum),
        "animal_share": _ratio(animals_sum, total_sum),
        "humans_zero_windows": sum(1 for window in windows
                                   if window["humans"] == 0),
        "empty_windows": sum(1 for window in windows
                             if window["total"] == 0),
        "window_human_share": _shares(human_shares),
        "window_animal_share": _shares(animal_shares),
        "buckets": {},
        "segments": [],
        "segments_top": [],
        "runs": {},
    }

    bucket_aggs = {}
    for window in windows:
        bucket = pool_bucket(window["humans"], window["total"])
        agg = bucket_aggs.setdefault(bucket, {
            "windows": 0,
            "humans_sum": 0,
            "animals_sum": 0,
            "total_sum": 0,
        })
        agg["windows"] += 1
        agg["humans_sum"] += window["humans"]
        agg["animals_sum"] += window["animals"]
        agg["total_sum"] += window["total"]

    segments = []
    for window in windows:
        bucket = pool_bucket(window["humans"], window["total"])
        if not segments or segments[-1]["bucket"] != bucket:
            segments.append({
                "bucket": bucket,
                "windows": 0,
                "humans_sum": 0,
                "animals_sum": 0,
                "total_sum": 0,
            })
        segment = segments[-1]
        segment["windows"] += 1
        segment["humans_sum"] += window["humans"]
        segment["animals_sum"] += window["animals"]
        segment["total_sum"] += window["total"]

    for segment in segments:
        count = segment["windows"]
        segment["duration_s"] = round(count * POOL_WINDOW_SECONDS, 1)
        segment["humans_avg"] = segment["humans_sum"] / float(count)
        segment["animals_avg"] = segment["animals_sum"] / float(count)
        segment["total_avg"] = segment["total_sum"] / float(count)
        segment["human_share"] = _ratio(segment["humans_sum"],
                                        segment["total_sum"])
        segment["animal_share"] = _ratio(segment["animals_sum"],
                                         segment["total_sum"])

    runs = collections.Counter(segment["bucket"] for segment in segments)
    for bucket, agg in bucket_aggs.items():
        count = agg["windows"]
        agg["duration_s"] = round(count * POOL_WINDOW_SECONDS, 1)
        agg["humans_avg"] = agg["humans_sum"] / float(count)
        agg["animals_avg"] = agg["animals_sum"] / float(count)
        agg["total_avg"] = agg["total_sum"] / float(count)
        agg["human_share"] = _ratio(agg["humans_sum"], agg["total_sum"])
        agg["animal_share"] = _ratio(agg["animals_sum"], agg["total_sum"])
        agg["runs"] = runs.get(bucket, 0)

    result["buckets"] = bucket_aggs
    result["segments"] = segments
    result["segments_top"] = sorted(
        segments, key=lambda item: (item["windows"], item["humans_sum"]),
        reverse=True)[:8]
    result["runs"] = {key: int(value) for key, value in runs.items()}
    return result


def analyze_records(records, hashdb=None, top=8, file_meta=None):
    """Compute all distributions/verdicts for a list of parsed probe records."""
    meta = dict(file_meta or {})
    meta.setdefault("lines", None)
    meta.setdefault("probe_marker_lines", len(records))
    meta.setdefault("invalid_probe_lines", 0)
    meta.setdefault("probe_summary_lines", 0)
    meta.setdefault("summaries", [])

    unique_records = {}
    for record in records:
        unique_records.setdefault(unique_signature(record), record)
    unique_values = list(unique_records.values())

    models = collections.Counter(r.get("model", 0) for r in records)
    outfits = collections.Counter(r.get("metaOutfit", 0) for r in records
                                  if _is_nonzero(r, "metaOutfit"))
    dyn_scenarios = collections.Counter(r.get("dynScenario", 0) for r in records
                                        if _is_nonzero(r, "dynScenario"))
    point_types = collections.Counter(r.get("pointType", 0) for r in records
                                      if _is_nonzero(r, "pointType"))
    mounts = collections.Counter(r.get("mount", 0) for r in records
                                 if _is_nonzero(r, "mount"))
    weapon_values = [r.get("weapon", 0) for r in records]
    weapons = collections.Counter(value for value in weapon_values
                                  if value and value != WEAPON_UNARMED)

    human_counts = collections.Counter(_human_flag(r) for r in records)
    unique_human_counts = collections.Counter(_human_flag(r)
                                              for r in unique_values)

    def outfit_counts(subset):
        total = len(subset)
        nonzero = sum(1 for r in subset if _is_nonzero(r, "metaOutfit"))
        return {"total": total, "nonzero": nonzero, "zero": total - nonzero}

    human_records = [r for r in records if _human_flag(r) == "human"]
    animal_records = [r for r in records if _human_flag(r) == "animal"]
    unique_human = [r for r in unique_values if _human_flag(r) == "human"]
    unique_animal = [r for r in unique_values if _human_flag(r) == "animal"]

    equipped = {
        "outfit_nonzero": 0,
        "outfit_nonzero_equipped": 0,
        "outfit_nonzero_not_equipped": 0,
        "outfit_zero": 0,
        "outfit_zero_equipped": 0,
        "missing": 0,
    }
    for record in records:
        if "equipped" not in record:
            equipped["missing"] += 1
            continue
        is_equipped = bool(record.get("equipped"))
        if _is_nonzero(record, "metaOutfit"):
            equipped["outfit_nonzero"] += 1
            if is_equipped:
                equipped["outfit_nonzero_equipped"] += 1
            else:
                equipped["outfit_nonzero_not_equipped"] += 1
        else:
            equipped["outfit_zero"] += 1
            if is_equipped:
                equipped["outfit_zero_equipped"] += 1

    scenario = {
        "any": 0,
        "any_dyn_nonzero": 0,
        "any_point_nonzero": 0,
        "any_detected": 0,
        "any_both_zero": 0,
        "no_any_dyn_nonzero": 0,
        "no_any_point_nonzero": 0,
    }
    for record in records:
        if _is_nonzero(record, "scenarioAny"):
            scenario["any"] += 1
            dyn = _is_nonzero(record, "dynScenario")
            point = _is_nonzero(record, "pointType")
            if dyn:
                scenario["any_dyn_nonzero"] += 1
            if point:
                scenario["any_point_nonzero"] += 1
            if dyn or point:
                scenario["any_detected"] += 1
            else:
                scenario["any_both_zero"] += 1
        else:
            if _is_nonzero(record, "dynScenario"):
                scenario["no_any_dyn_nonzero"] += 1
            if _is_nonzero(record, "pointType"):
                scenario["no_any_point_nonzero"] += 1

    ped_types = collections.Counter(r.get("pedType") for r in records
                                    if "pedType" in r)
    ped_type_table = []
    for value, count in sorted(ped_types.items(),
                               key=lambda item: str(item[0])):
        try:
            numeric = int(value)
        except (TypeError, ValueError):
            numeric = None
        ped_type_table.append({
            "value": value,
            "label": PED_TYPE_LABELS.get(numeric, "unknown")
            if numeric is not None else "unknown",
            "count": count,
        })

    # Cross-check the pedType 4/5 -> male/female hypothesis against gender
    # coded model names (a_m_*/a_f_*, MALES/FEMALES tokens). This is a
    # hypothesis check only: 24/49 records in the 23:00 run had no gender
    # coded model name and stay "unknown".
    ped_type_gender = {
        "known": 0,
        "match": 0,
        "mismatch": 0,
        "unknown": 0,
        "examples": [],
    }
    for record in records:
        expected = PED_TYPE_GENDERS.get(record.get("pedType"))
        if expected is None:
            continue
        name = model_name(hashdb, record.get("model", 0))
        actual = model_gender(name)
        if actual == "unknown":
            ped_type_gender["unknown"] += 1
            continue
        ped_type_gender["known"] += 1
        if actual == expected:
            ped_type_gender["match"] += 1
        else:
            ped_type_gender["mismatch"] += 1
            if len(ped_type_gender["examples"]) < 5:
                ped_type_gender["examples"].append({
                    "model": record.get("model", 0),
                    "name": name,
                    "pedType": record.get("pedType"),
                    "expected": expected,
                    "model_gender": actual,
                })

    missions = sum(1 for r in records if _is_nonzero(r, "mission"))

    class_stats = {}
    mismatch = {"known": 0, "mismatch": 0, "examples": []}
    for record in records:
        name = model_name(hashdb, record.get("model", 0))
        cls = model_class(name) if name is not None else "unknown"
        bucket = class_stats.setdefault(cls, {
            "count": 0,
            "outfit_nonzero": 0,
            "mount_nonzero": 0,
            "weapon_nonzero": 0,
            "weapon_armed": 0,
            "scenario_any": 0,
            "dyn_nonzero": 0,
            "point_nonzero": 0,
            "human_flag": 0,
        })
        bucket["count"] += 1
        bucket["outfit_nonzero"] += int(_is_nonzero(record, "metaOutfit"))
        bucket["mount_nonzero"] += int(_is_nonzero(record, "mount"))
        weapon = record.get("weapon", 0)
        bucket["weapon_nonzero"] += int(bool(weapon))
        bucket["weapon_armed"] += int(bool(weapon) and weapon != WEAPON_UNARMED)
        bucket["scenario_any"] += int(_is_nonzero(record, "scenarioAny"))
        bucket["dyn_nonzero"] += int(_is_nonzero(record, "dynScenario"))
        bucket["point_nonzero"] += int(_is_nonzero(record, "pointType"))
        bucket["human_flag"] += int(_human_flag(record) == "human")

        if name is not None and (class_is_human(cls) or class_is_animal(cls)):
            mismatch["known"] += 1
            flag_human = _human_flag(record) == "human"
            name_human = class_is_human(cls)
            if flag_human != name_human:
                mismatch["mismatch"] += 1
                if len(mismatch["examples"]) < 5:
                    mismatch["examples"].append({
                        "model": record.get("model", 0),
                        "name": name,
                        "human": record.get("human"),
                    })

    missing_fields = {
        field: sum(1 for r in records if field not in r)
        for field in CANONICAL_FIELDS
    }

    # Per-window summary lines emitted by the client:
    #   probe: scan humans=H animals=A total=T logged=L newCombos=C unique=U
    summaries = [item for item in (meta.get("summaries") or [])
                 if isinstance(item, dict)]
    humans_values = [int(item.get("humans", 0)) for item in summaries]
    logged_values = [int(item.get("logged", 0)) for item in summaries]
    new_combos_values = [int(item.get("new_combos", 0)) for item in summaries]
    unique_combos_values = [int(item.get("unique", 0)) for item in summaries]
    pool = pool_stats(summaries)
    scan = {
        "windows": len(summaries),
        "humans_max": max(humans_values) if humans_values else 0,
        "humans_avg": (sum(humans_values) / float(len(humans_values))
                       if humans_values else None),
        "logged_max": max(logged_values) if logged_values else 0,
        "logged_avg": (sum(logged_values) / float(len(logged_values))
                       if logged_values else None),
        "new_combos_total": sum(new_combos_values),
        "unique_max": max(unique_combos_values) if unique_combos_values else 0,
        "pool": pool,
    }

    stats = {
        "records": len(records),
        "unique_signatures": len(unique_values),
        "unique_values": unique_values,
        "unique_models": len(models),
        "unique_outfits": len(outfits),
        "missing_fields": missing_fields,
        "human": {
            "human": human_counts.get("human", 0),
            "animal": human_counts.get("animal", 0),
            "unknown": human_counts.get("unknown", 0),
            "unique_human": unique_human_counts.get("human", 0),
            "unique_animal": unique_human_counts.get("animal", 0),
            "unique_unknown": unique_human_counts.get("unknown", 0),
        },
        "outfit": {
            "all": outfit_counts(records),
            "human": outfit_counts(human_records),
            "animal": outfit_counts(animal_records),
            "unique_all": outfit_counts(unique_values),
            "unique_human": outfit_counts(unique_human),
            "unique_animal": outfit_counts(unique_animal),
        },
        "equipped": equipped,
        "scenario": scenario,
        "mount": {
            "nonzero": sum(1 for r in records if _is_nonzero(r, "mount")),
            "records": len(records),
        },
        "weapon": {
            "records": len(records),
            # Raw "hash != 0" count, kept for backward compatibility/debug.
            "nonzero": sum(1 for value in weapon_values if value),
            "raw_nonzero": sum(1 for value in weapon_values if value),
            # Armed = a real weapon hash, excluding WEAPON_UNARMED (0xA2719263).
            "armed": sum(1 for value in weapon_values
                         if value and value != WEAPON_UNARMED),
            "unarmed": sum(1 for value in weapon_values
                           if value == WEAPON_UNARMED),
            "zero": sum(1 for value in weapon_values if value == 0),
        },
        "mission": {
            "nonzero": missions,
            "records": len(records),
        },
        "ped_types": {str(key): value for key, value in sorted(ped_types.items(),
                                                               key=lambda kv: str(kv[0]))},
        "ped_type": {
            "table": ped_type_table,
            "gender_check": ped_type_gender,
        },
        "classes": dict(sorted(class_stats.items())),
        "models_top": _counter_top(models, top, hashdb),
        "outfits_top": _counter_top(outfits, top, None),
        "dyn_top": _counter_top(dyn_scenarios, top, None),
        "point_top": _counter_top(point_types, top, None),
        "mount_top": _counter_top(mounts, top, hashdb),
        "weapon_top": _counter_top(weapons, top, None),
        "human_name_mismatch": mismatch,
        "scan": scan,
        "hashdb_available": hashdb is not None,
    }

    verdicts = build_verdicts(stats)
    return {
        "meta": meta,
        "stats": stats,
        "verdicts": verdicts,
    }


def _verdict(verdict_id, title, status, value, detail):
    return {
        "id": verdict_id,
        "title": title,
        "status": status,
        "value": value,
        "detail": detail,
    }


def build_verdicts(stats):
    """Turn distributions into E-1 hypothesis verdicts."""
    verdicts = []

    # H1: meta outfit hash readable for human NPCs (unique signatures are used
    # to damp the client-side throttle bias).
    unique_human = stats["outfit"]["unique_human"]
    h1_ratio = _ratio(unique_human["nonzero"], unique_human["total"])
    if unique_human["total"] == 0:
        verdicts.append(_verdict(
            "H1", "meta outfit hash readable for human NPCs", "INSUFFICIENT",
            None,
            "no human records in the probe log; run probe in a populated area"))
    elif h1_ratio is not None and h1_ratio >= H1_PASS:
        verdicts.append(_verdict(
            "H1", "meta outfit hash readable for human NPCs", "PASS", h1_ratio,
            "%d/%d unique human peds have metaOutfit != 0"
            % (unique_human["nonzero"], unique_human["total"])))
    elif h1_ratio is not None and h1_ratio >= H1_PARTIAL:
        verdicts.append(_verdict(
            "H1", "meta outfit hash readable for human NPCs", "PARTIAL", h1_ratio,
            "%d/%d unique human peds have metaOutfit != 0; use per-class fallback"
            % (unique_human["nonzero"], unique_human["total"])))
    else:
        verdicts.append(_verdict(
            "H1", "meta outfit hash readable for human NPCs", "FAIL", h1_ratio,
            "only %d/%d unique human peds have metaOutfit != 0; "
            "need metaped_outfits brute force / component capture"
            % (unique_human["nonzero"], unique_human["total"])))

    # H2: equipped flag agrees with metaOutfit != 0.
    equipped = stats["equipped"]
    if equipped["outfit_nonzero"] == 0 or equipped["missing"] > 0:
        detail = ("no equipped data" if equipped["outfit_nonzero"] == 0
                  else "equipped field missing in %d record(s)"
                       % equipped["missing"])
        verdicts.append(_verdict(
            "H2", "_IS_META_PED_OUTFIT_EQUIPPED agrees with outfit hash",
            "UNKNOWN", None, detail))
    else:
        h2_ratio = _ratio(equipped["outfit_nonzero_equipped"],
                          equipped["outfit_nonzero"])
        if h2_ratio is not None and h2_ratio >= H2_PASS:
            status = "PASS"
        elif h2_ratio is not None and h2_ratio > 0.0:
            status = "WARN"
        else:
            status = "FAIL"
        verdicts.append(_verdict(
            "H2", "_IS_META_PED_OUTFIT_EQUIPPED agrees with outfit hash",
            status, h2_ratio,
            "%d/%d records with metaOutfit != 0 report equipped=1 "
            "(outfit==0 records with equipped=1: %d)"
            % (equipped["outfit_nonzero_equipped"],
               equipped["outfit_nonzero"],
               equipped["outfit_zero_equipped"])))

    # H3: scenario detection coverage (dynamic scenario and/or point type).
    scenario = stats["scenario"]
    if scenario["any"] == 0:
        verdicts.append(_verdict(
            "H3", "scenario detection coverage", "INSUFFICIENT", None,
            "no record with scenarioAny=1; probe more NPCs in/around camps, "
            "saloons, benches, beds"))
    else:
        coverage = _ratio(scenario["any_detected"], scenario["any"])
        dyn_ratio = _ratio(scenario["any_dyn_nonzero"], scenario["any"])
        point_ratio = _ratio(scenario["any_point_nonzero"], scenario["any"])
        if coverage is not None and coverage >= H3_COVERAGE_PASS:
            status = "PASS"
            chain = ("dynamic-scenario chain"
                     if (dyn_ratio or 0.0) >= (point_ratio or 0.0)
                     else "scenario-point-type chain")
            detail = ("%d/%d scenario peds detected (dyn=%.0f%%, point=%.0f%%); "
                      "preferred chain: %s"
                      % (scenario["any_detected"], scenario["any"],
                         (dyn_ratio or 0.0) * 100.0,
                         (point_ratio or 0.0) * 100.0, chain))
        elif coverage is not None and coverage >= H3_COVERAGE_PARTIAL:
            status = "PARTIAL"
            detail = ("%d/%d scenario peds detected (dyn=%.0f%%, point=%.0f%%); "
                      "use hash replay with fallbacks, expect pose gaps"
                      % (scenario["any_detected"], scenario["any"],
                         (dyn_ratio or 0.0) * 100.0,
                         (point_ratio or 0.0) * 100.0))
        else:
            status = "FAIL"
            detail = ("only %d/%d scenario peds detected (dyn=%.0f%%, "
                      "point=%.0f%%); replan scenario detection (extra probe "
                      "fields / boolean probe over known scenario hashes)"
                      % (scenario["any_detected"], scenario["any"],
                         (dyn_ratio or 0.0) * 100.0,
                         (point_ratio or 0.0) * 100.0))
        verdicts.append(_verdict("H3", "scenario detection coverage", status,
                                 coverage, detail))

    # H4: human vs animal pool ratio is observable. Prefer the scan-summary
    # pool counters (task-38: humans=/animals=/total= count ALL peds in the
    # radius before the human top-20 filter), because per-ped records are
    # deliberately human-only.
    scan = stats.get("scan") or {}
    pool = scan.get("pool") or {}
    if pool.get("windows") and (pool.get("total_sum") or 0) > 0:
        human_share = pool.get("human_share")
        animal_share = pool.get("animal_share")
        buckets = pool.get("buckets") or {}

        def _bucket_animal_share(bucket_name):
            bucket = buckets.get(bucket_name) or {}
            share = bucket.get("animal_share")
            if share is None:
                return "n/a"
            return "%.1f%%" % (share * 100.0)

        detail = (
            "pool counters from %d scan windows: humans=%d, animals=%d, "
            "total=%d -> human %.1f%% / animal %.1f%%; animal share is "
            "strongly location-dependent (wild windows animal %s, town "
            "windows animal %s); per-ped records remain human-only"
            % (pool["windows"], pool.get("humans_sum", 0),
               pool.get("animals_sum", 0), pool.get("total_sum", 0),
               (human_share or 0.0) * 100.0, (animal_share or 0.0) * 100.0,
               _bucket_animal_share("wild"), _bucket_animal_share("town")))
        verdicts.append(_verdict(
            "H4", "human vs animal pool ratio observable", "PASS",
            {"human_share": human_share, "animal_share": animal_share},
            detail))
    else:
        known_flags = stats["human"]["human"] + stats["human"]["animal"]
        records = stats["records"]
        if known_flags == 0:
            verdicts.append(_verdict(
                "H4", "human vs animal ratio observable", "INSUFFICIENT", None,
                "human= field is missing in every record and no scan-summary "
                "pool counters (humans=/animals=/total=) were found"))
        elif stats["human"]["animal"] == 0:
            verdicts.append(_verdict(
                "H4", "human vs animal ratio observable", "WARN",
                _ratio(known_flags, records),
                "no animal records and no scan-summary pool counters; the "
                "current probe logs only human peds per line - animal share "
                "cannot be measured from per-ped records alone"))
        else:
            verdicts.append(_verdict(
                "H4", "human vs animal ratio observable", "PASS",
                _ratio(known_flags, records),
                "human=%d, animal=%d (%s of records have a valid human flag)"
                % (stats["human"]["human"], stats["human"]["animal"],
                   "%.1f%%" % (100.0 * known_flags / records))))

    # H5: mount/weapon info present (informational, no gate).
    weapon = stats["weapon"]
    records = stats["records"]
    mount_ratio = _ratio(stats["mount"]["nonzero"], stats["mount"]["records"])
    armed_ratio = _ratio(weapon["armed"], records)
    unarmed_ratio = _ratio(weapon["unarmed"], records)
    verdicts.append(_verdict(
        "H5", "mount/weapon fields populated (WEAPON_UNARMED excluded)",
        "INFO",
        {"mount": mount_ratio, "armed": armed_ratio,
         "unarmed": unarmed_ratio},
        "mount!=0: %d/%d (%.1f%%); armed (hash != 0 and != WEAPON_UNARMED "
        "0xA2719263): %d/%d (%.1f%%); unarmed: %d/%d; weapon=0: %d/%d "
        "(informational; mount coverage needs a road/camp probe)"
        % (stats["mount"]["nonzero"], stats["mount"]["records"],
           (mount_ratio or 0.0) * 100.0,
           weapon["armed"], records, (armed_ratio or 0.0) * 100.0,
           weapon["unarmed"], records, weapon["zero"], records)))

    # H6: human flag vs model name (contract sanity).
    mismatch = stats["human_name_mismatch"]
    if not stats["hashdb_available"] or mismatch["known"] == 0:
        verdicts.append(_verdict(
            "H6", "human flag agrees with model name", "UNKNOWN", None,
            "hashdb (third_party/rdr2-hashdb/Peds.json) unavailable or no "
            "records matched a known model name"))
    else:
        h6_ratio = _ratio(mismatch["mismatch"], mismatch["known"])
        if h6_ratio is not None and h6_ratio <= H6_PASS:
            status = "PASS"
        elif h6_ratio is not None and h6_ratio <= H6_WARN:
            status = "WARN"
        else:
            status = "FAIL"
        verdicts.append(_verdict(
            "H6", "human flag agrees with model name", status, h6_ratio,
            "%d/%d known-model records disagree with IS_PED_HUMAN"
            % (mismatch["mismatch"], mismatch["known"])))

    return verdicts


# --------------------------------------------------------------------------
# Report / CLI
# --------------------------------------------------------------------------

def _print_table(title, headers, rows):
    print()
    print(title)
    if not rows:
        print("  (no data)")
        return
    widths = [len(str(h)) for h in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(str(value)))
    print("  " + "  ".join(str(h).ljust(widths[i])
                           for i, h in enumerate(headers)))
    print("  " + "  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rows:
        print("  " + "  ".join(str(value).ljust(widths[i])
                               for i, value in enumerate(row)))


def _fmt_hex(value):
    if value is None:
        return "n/a"
    if isinstance(value, bool):
        return "1" if value else "0"
    try:
        return "0x%08X" % (int(value) & 0xFFFFFFFF)
    except (TypeError, ValueError):
        return str(value)


def _fmt_pct(numerator, denominator):
    ratio = _ratio(numerator, denominator)
    if ratio is None:
        return "n/a"
    return "%.1f%%" % (ratio * 100.0)


def _fmt_share_stats(values):
    """Format {'min','avg','max'} shares as 'min% / avg% / max%'."""
    if not values or values.get("avg") is None:
        return "n/a"
    return "%.1f%% / %.1f%% / %.1f%%" % (
        values["min"] * 100.0, values["avg"] * 100.0, values["max"] * 100.0)


def _fmt_number(value, digits=1):
    if value is None:
        return "n/a"
    return ("%." + str(digits) + "f") % value


def print_report(report):
    meta = report["meta"]
    stats = report["stats"]

    print("=" * 78)
    print("E-1 NPC probe analysis")
    print("=" * 78)
    if meta.get("lines") is not None:
        print("log lines: %d, probe marker lines: %d, parsed: %d, invalid: %d, "
              "scan summaries: %d"
              % (meta["lines"], meta["probe_marker_lines"],
                 stats["records"], meta["invalid_probe_lines"],
                 meta.get("probe_summary_lines", 0)))
    print("hashdb: %s"
          % ("loaded" if stats["hashdb_available"] else "not available"))
    print("records: %d, unique signatures: %d, unique models: %d, "
          "unique outfits: %d"
          % (stats["records"], stats["unique_signatures"],
             stats["unique_models"], stats["unique_outfits"]))

    scan = stats.get("scan") or {}
    pool = scan.get("pool") or {}
    if scan.get("windows"):
        _print_table("Pool totals by scan windows (client summary lines)", [
            "metric", "value",
        ], [
            ["scan windows", scan["windows"]],
            ["windows with pool counters", pool.get("windows", 0)],
            ["pool humans (sum)", pool.get("humans_sum", "n/a")],
            ["pool animals (sum)", pool.get("animals_sum", "n/a")],
            ["pool total (sum)", pool.get("total_sum", "n/a")],
            ["human share (pool sum)",
             _fmt_pct(pool.get("humans_sum", 0), pool.get("total_sum", 0))],
            ["animal share (pool sum)",
             _fmt_pct(pool.get("animals_sum", 0), pool.get("total_sum", 0))],
            ["window human share min/avg/max",
             _fmt_share_stats(pool.get("window_human_share"))],
            ["window animal share min/avg/max",
             _fmt_share_stats(pool.get("window_animal_share"))],
            ["windows humans=0", pool.get("humans_zero_windows", 0)],
            ["empty windows (total=0)", pool.get("empty_windows", 0)],
            ["logged peds (max)", scan["logged_max"]],
            ["logged peds (avg)", _fmt_number(scan["logged_avg"], 2)],
            ["new combos total", scan["new_combos_total"]],
            ["unique combos (max)", scan["unique_max"]],
        ])

        bucket_rows = []
        for bucket in ("wild", "mixed", "town", "empty"):
            agg = (pool.get("buckets") or {}).get(bucket)
            if not agg:
                continue
            bucket_rows.append([
                bucket, agg["windows"], agg.get("runs", 0),
                _fmt_number(agg.get("duration_s"), 0),
                _fmt_number(agg.get("humans_avg"), 1),
                _fmt_number(agg.get("animals_avg"), 1),
                _fmt_pct(agg.get("humans_sum", 0), agg.get("total_sum", 0)),
                _fmt_pct(agg.get("animals_sum", 0), agg.get("total_sum", 0)),
            ])
        _print_table("Pool by location heuristic (scan windows: wild<20% "
                     "humans, town>=50%)", [
            "bucket", "windows", "runs", "duration_s", "humans_avg",
            "animals_avg", "human_share", "animal_share",
        ], bucket_rows)

        segment_rows = []
        for index, segment in enumerate(pool.get("segments_top") or [], 1):
            segment_rows.append([
                index, segment["bucket"], segment["windows"],
                _fmt_number(segment.get("duration_s"), 0),
                _fmt_number(segment.get("humans_avg"), 1),
                _fmt_number(segment.get("animals_avg"), 1),
                _fmt_pct(segment.get("humans_sum", 0),
                         segment.get("total_sum", 0)),
                _fmt_pct(segment.get("animals_sum", 0),
                         segment.get("total_sum", 0)),
            ])
        _print_table("Longest pool segments (location heuristic)", [
            "#", "bucket", "windows", "duration_s", "humans_avg",
            "animals_avg", "human_share", "animal_share",
        ], segment_rows)
        print("  NOTE: scan lines carry no coordinates; location/segment split")
        print("        is inferred from pool composition. Animal share is")
        print("        location-dependent (town vs wilderness) - run separate")
        print("        probe sessions per location for exact numbers.")

    human = stats["human"]
    _print_table("Pool: human vs animal (human= field)", [
        "group", "records", "share", "unique signatures",
    ], [
        ["human", human["human"],
         _fmt_pct(human["human"], stats["records"]), human["unique_human"]],
        ["animal", human["animal"],
         _fmt_pct(human["animal"], stats["records"]), human["unique_animal"]],
        ["unknown", human["unknown"],
         _fmt_pct(human["unknown"], stats["records"]), human["unique_unknown"]],
    ])

    outfit = stats["outfit"]
    _print_table("Outfits: metaOutfit != 0", [
        "group", "outfit != 0", "total", "share",
    ], [
        ["all", outfit["all"]["nonzero"], outfit["all"]["total"],
         _fmt_pct(outfit["all"]["nonzero"], outfit["all"]["total"])],
        ["human", outfit["human"]["nonzero"], outfit["human"]["total"],
         _fmt_pct(outfit["human"]["nonzero"], outfit["human"]["total"])],
        ["animal", outfit["animal"]["nonzero"], outfit["animal"]["total"],
         _fmt_pct(outfit["animal"]["nonzero"], outfit["animal"]["total"])],
        ["unique human", outfit["unique_human"]["nonzero"],
         outfit["unique_human"]["total"],
         _fmt_pct(outfit["unique_human"]["nonzero"],
                  outfit["unique_human"]["total"])],
    ])

    equipped = stats["equipped"]
    _print_table("Equipped consistency (metaOutfit != 0 => equipped=1)", [
        "outfit", "records", "equipped=1", "share",
    ], [
        ["metaOutfit != 0", equipped["outfit_nonzero"],
         equipped["outfit_nonzero_equipped"],
         _fmt_pct(equipped["outfit_nonzero_equipped"],
                  equipped["outfit_nonzero"])],
        ["metaOutfit == 0", equipped["outfit_zero"],
         equipped["outfit_zero_equipped"],
         _fmt_pct(equipped["outfit_zero_equipped"],
                  equipped["outfit_zero"])],
    ])

    scenario = stats["scenario"]
    _print_table("Scenarios (scenarioAny / dynamic scenario / point type)", [
        "metric", "count", "share of scenarioAny",
    ], [
        ["scenarioAny=1", scenario["any"], "100.0%"],
        ["dynScenario != 0", scenario["any_dyn_nonzero"],
         _fmt_pct(scenario["any_dyn_nonzero"], scenario["any"])],
        ["pointType != 0", scenario["any_point_nonzero"],
         _fmt_pct(scenario["any_point_nonzero"], scenario["any"])],
        ["dyn or point detected", scenario["any_detected"],
         _fmt_pct(scenario["any_detected"], scenario["any"])],
        ["both zero (undetected)", scenario["any_both_zero"],
         _fmt_pct(scenario["any_both_zero"], scenario["any"])],
    ])

    weapon = stats["weapon"]
    _print_table("Mount / weapon / mission", [
        "metric", "count", "total", "share",
    ], [
        ["mount != 0", stats["mount"]["nonzero"], stats["mount"]["records"],
         _fmt_pct(stats["mount"]["nonzero"], stats["mount"]["records"])],
        ["armed (weapon != 0, != Unarmed)", weapon["armed"],
         weapon["records"], _fmt_pct(weapon["armed"], weapon["records"])],
        ["unarmed (0xA2719263)", weapon["unarmed"], weapon["records"],
         _fmt_pct(weapon["unarmed"], weapon["records"])],
        ["weapon == 0", weapon["zero"], weapon["records"],
         _fmt_pct(weapon["zero"], weapon["records"])],
        ["weapon != 0 (raw)", weapon["nonzero"], weapon["records"],
         _fmt_pct(weapon["nonzero"], weapon["records"])],
        ["mission != 0", stats["mission"]["nonzero"],
         stats["mission"]["records"],
         _fmt_pct(stats["mission"]["nonzero"], stats["mission"]["records"])],
    ])

    ped_type = stats.get("ped_type") or {}
    _print_table("pedType distribution (GET_PED_TYPE; 4/5 labels are a "
                 "hypothesis)", ["pedType", "label", "records", "share"], [
        [row["value"], row["label"], row["count"],
         _fmt_pct(row["count"], stats["records"])]
        for row in ped_type.get("table") or []
    ])
    gender_check = ped_type.get("gender_check") or {}
    if gender_check:
        _print_table("pedType gender check (4=male, 5=female; vs model name)", [
            "known", "match", "mismatch", "unknown",
        ], [[gender_check.get("known", 0), gender_check.get("match", 0),
             gender_check.get("mismatch", 0),
             gender_check.get("unknown", 0)]])

    _print_table("Model classes (hashdb names)", [
        "class", "records", "outfit!=0", "mount", "armed", "scenarioAny",
    ], [
        [name, bucket["count"], _fmt_pct(bucket["outfit_nonzero"],
                                         bucket["count"]),
         bucket["mount_nonzero"], bucket.get("weapon_armed", 0),
         bucket["scenario_any"]]
        for name, bucket in stats["classes"].items()
    ])

    _print_table("Top models (by probe records)", [
        "model", "name", "class", "count",
    ], [
        [_fmt_hex(row["value"]), row["name"] or "?", row["class"] or "?",
         row["count"]]
        for row in stats["models_top"]
    ])

    for title, key in (
        ("Top meta outfits", "outfits_top"),
        ("Top dynamic scenarios", "dyn_top"),
        ("Top scenario point types", "point_top"),
        ("Top mount models/handles", "mount_top"),
        ("Top armed weapons (Unarmed excluded)", "weapon_top"),
    ):
        rows = []
        for row in stats[key]:
            rows.append([_fmt_hex(row["value"]), row["name"] or "-",
                         row["count"]])
        _print_table(title, ["hash", "name", "count"], rows)

    print()
    print("Verdicts")
    print("-" * 78)
    for verdict in report["verdicts"]:
        value = verdict["value"]
        if isinstance(value, float):
            value = "%.3f" % value
        elif isinstance(value, dict):
            value = ", ".join("%s=%.3f" % (k, v)
                              for k, v in sorted(value.items())
                              if isinstance(v, float))
        print("[%s] %s: %s" % (verdict["status"], verdict["id"],
                               verdict["title"]))
        print("      %s" % verdict["detail"])
        if value not in (None, ""):
            print("      value: %s" % value)

    print()
    print("E1 plan hint")
    print("-" * 78)
    for hint in e1_plan_hint(report["verdicts"]):
        print("* " + hint)


def e1_plan_hint(verdicts):
    """Short E1 plan hints derived from H1/H3/H4 verdicts."""
    by_id = {verdict["id"]: verdict for verdict in verdicts}
    hints = []

    h1 = by_id.get("H1", {})
    h1_status = h1.get("status")
    if h1_status == "PASS":
        hints.append("H1 PASS -> E1-A: appearance via _REQUEST_META_PED_OUTFIT "
                     "+ _APPLY_PED_META_PED_OUTFIT for every actor; send "
                     "metaOutfit hash in ActorOutfit (protocol v15 0x1C).")
    elif h1_status == "PARTIAL":
        hints.append("H1 PARTIAL -> E1-A only for the minority with hash != 0; "
                     "for the rest use E1-B brute force "
                     "(_IS_META_PED_OUTFIT_EQUIPPED over rdr3-discoveries "
                     "metaped_outfits.lua per model) or E1-C per-component "
                     "asset/tint capture (GET_META_PED_ASSET_GUIDS/TINT + "
                     "_SET_META_PED_TAG).")
    elif h1_status == "FAIL":
        hints.append("H1 FAIL -> E1-B first: brute force equipped outfits "
                     "from metaped_outfits.lua; if that also fails, E1-C "
                     "component capture (GET_META_PED_ASSET_GUIDS/TINT + "
                     "_SET_META_PED_TAG) with a dedicated appearance message.")
    else:
        hints.append("H1 INSUFFICIENT -> re-run the probe in a populated town "
                     "and keep npc_debug=1 for 2-3 minutes.")

    h3 = by_id.get("H3", {})
    h3_status = h3.get("status")
    if h3_status == "PASS":
        hints.append("H3 PASS -> replay scenarios; prefer the chain with the "
                     "higher coverage from the verdict detail: scenario-point "
                     "chain (CREATE_SCENARIO_POINT_HASH + TASK_USE_SCENARIO_"
                     "POINT, keeps props) or dynamic hash "
                     "(TASK_START_SCENARIO_IN_PLACE_HASH).")
    elif h3_status == "PARTIAL":
        hints.append("H3 PARTIAL -> implement scenario replay for the "
                     "detected subset; for undetected peds fall back to "
                     "TASK_STAND_STILL/idle and document pose gaps.")
    elif h3_status == "FAIL":
        hints.append("H3 FAIL -> extend the probe before E1-D "
                     "(PED_HAS_USE_SCENARIO_TASK, _GET_SCENARIO_POINT_PED_IS_"
                     "USING, boolean probe over known scenario hashes).")
    else:
        hints.append("H3 INSUFFICIENT -> probe saloons/camps/benches/beds "
                     "where NPCs sit, smoke, drink, sleep.")

    h4 = by_id.get("H4", {})
    if h4.get("status") == "PASS":
        value = h4.get("value") or {}
        human_share = value.get("human_share")
        animal_share = value.get("animal_share")
        hints.append("H4 PASS -> pool share is measurable from scan counters "
                     "(human %s, animal %s overall); animal share is "
                     "location-dependent -> E1 stays human-only, animals/"
                     "horses are a separate E2 scope with per-location budgets."
                     % ("%.1f%%" % (human_share * 100.0)
                        if human_share is not None else "n/a",
                        "%.1f%%" % (animal_share * 100.0)
                        if animal_share is not None else "n/a"))
    elif h4.get("status") == "WARN":
        hints.append("H4 WARN -> probe is human-filtered (task-34), animal "
                     "share is not measurable; add an all-peds probe mode "
                     "before E1/E2 animal scope.")
    h5 = by_id.get("H5", {})
    h5_value = h5.get("value") or {}
    if isinstance(h5_value, dict) and h5_value.get("armed") is not None:
        hints.append("H5 INFO -> armed %.0f%% of sampled humans (WEAPON_UNARMED "
                     "0xA2719263 excluded); most NPCs are unarmed, so combat "
                     "replication is lower priority than appearance/locomotion."
                     % (h5_value["armed"] * 100.0))
    h2 = by_id.get("H2", {})
    if h2.get("status") in ("FAIL", "UNKNOWN"):
        hints.append("H2 not PASS -> verify _IS_META_PED_OUTFIT_EQUIPPED "
                     "hash/arity/argument order; do not rely on equipped yet.")
    return hints


def _json_safe(value):
    """Convert sets/Path-like values so json.dumps always works."""
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    if isinstance(value, set):
        return sorted(_json_safe(item) for item in value)
    if isinstance(value, Path):
        return str(value)
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Analyze E-1 NPC probe lines from rdr2-coop.log",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python tests/npc_probe_analyze.py rdr2-coop.log\n"
            "  python tests/npc_probe_analyze.py rdr2-coop.log --strict\n"
            "  python tests/npc_probe_analyze.py rdr2-coop.log --json\n"
        ),
    )
    parser.add_argument("log", help="path to a copy of rdr2-coop.log")
    parser.add_argument("--top", type=int, default=8,
                        help="rows per top-list (default: 8)")
    parser.add_argument("--hashdb", default=None,
                        help="optional path to rdr2-hashdb Peds.json")
    parser.add_argument("--json", action="store_true",
                        help="print machine-readable JSON only")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a gating verdict (H1/H2/H3/H6) is FAIL")
    args = parser.parse_args(argv)

    if args.top < 1:
        print("error: --top must be >= 1", file=sys.stderr)
        return 2

    hashdb = load_hashdb(args.hashdb) if args.hashdb else load_hashdb()
    if args.hashdb and hashdb is None:
        print("warning: cannot load hashdb %s; model names unavailable"
              % args.hashdb, file=sys.stderr)

    try:
        records, file_meta = read_probe_file(args.log)
    except OSError as exc:
        print("error: cannot read %s: %s" % (args.log, exc), file=sys.stderr)
        return 2

    report = analyze_records(records, hashdb=hashdb, top=args.top,
                             file_meta=file_meta)
    report["file"] = str(args.log)
    report["meta"]["hashdb_path"] = (str(args.hashdb) if args.hashdb
                                     else str(DEFAULT_HASHDB))

    if report["stats"]["records"] == 0:
        print("error: no probe records found in %s" % args.log,
              file=sys.stderr)
        summaries = file_meta.get("summaries") or []
        if summaries:
            humans_max = max(int(item.get("humans", 0)) for item in summaries)
            print("hint: %d scan window(s) found, max humans in radius=%d; "
                  "if humans=0 the radius was empty, if humans>0 the probe "
                  "may be human-filtered or all combos were throttled"
                  % (len(summaries), humans_max), file=sys.stderr)
        if file_meta["probe_marker_lines"]:
            print("hint: %d 'probe:' line(s) were malformed; check the field "
                  "contract in docs/NPC_PROBE.md"
                  % file_meta["invalid_probe_lines"], file=sys.stderr)
        elif not summaries:
            print("hint: run the client with npc_actor=1, npc_debug=1 on the "
                  "host and let it run for at least ~10 seconds near NPCs "
                  "(see docs/NPC_PROBE.md for the current gating)",
                  file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(_json_safe(report), ensure_ascii=True,
                         indent=2, sort_keys=True))
    else:
        print_report(report)

    if args.strict:
        failed = [verdict["id"] for verdict in report["verdicts"]
                  if verdict["id"] in GATING_VERDICTS
                  and verdict["status"] == "FAIL"]
        if failed:
            print("strict gate failed: %s" % ", ".join(failed),
                  file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    # Windows consoles may default to a legacy code page; probe output is
    # ASCII-only, but keep this guard for redirected/UTF-8 environments.
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    sys.exit(main())
