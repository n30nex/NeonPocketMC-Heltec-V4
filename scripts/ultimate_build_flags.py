"""Embed exact Git provenance in Ultimate firmware without changing source files."""

import os
from pathlib import Path
import subprocess

Import("env")

sha = os.environ.get("ULTIMATE_BUILD_SHA")
if not sha:
    sha = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=env.subst("$PROJECT_DIR"), text=True
    ).strip()
if len(sha) != 40 or any(char not in "0123456789abcdefABCDEF" for char in sha):
    raise RuntimeError("ULTIMATE_BUILD_SHA must be an exact 40-character Git SHA")
build_dir = Path(env.subst("$BUILD_DIR"))
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "ultimate_build_sha.h").write_text(
    '#pragma once\n#define ULTIMATE_BUILD_SHA "%s"\n' % sha.lower(),
    encoding="ascii",
)
env.Prepend(CPPPATH=[str(build_dir)])
