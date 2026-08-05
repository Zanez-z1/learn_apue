#!/usr/bin/env python3
"""Build the project and run CTest before Codex finishes a turn."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


MAX_FEEDBACK_CHARS = 6000
DEVELOPER_DOCUMENTS = {
    "docs/development-status.md",
    "docs/test-plan.md",
}
DEVELOPMENT_PATHS = (
    "CMakeLists.txt",
    "config",
    "include",
    "scripts",
    "src",
    "tests",
)


def emit(payload: dict[str, object]) -> None:
    """Write the single JSON object required by the Stop hook protocol."""
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")


def run(command: list[str], repository: Path) -> tuple[int, str]:
    """Run one verification command and combine its output for diagnostics."""
    completed = subprocess.run(
        command,
        cwd=repository,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return completed.returncode, completed.stdout


def is_development_path(path: str) -> bool:
    """Return whether a path belongs to a functional development increment."""
    return any(path == prefix or path.startswith(prefix + "/")
               for prefix in DEVELOPMENT_PATHS)


def working_tree_paths(repository: Path) -> tuple[int, set[str], str]:
    """Read modified and untracked paths from porcelain output."""
    return_code, output = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], repository
    )
    paths: set[str] = set()
    if return_code != 0:
        return return_code, paths, output
    for line in output.splitlines():
        if len(line) < 4:
            continue
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        paths.add(path)
    return 0, paths, ""


def latest_commit(repository: Path, paths: list[str]) -> tuple[int, str]:
    """Return the newest commit that touched any selected path."""
    return_code, output = run(
        ["git", "log", "-1", "--format=%H", "--", *paths], repository
    )
    return return_code, output.strip()


def verify_development_documents(repository: Path) -> tuple[bool, str]:
    """Require both developer records for uncommitted and committed code changes."""
    return_code, paths, output = working_tree_paths(repository)
    if return_code != 0:
        return False, f"Cannot inspect working tree:\n{output}"
    if any(is_development_path(path) for path in paths):
        missing = sorted(DEVELOPER_DOCUMENTS - paths)
        if missing:
            return False, (
                "Functional changes are not recorded in both developer documents: "
                + ", ".join(missing)
            )

    return_code, source_commit = latest_commit(
        repository, list(DEVELOPMENT_PATHS)
    )
    if return_code != 0:
        return False, "Cannot inspect the latest functional commit."
    if not source_commit:
        return True, ""
    for document in sorted(DEVELOPER_DOCUMENTS):
        return_code, document_commit = latest_commit(repository, [document])
        if return_code != 0 or not document_commit:
            return False, f"Developer document has no Git history: {document}"
        return_code, output = run(
            ["git", "merge-base", "--is-ancestor", source_commit, document_commit],
            repository,
        )
        if return_code != 0:
            return False, (
                f"{document} is older than the latest functional commit "
                f"{source_commit[:7]}."
            )
    return True, ""


def main() -> int:
    try:
        hook_input = json.load(sys.stdin)
    except (json.JSONDecodeError, TypeError) as exc:
        emit({"continue": True, "systemMessage": f"Verification hook input error: {exc}"})
        return 0

    repository = Path(__file__).resolve().parents[2]
    build_directory = repository / "build"
    commands: list[list[str]] = []

    documents_valid, documents_error = verify_development_documents(repository)
    if not documents_valid:
        if hook_input.get("stop_hook_active"):
            emit({"continue": True, "systemMessage": documents_error})
        else:
            emit({"decision": "block", "reason": documents_error})
        return 0

    if not (build_directory / "CMakeCache.txt").is_file():
        commands.append(
            [
                "cmake",
                "-S",
                str(repository),
                "-B",
                str(build_directory),
                "-DCMAKE_BUILD_TYPE=Debug",
            ]
        )
    commands.extend(
        [
            ["cmake", "--build", str(build_directory), "--parallel"],
            ["ctest", "--test-dir", str(build_directory), "--output-on-failure"],
        ]
    )

    for command in commands:
        return_code, output = run(command, repository)
        if return_code == 0:
            continue

        command_text = " ".join(command)
        feedback = (
            f"Project verification failed: {command_text}\n\n"
            f"{output[-MAX_FEEDBACK_CHARS:]}"
        )
        if hook_input.get("stop_hook_active"):
            emit({"continue": True, "systemMessage": feedback})
        else:
            emit({"decision": "block", "reason": feedback})
        return 0

    emit({"continue": True})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
