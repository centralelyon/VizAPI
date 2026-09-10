import json
import subprocess


OCTI_BIN = "/usr/local/bin/octi"
OCTI_TIMEOUT_SECONDS = 600


def run_octi(graph):
    try:
        result = subprocess.run(
            [OCTI_BIN],
            input=json.dumps(graph),
            text=True,
            capture_output=True,
            timeout=OCTI_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("OCTI timed out.") from exc

    if result.returncode != 0:
        raise RuntimeError(
            f"OCTI failed: {result.stderr[-3000:]}"
        )

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "OCTI did not return valid JSON."
        ) from exc