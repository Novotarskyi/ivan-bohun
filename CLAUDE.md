# Operating principles

How this project is built - by the owner and the AI pair alike. The method is
part of the machine; disclosed here the way the architecture is disclosed in
`docs/`.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

---

## Project laws (earned, each by an incident or an owner's decree)

- **No em-dashes in any writing.** Short dashes only - code comments, commit
  messages, docs, everything.
- **The owner runs all device commands.** Flashing, power cycles, anything
  touching a microcontroller: prepare exact commands, the owner executes,
  interpret the pasted output.
- **No git commit or push without being asked.** Work stays uncommitted; the
  owner makes their own commits.
- **A release must never brick its own delivery path.** Deploys go followers
  first, leader last, through every gate including the 90 s rollback trial.
- **Displays are bench-flashed before the fleet takes any shared-code change** -
  the bench catches what a healthy-looking fleet hides.
- **The bench only shapes which backend is picked.** It never decides whether
  a connection is accepted; a visitor is never refused because of the bench.
- **Kill background servers before reporting done.** Verification must not
  leave processes running.
- **Pin dependencies exactly.** No ranges; bump deliberately.
- **Long scripts echo progress line by line** - never buffer a multi-minute
  run to the end.
- **Every firmware change bumps that variant's `version.txt` in the same
  edit.** The integer is the release identity: baked into the app descriptor,
  gossiped in the heartbeat (v15), shown on every glass. An image whose code
  changed but whose version did not is a lie on the wire. (`idf.py
  reconfigure` before the build - a bare rebuild does not re-read the file.)
- **Secrets never enter the tree or its history.** `secrets.h`, certs and
  build dirs are gitignored; a public push of a secret is a disclosure, not a
  mistake.
