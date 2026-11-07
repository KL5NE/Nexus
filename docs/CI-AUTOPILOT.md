# CI Autopilot — staged, owner approval required

This folder documents the draft fail-closed unattended CI workflow. **This is not active until the workflows are merged to the default branch with Sean's approval**. Creating a PR does not activate a workflow_run handler.

The existing build workflows are unchanged; they remain responsible for their own real build/test gates. **This draft does not yet satisfy the requested single end-to-end build → test → package pipeline.**

## Safeguards
- Only failures from owner-pushed workflows on owned branches are eligible for Claude repair. Fork PR, main, protected branches, CI-fix branches and arbitrary actors are excluded.
- No merges, tags, releases, deployment to HAMBEAST, system-clock changes, secret changes or upstream PRs.
- One issue per workflow and branch, with a maximum of two dispatches recorded in the issue body. Failures beyond limit receive the `needs-sean` label.
- Repair requests are dispatched via GitHub repository_dispatch, which GitHub allows to trigger a new workflow when created using GITHUB_TOKEN; the Claude Code action supports repository_dispatch. Never check out code from untrusted fork PRs in this privileged handler.
- No `ANTHROPIC_API_KEY` means no Claude fix attempts. Add the key individually to each repository through Settings → Secrets and variables → Actions → New repository secret. Do not paste it into a chat or commit it.
- A green core-test run is **not** an installable product. Only a successful packaging workflow publishing a verified installer/ZIP can be called ready to download.
- The final green artifact report depends on release-specific packaging workflows; VM/interactive validation remains distinct.

## Verification requirements
Before enabling for production:
1. Review workflow permissions and allowlist. Approve the PR into the default branch only when ready.
2. Add the secret separately to both GitHub repositories.
3. Push a deliberate, non-safety test failure in an owner-only throwaway branch and verify: CI failure → issue → repository_dispatch → Claude fix PR → green PR test → no merge.
4. Verify two-attempt limit, no secret leakage, failure on CI-fix branches, label behavior and no release side effects.

**Status:** draft automation, not a proven autonomous repair loop.