#!/usr/bin/env python3
"""Unit tests for tests/npc_probe_analyze.py (stdlib only, no pytest needed).

Run directly:
    python tests/test_npc_probe_analyze.py

Checks are explicit (raise AssertionError), so the suite also works under
``python -O`` where bare asserts are stripped.
"""

import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import npc_probe_analyze as npa


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_equal(actual, expected, message):
    if actual != expected:
        raise AssertionError("%s: expected %r, got %r"
                             % (message, expected, actual))


DEFAULT_FIELDS = {
    "model": 0x0D7114C9,
    "metaOutfit": 0x11111111,
    "equipped": 1,
    "mount": 0,
    "weapon": 0,
    "scenarioAny": 0,
    "dynScenario": 0,
    "pointType": 0,
    "pedType": 1,
    "human": 1,
    "mission": 0,
}


def probe_line(prefix="", omit=(), **overrides):
    """Build a synthetic probe line (fields in canonical order).

    ``prefix`` is arbitrary log text placed before the mandatory ``probe:``
    marker (e.g. a timestamp), mirroring real rdr2-coop.log lines.
    """
    values = dict(DEFAULT_FIELDS)
    values.update(overrides)
    parts = []
    for field in npa.CANONICAL_FIELDS:
        if field in omit:
            continue
        raw = values[field]
        if isinstance(raw, str):
            text = raw
        elif field in npa.HEX_FIELDS:
            text = "0x%08X" % (int(raw) & 0xFFFFFFFF)
        else:
            text = str(raw)
        parts.append("%s=%s" % (field, text))
    return "%sprobe: %s" % (prefix, " ".join(parts))


def parse(text):
    record = npa.parse_probe_line(text)
    if record is None:
        raise AssertionError("failed to parse synthetic probe line: %r" % text)
    return record


def records(*lines):
    return [parse(line) for line in lines]


def report_for(records_list, hashdb=None, top=5):
    file_meta = {
        "lines": len(records_list),
        "probe_marker_lines": len(records_list),
        "invalid_probe_lines": 0,
    }
    return npa.analyze_records(records_list, hashdb=hashdb, top=top,
                               file_meta=file_meta)


def verdict_of(report, verdict_id):
    for item in report["verdicts"]:
        if item["id"] == verdict_id:
            return item
    raise AssertionError("verdict %s not found" % verdict_id)


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------

def test_parse_full_line():
    line = probe_line(
        prefix="[12:00:00] ",
        model="0xAbCdEf01",
        metaOutfit="0x7E57C0DE",
        equipped=1,
        mount="0x28f9976a",
        weapon="0x4AAE5FFA",
        scenarioAny=1,
        dynScenario="0xABCDEF01",
        pointType="0x1234",
        pedType=7,
        human=0,
        mission=1,
    )
    record = npa.parse_probe_line(line)
    check(record is not None, "prefixed line should parse")
    check_equal(record["model"], 0xABCDEF01, "model")
    check_equal(record["metaOutfit"], 0x7E57C0DE, "metaOutfit")
    check_equal(record["mount"], 0x28F9976A, "mount")
    check_equal(record["weapon"], 0x4AAE5FFA, "weapon")
    check_equal(record["scenarioAny"], 1, "scenarioAny")
    check_equal(record["dynScenario"], 0xABCDEF01, "dynScenario")
    check_equal(record["pointType"], 0x1234, "pointType")
    check_equal(record["pedType"], 7, "pedType")
    check_equal(record["human"], 0, "human")
    check_equal(record["mission"], 1, "mission")


def test_parse_tolerates_order_spaces_and_bool_like():
    line = ("probe: human=true equipped=false model=0x1 metaOutfit=0x2 "
            "foo=bar mount=0x0 weapon=0x0 scenarioAny=0 dynScenario=0x0 "
            "pointType=0x0 pedType=08 mission=0\r\n")
    record = npa.parse_probe_line(line)
    check(record is not None, "tolerant line should parse")
    check_equal(record["human"], 1, "true -> 1")
    check_equal(record["equipped"], 0, "false -> 0")
    check_equal(record["pedType"], 8, "leading-zero decimal -> 8")
    check_equal(record["model"], 1, "model parsed despite order")


def test_parse_rejects_bad_lines():
    for bad in ("", "nothing here", "probe:", "probe: metaOutfit=0x1",
                "probe: model=0xZZ", "probe: model=0x1 equipped=maybe"):
        check(npa.parse_probe_line(bad) is None,
              "line must be rejected: %r" % bad)
    check(npa.parse_probe_line(None) is None, "non-string must be rejected")


def test_unique_signature_and_duplicates():
    line = probe_line()
    parsed = [parse(line), parse(line), parse(line)]
    report = report_for(parsed)
    check_equal(report["stats"]["records"], 3, "records")
    check_equal(report["stats"]["unique_signatures"], 1, "unique signatures")


def test_missing_fields_are_reported():
    line = probe_line(omit=("equipped", "dynScenario"))
    record = parse(line)
    report = report_for([record])
    missing = report["stats"]["missing_fields"]
    check_equal(missing["equipped"], 1, "missing equipped")
    check_equal(missing["dynScenario"], 1, "missing dynScenario")
    check_equal(missing["model"], 0, "model present")
    check_equal(verdict_of(report, "H2")["status"], "UNKNOWN",
                "H2 must be unknown without equipped data")


# --------------------------------------------------------------------------
# hypotheses H1/H2/H3/H4/H6
# --------------------------------------------------------------------------

def test_h1_pass_partial_fail_insufficient():
    passing = [parse(probe_line(model=0x1000 + i,
                                metaOutfit=0x2000 + i)) for i in range(10)]
    report = report_for(passing)
    check_equal(verdict_of(report, "H1")["status"], "PASS", "H1 all nonzero")

    partial = [parse(probe_line(model=0x3000 + i,
                                metaOutfit=(0x4000 + i if i < 5 else 0)))
               for i in range(10)]
    report = report_for(partial)
    check_equal(verdict_of(report, "H1")["status"], "PARTIAL", "H1 50%")

    failing = [parse(probe_line(model=0x5000 + i, metaOutfit=0))
               for i in range(10)]
    report = report_for(failing)
    check_equal(verdict_of(report, "H1")["status"], "FAIL", "H1 all zero")

    animals = [parse(probe_line(model=0x6000 + i, metaOutfit=0x7000 + i,
                                human=0)) for i in range(3)]
    report = report_for(animals)
    check_equal(verdict_of(report, "H1")["status"], "INSUFFICIENT",
                "H1 no humans")


def test_h2_equipped_consistency():
    good = [parse(probe_line(model=0x1100 + i, equipped=1))
            for i in range(3)]
    report = report_for(good)
    check_equal(verdict_of(report, "H2")["status"], "PASS", "H2 consistent")

    bad = [parse(probe_line(model=0x1200 + i, equipped=0))
           for i in range(3)]
    report = report_for(bad)
    check_equal(verdict_of(report, "H2")["status"], "FAIL", "H2 all false")

    missing = [parse(probe_line(model=0x1300 + i, omit=("equipped",)))
               for i in range(3)]
    report = report_for(missing)
    check_equal(verdict_of(report, "H2")["status"], "UNKNOWN",
                "H2 without equipped")


def test_h3_dynamic_scenario_detected():
    lines = [probe_line(model=0x2000 + i, scenarioAny=1,
                        dynScenario=0xABCDEF00 + i) for i in range(5)]
    report = report_for(records(*lines))
    check_equal(verdict_of(report, "H3")["status"], "PASS", "H3 dynamic")
    scenario = report["stats"]["scenario"]
    check_equal(scenario["any"], 5, "scenarioAny count")
    check_equal(scenario["any_dyn_nonzero"], 5, "dyn count")
    check_equal(scenario["any_detected"], 5, "detected count")


def test_h3_point_type_detected():
    lines = [probe_line(model=0x2100 + i, scenarioAny=1,
                        pointType=0x123400 + i) for i in range(4)]
    report = report_for(records(*lines))
    verdict = verdict_of(report, "H3")
    check_equal(verdict["status"], "PASS", "H3 point type")
    check("point" in verdict["detail"].lower(), "detail should mention point")


def test_h3_partial_and_fail():
    partial_lines = []
    for i in range(10):
        dyn = 0x300000 + i if i < 3 else 0
        partial_lines.append(probe_line(model=0x3100 + i, scenarioAny=1,
                                        dynScenario=dyn))
    report = report_for(records(*partial_lines))
    check_equal(verdict_of(report, "H3")["status"], "PARTIAL",
                "H3 30% coverage")

    failing = [probe_line(model=0x3200 + i, scenarioAny=1)
               for i in range(10)]
    report = report_for(records(*failing))
    check_equal(verdict_of(report, "H3")["status"], "FAIL", "H3 zero coverage")

    no_scenario = [probe_line(model=0x3300 + i) for i in range(3)]
    report = report_for(records(*no_scenario))
    check_equal(verdict_of(report, "H3")["status"], "INSUFFICIENT",
                "H3 no scenario records")


def test_h4_human_animal_ratio():
    mixed = records(
        probe_line(model=0x4001, human=1),
        probe_line(model=0x4002, human=1),
        probe_line(model=0x4003, human=0),
        probe_line(model=0x4004, human=0),
    )
    report = report_for(mixed)
    check_equal(verdict_of(report, "H4")["status"], "PASS", "H4 mixed")

    human_only = records(probe_line(model=0x4101, human=1),
                         probe_line(model=0x4102, human=1))
    report = report_for(human_only)
    check_equal(verdict_of(report, "H4")["status"], "WARN", "H4 no animals")

    no_flag = [parse(probe_line(model=0x4200 + i, omit=("human",)))
               for i in range(2)]
    report = report_for(no_flag)
    check_equal(verdict_of(report, "H4")["status"], "INSUFFICIENT",
                "H4 without human flag")


def test_h6_human_name_mismatch():
    hashdb = {
        0x5001: "A_M_M_TOWNFOLK_01",
        0x5002: "A_F_M_TOWNFOLK_01",
    }
    matching = records(probe_line(model=0x5001, human=1),
                       probe_line(model=0x5002, human=1))
    report = report_for(matching, hashdb=hashdb)
    check_equal(verdict_of(report, "H6")["status"], "PASS",
                "H6 matching names")

    hashdb_animal = {0x5100: "A_C_HORSE_ARABIAN_WHITE"}
    mismatching = records(probe_line(model=0x5100, human=1))
    report = report_for(mismatching, hashdb=hashdb_animal)
    check_equal(verdict_of(report, "H6")["status"], "FAIL",
                "H6 human flag on animal model")

    report = report_for(matching, hashdb=None)
    check_equal(verdict_of(report, "H6")["status"], "UNKNOWN",
                "H6 without hashdb")


def test_model_class_mapping():
    cases = {
        "A_M_M_TOWNFOLK_01": "ambient_male",
        "A_F_M_TOWNFOLK_01": "ambient_female",
        "G_M_M_ODRISCOLL_01": "gang",
        "U_M_M_ASBGUNSMITH_01": "unique",
        "S_M_Y_ARMY_01": "story",
        "CS_ABIGAILROBERTS": "cutscene",
        "MP_MALE": "multiplayer",
        "RE_ESCORT_FEMALES_01": "event",
        "MSP_TRELAWNY1_MALES_01": "mission_actor",
        "A_C_HORSE_ARABIAN_WHITE": "horse",
        "A_C_DEER_01": "animal",
        "PLAYER_ZERO": "player",
        "WESTERN_SADDLE_01": "saddle_prop",
        "": "unnamed",
    }
    for name, expected in cases.items():
        check_equal(npa.model_class(name), expected, "class of %r" % name)


def test_mount_weapon_mission_stats():
    lines = [
        probe_line(model=0x6001, mount=0x28F9976A, weapon=0x4AAE5FFA,
                   mission=1),
        probe_line(model=0x6002),
    ]
    report = report_for(records(*lines))
    stats = report["stats"]
    check_equal(stats["mount"]["nonzero"], 1, "mount count")
    check_equal(stats["weapon"]["nonzero"], 1, "weapon count")
    check_equal(stats["mission"]["nonzero"], 1, "mission count")
    check_equal(stats["mount_top"][0]["value"], 0x28F9976A, "mount top")
    check_equal(stats["weapon_top"][0]["value"], 0x4AAE5FFA, "weapon top")


def test_class_statistics():
    hashdb = {0x7001: "A_M_M_TOWNFOLK_01", 0x7002: "A_C_DEER_01"}
    lines = [
        probe_line(model=0x7001, human=1, metaOutfit=0x1234),
        probe_line(model=0x7002, human=0, metaOutfit=0),
    ]
    report = report_for(records(*lines), hashdb=hashdb)
    classes = report["stats"]["classes"]
    check("ambient_male" in classes, "ambient_male class present")
    check("animal" in classes, "animal class present")
    check_equal(classes["ambient_male"]["outfit_nonzero"], 1,
                "ambient outfit count")
    check_equal(classes["animal"]["outfit_nonzero"], 0, "animal outfit count")


# --------------------------------------------------------------------------
# file reading / CLI
# --------------------------------------------------------------------------

def test_default_hashdb_path_loads_repo_data():
    # The repo ships third_party/rdr2-hashdb/Peds.json; when present the
    # default loader must resolve it relative to this test file.
    if not npa.DEFAULT_HASHDB.exists():
        print("SKIP: default hashdb not present in this checkout")
        return
    hashdb = npa.load_hashdb()
    check(hashdb is not None, "default hashdb should load")
    check_equal(hashdb.get(0x0D7114C9), "PLAYER_ZERO",
                "PLAYER_ZERO name from default hashdb")


def test_main_human_report():
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        missing_hashdb = str(tmp_path / "missing-Peds.json")
        log = tmp_path / "probe.log"
        log.write_text(
            "\n".join([
                probe_line(model=0x1001, metaOutfit=0x2001),
                probe_line(model=0x1002, metaOutfit=0x2002,
                           scenarioAny=1, dynScenario=0xABCDEF01),
                "probe: scan humans=2 logged=2 newCombos=2 unique=2",
            ]) + "\n",
            encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(io.StringIO()):
            code = npa.main([str(log), "--hashdb", missing_hashdb])
        text = output.getvalue()
        check_equal(code, 0, "human report exit code")
        for marker in ("E-1 NPC probe analysis", "Verdicts", "[PASS] H1",
                       "Probe scan windows", "E1 plan hint"):
            check(marker in text, "human report should contain %r" % marker)


def test_parse_summary_line():
    summary = npa.parse_probe_summary_line(
        "probe: scan humans=13 logged=12 newCombos=2 unique=37")
    check(summary is not None, "summary line should parse")
    check_equal(summary["humans"], 13, "humans")
    check_equal(summary["logged"], 12, "logged")
    check_equal(summary["new_combos"], 2, "new_combos")
    check_equal(summary["unique"], 37, "unique")
    check(npa.parse_probe_summary_line("probe: model=0x1") is None,
          "ped line is not a summary")
    check(npa.parse_probe_summary_line("probe: scan broken") is None,
          "summary without humans is rejected")
    check(npa.parse_probe_line(
        "probe: scan humans=13 logged=12 newCombos=2 unique=37") is None,
        "summary must not be parsed as a ped record")


def test_scan_stats():
    records_list = [parse(probe_line(model=0x5500))]
    file_meta = {
        "lines": 4,
        "probe_marker_lines": 3,
        "invalid_probe_lines": 0,
        "probe_summary_lines": 2,
        "summaries": [
            {"humans": 10, "logged": 9, "new_combos": 4, "unique": 4},
            {"humans": 6, "logged": 6, "new_combos": 0, "unique": 4},
        ],
    }
    report = npa.analyze_records(records_list, hashdb=None, top=3,
                                 file_meta=file_meta)
    scan = report["stats"]["scan"]
    check_equal(scan["windows"], 2, "scan windows")
    check_equal(scan["humans_max"], 10, "scan humans max")
    check_equal(scan["logged_max"], 9, "scan logged max")
    check_equal(scan["new_combos_total"], 4, "scan new combos total")


def test_read_probe_file_streaming():
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "rdr2-coop.log"
        log.write_text(
            "coop: some unrelated line\n"
            + probe_line() + "\n"
            + "probe: broken model=0xZZ\n"
            + probe_line(model=0x0D7114C9, metaOutfit=0x11111111) + "\n"
            + "probe: scan humans=2 logged=2 newCombos=2 unique=2\n"
            + "coop: another line\n",
            encoding="utf-8")
        parsed, meta = npa.read_probe_file(log)
        check_equal(meta["lines"], 6, "lines")
        check_equal(meta["probe_marker_lines"], 4, "probe marker lines")
        check_equal(meta["invalid_probe_lines"], 1, "invalid probe lines")
        check_equal(meta["probe_summary_lines"], 1, "probe summary lines")
        check_equal(len(parsed), 2, "parsed records")
        check_equal(len(meta["summaries"]), 1, "parsed summaries")
        check_equal(meta["summaries"][0]["humans"], 2, "summary humans")


def test_main_json_strict_and_no_data():
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        missing_hashdb = str(tmp_path / "missing-Peds.json")

        ok_log = tmp_path / "ok.log"
        ok_log.write_text(
            "\n".join([
                probe_line(model=0x1001, metaOutfit=0x2001),
                probe_line(model=0x1002, metaOutfit=0x2002,
                           scenarioAny=1, dynScenario=0xABCDEF01),
                "coop: unrelated",
            ]) + "\n",
            encoding="utf-8")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(io.StringIO()):
            code = npa.main([str(ok_log), "--json", "--hashdb",
                             missing_hashdb])
        check_equal(code, 0, "main code for valid log")
        payload = json.loads(output.getvalue())
        check_equal(payload["stats"]["records"], 2, "json records")
        check_equal(payload["meta"]["invalid_probe_lines"], 0, "json invalid")
        check("verdicts" in payload, "json verdicts")
        check("H1" in {item["id"] for item in payload["verdicts"]},
              "H1 in json verdicts")

        no_probe = tmp_path / "no-probe.log"
        no_probe.write_text("coop: hello\nnothing here\n", encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            code = npa.main([str(no_probe), "--hashdb", missing_hashdb])
        check_equal(code, 2, "no probe data -> exit 2")

        fail_log = tmp_path / "fail.log"
        fail_log.write_text(
            "\n".join(probe_line(model=0x3000 + i, metaOutfit=0)
                      for i in range(3)) + "\n",
            encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            code = npa.main([str(fail_log), "--strict",
                             "--hashdb", missing_hashdb])
        check_equal(code, 1, "strict H1 fail -> exit 1")

        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            code = npa.main([str(tmp_path / "absent.log")])
        check_equal(code, 2, "unreadable log -> exit 2")


def test_json_safe_handles_nested_sets():
    payload = npa._json_safe({"a": {1, 2}, "b": [3, {4}]})
    check_equal(payload["a"], [1, 2], "set converted to sorted list")
    check_equal(payload["b"], [3, [4]], "nested set converted in list")


def main():
    tests = [
        test_parse_full_line,
        test_parse_tolerates_order_spaces_and_bool_like,
        test_parse_rejects_bad_lines,
        test_unique_signature_and_duplicates,
        test_missing_fields_are_reported,
        test_h1_pass_partial_fail_insufficient,
        test_h2_equipped_consistency,
        test_h3_dynamic_scenario_detected,
        test_h3_point_type_detected,
        test_h3_partial_and_fail,
        test_h4_human_animal_ratio,
        test_h6_human_name_mismatch,
        test_model_class_mapping,
        test_mount_weapon_mission_stats,
        test_class_statistics,
        test_default_hashdb_path_loads_repo_data,
        test_parse_summary_line,
        test_scan_stats,
        test_read_probe_file_streaming,
        test_main_human_report,
        test_main_json_strict_and_no_data,
        test_json_safe_handles_nested_sets,
    ]
    for test in tests:
        test()
        print("OK: %s" % test.__name__)
    print("ALL %d TESTS PASSED" % len(tests))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print("FAIL: %s" % exc)
        raise
