import json
import subprocess


OCTI_BIN = "/usr/local/bin/octi"
LOOM_TIMEOUT_SECONDS = 600


def run_loom(graph):
    try:
        result = subprocess.run(
            [OCTI_BIN],
            input=json.dumps(graph),
            text=True,
            capture_output=True,
            timeout=LOOM_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("LOOM timed out.") from exc

    if result.returncode != 0:
        raise RuntimeError(
            f"LOOM failed: {result.stderr[-3000:]}"
        )

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "LOOM did not return valid JSON."
        ) from exc
