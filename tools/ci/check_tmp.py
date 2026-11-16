"""The temporary directory of one check: its TMPDIR, inside the build tree instead of /tmp.

Python's tempfile, std::filesystem::temp_directory_path() and the compilers all follow TMPDIR, so everything a
check makes there (and whatever a failed, crashed or killed check leaves behind) stays in a directory the runner
owns. The runner empties it before the check and removes it after a pass; a failed check's files stay for
diagnosis until that check runs again. Used by tools/ci/run_host_tests.py and tools/ci/gate.py.
"""
import os
from pathlib import Path
import shutil


def prepare(directory: Path) -> dict:
    """Empties the directory and returns os.environ with TMPDIR set to it."""
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    return dict(os.environ, TMPDIR=str(directory))


def finish(directory: Path, passed: bool) -> None:
    """Removes the directory after a pass; a failed check's directory stays until its next run."""
    if passed and directory.exists():
        shutil.rmtree(directory)
