"""Run the native lifecycle harness with actual redirected stdio and log files."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = sys.argv[1]
    scenarios = (
        "early", "no_stdin", "no_stdout", "disabled", "disabled_queue", "backend", "stdin",
        "flush", "retry_resolve", "retry_hook", "restart",
    )
    failures = []
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix="hlcli-early-") as root:
            game = pathlib.Path(root) / "mod"
            (game / "metahook" / "configs").mkdir(parents=True)
            result = subprocess.run(
                [executable, scenario, str(game)], input="echo stdin_probe\n" if scenario == "stdin" else "",
                capture_output=True, text=True, timeout=10,
            )
            try:
                assert 0 == result.returncode, result.stderr
                output = result.stdout
                count = 3 if scenario in ("flush", "restart") else 2
                assert count == (game / "metahook/configs/halflifecli/errors.log").read_text().count(
                    "fatal 100% value=7\n"
                ), "fatal file output missing or duplicated"
                expected = 0 if scenario in ("disabled", "disabled_queue", "no_stdout") else count
                assert expected == output.count("[halflife-cli] sys_error: fatal 100% value=7\n"), output
                assert "UNLOADED_OUTPUT_MUST_BE_SILENT" not in output, output
                if scenario == "flush":
                    assert 1 == output.count("complete\n"), output
                    assert 1 == output.count("partial\n"), output
                    assert output.index("partial\n") < output.index("[halflife-cli] sys_error:"), output
                print(f"PASS {scenario}")
            except AssertionError as error:
                failures.append(f"{scenario}: {error}")
    if failures:
        raise AssertionError("\n".join(failures))


if __name__ == "__main__":
    main()
