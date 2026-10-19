# AGENTS.md

## Git identity firewall

The maintainer has two GitHub identities that must never cross. Each repo belongs to exactly
one of them, and the owner of its `origin` remote decides which. Every commit, push, issue,
PR and doc in this repo uses this repo's identity only.

**Lane: `mercurious` (pseudonymous).** This repo is `mercurious/autojam` (public, sister of
llamatalk, retro-voice-chat, autogamer, ug-rap-machine and fliptribe).

- Commit as `mercurious <dutch.money@gmail.com>`. That is already this repo's `user.email`.
- Keep the maintainer's real name, institution and other accounts out of everything here:
  code, docs, commit messages, issues and PRs. Nothing in this repo may link this identity to
  the other one.
- `gh` usually has the other account active. Before pushing, or if a push fails with
  "Repository not found" or 403, run `gh auth switch --user mercurious`. When you're done,
  switch back to the account that `gh auth status` showed as active before.

On the maintainer's machines, a Claude Code `PreToolUse` hook enforces this:
`~/.claude/hooks/git-identity-firewall.py`, registered on `Bash` in `~/.claude/settings.json`.
It runs before `git commit|merge|rebase|cherry-pick|am|revert|push`, including inside
`ssh HOST '...'`, and before `gh repo create --push` and `gh pr create`. It denies the command
when the identity doesn't belong to the owner's lane. A repo with no remote is allowed.

A second firewall runs inside git itself (`core.hooksPath` = `~/.config/git/identity-firewall/hooks`):
`pre-commit` and `commit-msg` block the other lane's names and addresses in staged changes, paths
and messages; `pre-push` checks every outgoing commit. Never pass `--no-verify` or change
`core.hooksPath`; only the maintainer may bypass a block.

## Working here

- The build runs in a container (`build/build.sh`), or on the etk-cloud node with docker
  (`build/remote.sh`) to spare the laptop. Deploying to a handheld (`flip2/install.sh`) and
  publishing a release are the operator's actions: prepare the command, hand it over.
- The listener has a ground truth: `tools/test_analysis.sh` must pass before a change to
  `src/analysis.cpp` is done.
- `etk-rig` (`SM8250.local`) is the 8GB version; `flip2-12g` is the 12GB visionox version.
