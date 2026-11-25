# GitHub Actions checks and runner recovery

[Checks](../.github/workflows/ci.yml) validates the native engine, Python
companion and repository on GitHub-hosted runners. A job that cannot obtain a
runner has not tested the code. On 25 November 2026,
[run 45675318467](https://github.com/pwlart/mam/actions/runs/45675318467)
reported runner-acquisition failures for default and ASan, with macOS ARM64
capacity notices. Python, repository and UBSan jobs succeeded in that run.

## Current execution policy

Automatic push and pull-request runs select **`macos-15` (ARM64)** explicitly.
The native matrix retains default, ASan and UBSan, with `fail-fast: false`:
a failed preset does not cancel its siblings. Only one native matrix job runs
at a time. Alongside the separate Python job, this allows at most **two macOS
jobs per workflow run**. It is not a repository-wide concurrency limit.

The Python job builds and tests its default-derived native configuration plus
the optional companion suite. Repository hygiene, workflow validation and source/
history secret scans run separately on `ubuntu-24.04`. The workflow does not
replace native macOS coverage with Linux rendering claims.

Newer runs retire superseded runs for the same event, branch/PR and runner
selection. Push, pull-request and manual runs have separate concurrency groups;
Intel and ARM manual runs are also separate. Consequently, a branch push with
an open PR can still produce both push and PR checks. Cancellation of a
superseded run is different from a test failure or runner-acquisition failure.

Native jobs have a 20-minute execution timeout, Python 30 minutes and repository
checks 10 minutes. Builds use three parallel tasks. These bounds control work
after allocation; they do not impose or repair GitHub's runner queue timeout.
Pinning an image label reduces unexpected platform changes, but cannot reserve
capacity or freeze every installed tool version.

## Diagnosing and retrying a run

1. Open the run summary and read its annotations. Confirm the commit and runner
   architecture before comparing it with local results.
2. For a runner-acquisition or service error, use **Re-run jobs** and rerun the
   affected jobs when capacity is available. No source change is necessary
   merely because a runner was unavailable.
3. For configure, compile or test failures, inspect the first failing step and
   retained logs. Fix or reproduce the failure; a retry is not evidence that an
   intermittent defect has been resolved.

The workflow does not automatically retry failed builds/tests or silently
replace ARM runs with Intel. Failures continue to fail the job. Shell pipelines
retain failure status when output is copied into log files.

For a deliberate architecture alternative, the manual **Run workflow** control
offers `macos-15` and **`macos-15-intel`**. Select the branch and runner explicitly.
Intel results are additional Intel evidence; they do not establish ARM success.
Automatic ARM jobs retain their existing check names. Intel jobs include
**Intel alternative** in their names, so they cannot silently substitute for
the ARM check names in a branch rule. Review repository rules separately when
selecting which checks must pass.
GitHub requires the dispatch-enabled workflow on the default branch before this
manual control is available. Pushing these changes only to a feature branch
does not establish that the control is available; the workflow does not merge
itself or change the default branch. See GitHub's
[manual workflow instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).

## Retained diagnostics

Native and Python jobs record tool versions, dependency setup, configure/build
output, CTest output and JUnit results. JUnit destinations are absolute because
CTest presets change the working directory. The artifact step runs with
`always()`, has a three-minute limit and retains selected logs for seven days.
Artifact names distinguish the preset, architecture, run and attempt.

Artifacts contain logs and test reports, not complete build trees, environments
or generated music. An unallocated runner cannot execute the upload step or
produce these files. Hard cancellation, timeout or an artifact-service failure
can also prevent upload; `always()` is an attempt, not a delivery guarantee.
The original GitHub step logs and annotations remain the first diagnostic source.

Checkout retains no credentials and workflow permissions remain read-only.
Actions and downloaded validation tools are pinned; archive checksums are
verified before execution. No privileged pull-request trigger or broad write
token is introduced for automatic retries.

See [local testing](TESTING.md) for reproducing a failed preset. GitHub's
[runner reference](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)
describes the current hosted labels and capacity caveats. Changing a CI label
or obtaining a green software check does not change production audio or grant
musical acceptance.
