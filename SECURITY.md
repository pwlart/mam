# Security

Report suspected vulnerabilities or exposed credentials privately to **m@pwl.art**.
Include the affected version, a minimal reproduction and observed impact.
Do not put secrets or private audio in public issues.

Fixes target the current 0.1.x preview. There is no response-time guarantee or
maintained older release branch. The CLI and Python companion are local tools,
not a sandbox for hostile jobs; isolate untrusted inputs appropriately.

Run `bash tools/check_secrets.sh` with Gitleaks installed before committing.
It scans locally without uploading source. Ignore rules reduce accidental
staging but do not replace review. Shipped examples require no credentials.

If a credential is published, revoke or rotate it first. Removing a file or
replacing a branch does not invalidate it or remove copies held elsewhere.
