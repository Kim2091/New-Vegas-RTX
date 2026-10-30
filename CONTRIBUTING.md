# Contributing to New Vegas RTX

Thanks for helping. Small fixes are as welcome as big features, and you don't
need to get everything perfect: we'll help in review.

## What a pull request needs

1. **Licences.** Only add outside code or assets whose licence allows it. Say
   where they came from in the pull request, and add any required credit to
   `THIRD_PARTY_LICENSES.md`.
2. **What you tested.** A sentence is enough, for example "tested in Goodsprings
   and at a terminal, RTX 4070, vanilla, MO2". For visual changes, add a
   screenshot.
3. **One topic per pull request.** A fix and an unrelated cleanup go in separate
   pull requests.

## What helps

- Explain the cause of a bug, not only what you changed.
- Match the surrounding code style. We'll fix small style issues ourselves.
- Keep comments about what the code does. The story behind a change goes in the
  pull request or commit message.
- If you used AI tools, say so. That's fine, but you should understand the change
  you're submitting.
- For anything big (a new feature, a new option, changes to the runtime), open an
  issue first, so we can agree on the approach before you put in the work.

Don't worry about a tidy commit history: pull requests are squashed when merged,
and we'll write the final commit message with you. We'll also point out any
project design decisions in review, with an explanation.

## Reporting a bug

Open an issue, say what happened, and attach the newest zip from
`rtx-remix\logs\archive`. It has the build IDs and logs we need. Mod Organizer 2
users will find it in MO2's Overwrite folder.

## The runtime

The runtime and bridge are in the `runtime/` submodule, on the `fnv/main` branch
of [Kim2091/dxvk-remix](https://github.com/Kim2091/dxvk-remix/tree/fnv/main).
Pull requests for them go to that branch, and the same three points apply.

## Licence

Your contribution is released under this repository's licence (MIT, see
`LICENSE`).
