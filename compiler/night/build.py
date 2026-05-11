from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from .check import check_source
from .codegen_c import generate_c


@dataclass(frozen=True, slots=True)
class BuildArtifacts:
    c_path: Path
    binary_path: Path | None


def build_source(
    source_path: Path,
    output_path: Path | None = None,
    emit_c_path: Path | None = None,
    compiler: str | None = None,
) -> BuildArtifacts:
    source = source_path.read_text(encoding="utf-8")
    check_result = check_source(source, source_name=str(source_path))
    c_output = generate_c(check_result)

    c_path = emit_c_path or source_path.with_suffix(".generated.c")
    c_path.write_text(c_output.source, encoding="utf-8")

    binary_path: Path | None = None
    cc = compiler or shutil.which("clang") or shutil.which("gcc")
    if output_path is not None:
        if cc is None:
            raise RuntimeError("no C compiler found (expected clang or gcc)")
        subprocess.run([cc, str(c_path), "-o", str(output_path)], check=True)
        binary_path = output_path

    return BuildArtifacts(c_path=c_path, binary_path=binary_path)
