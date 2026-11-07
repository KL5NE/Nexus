# CI Health Reporting — no paid AI, no secrets

This branch replaces the proposed Claude API repair action with GitHub-native diagnostics and keeps all existing build and safety gates unchanged.

## What it does

- After each listed workflow completes on an owner-controlled push, record each job's status and failed steps.
- Include workflow run links, artifact names, artifact archive digests (when GitHub provides them), and sizes in an Actions job summary.
- On an eligible failure, collect a bounded log excerpt, classify a few **known** failure patterns, and create or update an issue named `CI failure: <workflow> on <branch>`.
- Never execute commands from CI logs or make an automatic code change. The diagnosis is a heuristic; confirm it before acting.
- One open tracking issue per workflow/branch avoids unnecessary duplicate incident issues. A new failed run updates that issue.

## What it does NOT do

- No `ANTHROPIC_API_KEY`, Claude Pro, API subscription, or third-party AI service.
- No autonomous source repair. There are no approved deterministic repair recipes yet; any such recipes must be added as narrowly scoped, independently tested patches.
- No automatic merge, tag, release, main push, HAMBEAST install, clock change, or upstream PR.
- No promise that a passing core test means the Windows installer is ready. The DLL build, packaging, disposable-runner acceptance, and HAMBEAST GUI tests are separate gates.
- No guaranteed push notification for every green result; GitHub watch and notification settings determine what the user sees.
- No unified one-run build → test → package → upload pipeline **yet**. That requires separate, explicitly evaluated refactoring of existing build workflows.
- No test counts or payload-file SHA256 unless the producing workflow reports them. Artifact archive digests, when available, are not equivalent to individual installed DLL checksums.

## Activation and trust boundary

GitHub Actions `workflow_run` workflows execute from the **default branch**. This draft exists only on `ci/autopilot-20261107` and must **not** be merged into `main` without Sean's approval.

If approved, the diagnostic job runs under a narrowly scoped token with `actions: read`, `contents: read`, `issues: write`. It never checks out source from the run it analyzes. It only creates issues for owner-triggered push failures on development branches.

The live demo requested in the original brief (failure → issue → auto-fix PR → green) is **not fulfilled**, because autonomous AI repair was deliberately removed and no safe deterministic fix recipe has yet been implemented.

## What happens when a run fails

1. Open the issue named `CI failure: ...`.
2. Follow the linked Actions run to the exact failed step and log.
3. Review the suggested root-cause pattern and produce a targeted correction on a development branch.
4. Retest and get owner approval; do not bypass security or acceptance checks.

For future efficiency, standardize a single product-ready workflow for each project and have it generate a release manifest with test counts, file checksums, artifact links, and explicit readiness gates. **This is a next milestone, not a claim of current functionality.**
