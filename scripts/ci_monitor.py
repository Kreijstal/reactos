#!/usr/bin/env python3
"""
CI monitor for the codex/rdp-server-side branch.

Polls GitHub Actions for the most recent "Build" workflow run on the
current branch tip, reports overall status, lists per-job conclusions,
and waits until the run reaches a terminal state.

Usage:
    scripts/ci_monitor.py                # poll latest run for current branch
    scripts/ci_monitor.py --sha SHA      # specific commit
    scripts/ci_monitor.py --watch        # block until done (default: --watch)
    scripts/ci_monitor.py --once         # one-shot status print
    scripts/ci_monitor.py --interval 30  # poll seconds (default 60)
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from collections import Counter

REPO = "Kreijstal/reactos"


def gh(path: str) -> dict:
    out = subprocess.run(
        ["gh", "api", path], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        sys.stderr.write(f"gh api {path} failed: {out.stderr.strip()}\n")
        sys.exit(2)
    return json.loads(out.stdout)


def current_branch() -> str:
    return subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def current_sha() -> str:
    return subprocess.run(
        ["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=True
    ).stdout.strip()


def find_run(branch: str, sha: str | None) -> dict | None:
    data = gh(f"/repos/{REPO}/actions/runs?branch={branch}&per_page=20")
    runs = [r for r in data["workflow_runs"] if r["name"] == "Build"]
    if sha:
        runs = [r for r in runs if r["head_sha"].startswith(sha)]
    return runs[0] if runs else None


def fmt_job(job: dict) -> str:
    name = job["name"]
    status = job["status"]
    conclusion = job["conclusion"] or "-"
    return f"  {status:>12} / {conclusion:>9}  {name}"


def print_status(run: dict, jobs: list[dict]) -> None:
    counts = Counter()
    for j in jobs:
        if j["status"] != "completed":
            counts[j["status"]] += 1
        else:
            counts[j["conclusion"] or "unknown"] += 1
    summary = " ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    print(
        f"run {run['id']}  sha {run['head_sha'][:11]}  "
        f"status={run['status']} conclusion={run['conclusion'] or '-'}  "
        f"jobs: {summary}  ({run['html_url']})"
    )


def list_failed(jobs: list[dict]) -> None:
    bad = [j for j in jobs if j["conclusion"] == "failure"]
    if not bad:
        return
    print(f"--- {len(bad)} failed jobs ---")
    for j in bad:
        print(fmt_job(j))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sha", default=None,
                    help="commit prefix to filter (default: current HEAD)")
    ap.add_argument("--branch", default=None,
                    help="branch name (default: current branch)")
    ap.add_argument("--interval", type=int, default=60,
                    help="poll interval in seconds (default 60)")
    ap.add_argument("--once", action="store_true",
                    help="print status once and exit")
    ap.add_argument("--no-wait-sha", action="store_true",
                    help="don't restrict to HEAD sha; track newest Build")
    args = ap.parse_args()

    branch = args.branch or current_branch()
    sha = args.sha
    if not args.no_wait_sha and not sha:
        sha = current_sha()
    print(f"watching branch={branch} sha={(sha or 'newest')[:11]}")

    while True:
        run = find_run(branch, sha[:11] if sha else None)
        if run is None:
            if args.once:
                print("no Build run found for that sha yet")
                return 1
            print(f"no Build run found yet; retrying in {args.interval}s")
            time.sleep(args.interval)
            continue

        jobs_data = gh(f"/repos/{REPO}/actions/runs/{run['id']}/jobs?per_page=100")
        jobs = jobs_data["jobs"]

        print_status(run, jobs)

        if run["status"] == "completed" or args.once:
            list_failed(jobs)
            if run["status"] == "completed":
                if run["conclusion"] == "success":
                    print("CI PASSED")
                    return 0
                else:
                    print("CI FAILED")
                    return 1
            return 0

        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
