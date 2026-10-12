"""Run the native lifecycle harness with actual redirected stdio.

The scenarios cover console/stdin enablement and the two-phase startup; the
fatal-error path is no longer exercised here because the plugin no longer hooks
Sys_Error (MetaHookSv's `-redirect_fatalerror_to_log` covers fatal errors).
"""
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable = sys.argv[1]
    scenarios = (
        "early", "no_stdin", "no_stdout", "disabled", "disabled_queue", "backend",
        "stdin", "restart",
    )
    failures = []
    for scenario in scenarios:
        with tempfile.TemporaryDirectory(prefix="hlcli-early-") as root:
            game = pathlib.Path(root) / "mod"
            game.mkdir(parents=True)
            result = subprocess.run(
                [executable, scenario, str(game)], input="echo stdin_probe\n" if scenario == "stdin" else "",
                capture_output=True, text=True, timeout=10,
            )
            try:
                assert 0 == result.returncode, result.stderr
                output = result.stdout
                assert "UNLOADED_OUTPUT_MUST_BE_SILENT" not in output, output
                print(f"PASS {scenario}")
            except AssertionError as error:
                failures.append(f"{scenario}: {error}")
    if failures:
        raise AssertionError("\n".join(failures))


if __name__ == "__main__":
    main()
