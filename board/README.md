# Board: tasks and notes for humans and agents

One place where everyone who works on this repository, human or AI, can
see what needs doing, who owns it, and what has been said about it. The
board lives in git so that every agent that can read the repository can
read the board, and every change to it is attributed and reversible.

Two files:

| File               | What it is                                   | Who writes it        |
|--------------------|----------------------------------------------|----------------------|
| `board/tasks.json` | the task list, one object per task           | anyone on the roster |
| `board/notes.md`   | the message board, newest entry at the top   | anyone on the roster |

The desktop and web app render these two files in the **Tasks** and
**Notes** tabs (see `docs/PLATFORM_PLAN.md`). Until the app exists, the
files are the board.

## Roster

Every actor has a handle. Use the handle, never a display name, in
`assigned_by`, `assigned_to` and note headers.

| Handle          | Who                                  | Kind  |
|-----------------|--------------------------------------|-------|
| `deandre`       | DeAndre                              | human |
| `deandre-fable` | DeAndre's Claude Fable session       | agent |
| `deandre-gpt6`  | DeAndre's GPT-6 session              | agent |
| `ali`           | Ali                                  | human |
| `ali-fable`     | Ali's Claude Fable session           | agent |

Only handles on this roster may create or assign tasks. Add a row here
before using a new handle.

## Task schema

```json
{
  "id": "T-007",
  "title": "Short imperative sentence",
  "status": "todo",
  "priority": "high",
  "assigned_by": "deandre",
  "assigned_to": "deandre-fable",
  "assigned_on": "2026-09-20",
  "due": "2026-09-27",
  "area": "platform",
  "depends_on": ["T-003"],
  "instructions": "What done looks like, files to touch, constraints.",
  "log": [
    {"on": "2026-09-20", "by": "deandre", "text": "created"}
  ]
}
```

- `id`: `T-` followed by a zero-padded number. Take the next free one.
- `status`: `todo`, `in_progress`, `blocked`, `review`, `done`, `dropped`.
- `priority`: `high`, `normal`, `low`.
- `assigned_to`: a roster handle, or `null` for unclaimed.
- `assigned_on`, `due`: ISO dates (UTC). `due` may be `null`.
- `area`: `bot` (the C++ system), `platform` (API, web, desktop app,
  hosting), `research` (strategy work), `ops` (running it), `board`.
- `depends_on`: task ids that must be `done` first.
- `instructions`: enough for a fresh agent with no other context.
- `log`: append-only; every status change gets a line.

## Protocol for agents

When a human says "check the board", "check your tasks", or gives you a
task id, do this:

1. `git pull` the branch you were told to work on, then read
   `board/tasks.json`. Your tasks are the ones whose `assigned_to` is your
   handle and whose `status` is `todo` or `blocked`. If a task id was
   named, take that one even if it is unassigned.
2. Check `depends_on`. If a dependency is not `done`, set your task to
   `blocked`, add a log line naming the blocker, and stop.
3. Set `status` to `in_progress`, add a log line, commit
   (`board: T-007 in progress (deandre-fable)`), push.
4. Do the work in the repository as usual (CLAUDE.md rules apply). Commit
   the work separately from board edits.
5. When done, set `status` to `review` if a human must look, else `done`;
   add a log line that says what was delivered (commit hashes, paths,
   URLs). Post a note in `board/notes.md` if anyone else needs to know.
6. Never delete a task. `dropped` with a reason is the way out.
7. Never edit another actor's log lines or notes. Append.

Agents may create tasks for other actors when the work is out of their
scope: fill `assigned_by` with your own handle and say why in
`instructions`.

## Notes conventions

`board/notes.md` is one file, newest first. Each note is:

```
## 2026-09-20 14:05 UTC · deandre-fable → all
Text of the note. Reference tasks as T-007 and commits by short hash.
```

Recipient is `all`, a handle, or a comma-separated list of handles. A
note that asks someone to do something should also be a task; the note
points at the task. Keep notes short; long content goes in `docs/`.

## Merging board edits

Edits to the board are small and frequent, so conflicts are easy: keep
both sides, keep every `log` line, and take the newest `status`. Never
resolve a board conflict by discarding the other side.
