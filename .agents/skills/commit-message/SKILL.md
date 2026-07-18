---
name: commit-message
description: Use only after the user explicitly requests an InpxWebReader commit.
---

# Commit messages

Commit only when the user explicitly requests it. Before committing, inspect the staged diff with `git diff --cached` and use:

```text
<Type>: <Imperative summary>

<One concise sentence describing the completed work.>
```

Allowed types: `Feature`, `Fix`, `Refactor`, `Test`, `Docs`, `Build`. The subject is concise English, imperative, has no scope parentheses or trailing period, and describes only staged work. The body is exactly one short English sentence that states what was completed without repeating the subject. If staged work spans incompatible types, propose a split or ask whether one combined commit is intended before committing.
