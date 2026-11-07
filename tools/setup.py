#!/usr/bin/env python3
"""One-time (and safe to rerun) setup of an FW-MAME checkout.

1. Applies patches/wilifpga-cm0-link.patch to the wilifpga submodule, unless
   it is already applied. The patch lets a design with `[host] cm0_link = true`
   in its design.toml use the CM0 nets (see the patch for details).
2. Links every fpga/<design>/ folder into wilifpga/designs/<design>, because
   the wilifpga tools only look there, and hides those links from the
   submodule's git status.

    python tools/setup.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WILIFPGA = ROOT / "wilifpga"
PATCH = ROOT / "patches" / "wilifpga-cm0-link.patch"


def git(*args, check=True):
    return subprocess.run(["git", "-C", str(WILIFPGA), *args], capture_output=True, text=True, check=check)


def apply_patch():
    if git("apply", "--reverse", "--check", str(PATCH), check=False).returncode == 0:
        print("wilifpga patch: already applied")
        return
    r = git("apply", "--check", str(PATCH), check=False)
    if r.returncode != 0:
        sys.exit(f"wilifpga patch does not apply cleanly:\n{r.stderr}")
    git("apply", str(PATCH))
    print("wilifpga patch: applied")


def link(src, dst):
    if dst.exists() or dst.is_symlink():
        print(f"link: {dst.relative_to(ROOT)} exists")
        return
    if os.name == "nt":   # a junction needs no admin rights
        subprocess.run(["cmd", "/c", "mklink", "/J", str(dst), str(src)], check=True, capture_output=True)
    else:
        dst.symlink_to(src, target_is_directory=True)
    print(f"link: {dst.relative_to(ROOT)} -> {src.relative_to(ROOT)}")


def exclude(names):
    gitdir = Path(git("rev-parse", "--git-dir").stdout.strip())
    if not gitdir.is_absolute():
        gitdir = WILIFPGA / gitdir
    path = gitdir / "info" / "exclude"
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = path.read_text().splitlines() if path.exists() else []
    new = [f"/designs/{n}" for n in names if f"/designs/{n}" not in lines]
    if new:
        path.write_text("\n".join(lines + new) + "\n")


def main():
    if not (WILIFPGA / "tools" / "fwfpga.py").exists():
        sys.exit("wilifpga submodule is missing: run `git submodule update --init` first")
    apply_patch()
    designs = sorted(p for p in (ROOT / "fpga").iterdir() if (p / "design.toml").exists())
    for d in designs:
        link(d, WILIFPGA / "designs" / d.name)
    exclude([d.name for d in designs])
    print("done. Build from wilifpga/:  tools/fwfpga build <design>")


if __name__ == "__main__":
    main()
