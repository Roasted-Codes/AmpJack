# Rules for every worker on the ROADMAP block

You are one of up to five coding workers. An orchestrator assigns you one package from
`docs/ROADMAP.md`. Read, in this order: `CLAUDE.md`, this file, `docs/ROADMAP.md` (Part 3 wins
over Part 2, which wins over Part 1), `docs/INTERNALS.md`, then the code your package names.

## Git
- You are in your own git worktree. First: `git fetch private 2>/dev/null; git checkout -b <your branch> next`
  (if `next` is not a local branch, use `private/next`). Confirm with `git log --oneline -1`.
- Commit to your branch only. **Do not push, do not merge, do not touch `main` or `next`.**
  Never push to `origin` (the upstream project).
- Commit messages: what and why, no history narration. End with the Co-Authored-By line the
  session gives you.
- A sync tool on this Mac sometimes creates duplicates named `something 2.ext`. Never commit
  them; delete any you see in your worktree.

## Scope
- Edit only the files your package **owns** (ROADMAP Part 1 section 3, as amended). If you
  need a change in someone else's file, do not make it: say so in your hand-back.
- The shared contracts (ROADMAP section 2 + Part 2 A + Part 3) are frozen once BASE-1 lands.
  If you find a contract wrong, stop and report; do not work around it.
- **Do not touch the Xbox (10.0.0.13)** unless your brief says you may. The owner is asleep;
  nobody can press the controller's button or listen.
- Never ask the owner anything. If blocked, do the rest and report the block.

## Code rules (ROADMAP Part 2 C.3, in short)
- Kernel rules in `docs/INTERNALS.md`: no floating point, no .bss, thunks for kernel
  callbacks, Usbd only on its DPC. `bash toolchain/build.sh` must print
  `out/xw.xex: image 0x... bytes, ok, no FP` (needs Docker).
- **One Docker build at a time on this Mac.** Always build as
  `lockf -t 3600 /tmp/xw-build.lock bash toolchain/build.sh` (same for any other `docker run`).
  Five workers building at once ran the Mac out of memory and it restarted, losing their work.
  For the same reason: commit to your branch after each finished step, not only at the end.
- Image under 0x100000. Report the image size before and after your change. State over
  about 1 KB is malloc'd at init, not static.
- Release vs `XW_DEV`: the feature, one small status block (at most 24 words) and error /
  state-change logs are release; probes, traces, dummy loads, verbose logs are `#ifdef XW_DEV`.
  Diagnostics are kept for future agents, behind that switch. A field or knob with no reader
  in a tool or acceptance script does not exist.
- Each new feature has its own xbdm gate, **off by default**, its own status block (new
  magic) and its own Python tool module. Do not append to XWDG, XVGA or XVHS.
- Comments say why, and what breaks if it is changed. No dates, run numbers, branch names.
- No duplicated helpers; no new file over about 600 lines; pure DSP in functions without
  globals so it is host-testable.
- Python for offline work: make a venv **outside the repo** (e.g. under your scratch
  directory) for numpy / scipy. Never commit a venv, downloads you may not redistribute, or
  build output.
- Verify off-console as far as possible: host tests in `tests/` (plain `cc`, see the existing
  ones), integer reference models in Python that the C matches bit-exactly.

## Hand-back (your final message; at most 300 words, no code listings)
1. Branch name and last commit hash; `git status` clean or not.
2. What is done, what is not, and why.
3. How you verified it (commands and results, numbers only).
4. Image size before / after, and the budgets from ROADMAP Part 2 C.3 if they apply.
5. Anything you needed in a file you do not own; contract problems; risks for the console.
6. What the owner must look at or listen to, one plain sentence each.
