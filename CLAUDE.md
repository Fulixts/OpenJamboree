# OpenJamboree: project rules

Static recompilation of Super Mario Party Jamboree for Windows, built on SwitchRecomp by MZ Labs.
Public repo: https://github.com/drewkellar/OpenJamboree (remote name `github`). Port notes: `docs/JAMBOREE_PORT.md`.

## Git workflow (required)

- **Never push to `main`.** Not directly, not by force, not by merging a PR. The only exception is when the owner
  explicitly says to push or merge to main for that specific change. Approval for one change doesn't carry over.
- Every change goes on a **feature branch**, never on `main`. Reuse the branch that fits: while a development branch
  is active (its PR open or not yet opened), follow-on work for it goes on that same branch, across as many sessions
  and requests as it takes. Don't start a new branch per request.
- Start a new branch only when no open branch fits the work (all merged, or the work is a separate topic). Base it
  on the latest `main`: `git fetch github && git switch -c <type>/<short-name> github/main`
  (types: `fix/`, `feat/`, `docs/`, `chore/`, `perf/`). Before starting, check `git branch` and `gh pr list` for
  one to reuse.
- **Always push finished work.** When a change is done and verified, commit it, push the feature branch to `github`
  and open a pull request against `main` (`gh pr create --base main`) for the owner's manual review. Report the PR link.
  Don't leave finished work only on the local machine.
- GitHub branch protection on `main`: changes only through a pull request, applies to admins too, no force pushes,
  no deletion, review conversations must be resolved. Don't change these settings unless the owner asks. Even an
  owner-approved change to main goes in through its PR (`gh pr merge` only after the owner says to merge).
- A local `pre-push` hook (`.git/hooks/pre-push`) also rejects pushes to `main`. Don't bypass it, and never set
  `ALLOW_MAIN_PUSH=1` unless the owner explicitly approved that push.
- A branch can carry a broad topic (e.g. a round of playtest fixes). New commits on a branch with an open PR update
  that PR; push them and say so. Once a branch's PR is merged, don't reuse it: start a fresh one off `main`.
- Branches stacked on an unmerged branch are allowed when the work depends on it; say so in the PR, and merge in order.
- Commit messages: what changed and why; end with the co-author line the session asks for.

## What must never be committed (public repo)

`.gitignore` blocks these; still check `git status` and the staged file list before every commit:
- Nintendo data: the dump (`exefs/`, `romfs/`, control files), any Switch container/asset format, keys, firmware.
- Anything made from the game: `games/` (built exe/dll, `*_build/` C code, `*_data/` with dump copy and saves),
  shader caches, captures, screenshots, logs, `threads.txt`, `.hints`, `_work/` symbol caches and dumps.
- Secrets: API tokens, credentials, `.env`, keys/certificates.
- Personal data: the owner's email, Windows user name, local absolute paths. Scripts use paths relative to themselves.
- Game code quoted in docs (function names/addresses as facts are fine; generated C or disassembly is not).

## Build and test

- Rebuild the runtime: `python/python.exe _work/build.py` (about 30 s) → copy `_work/runtime_test.exe` to `games/Recomp.exe`.
- Run and screenshot: `_work/run.ps1 -Seconds N -ShotList "a,b" -Tag t -Keys "t:vk,t:vk+vk@ms"` (uses the runtime's F8
  capture, focuses the window before keys). Path to a board with a save: see `docs/JAMBOREE_PORT.md`.
- Record each bug's evidence, cause and fix in `docs/JAMBOREE_PORT.md` in the same PR as the fix.
