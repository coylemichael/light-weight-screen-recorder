# LWSR Copilot Instructions

This is a Windows screen recorder written in C (Win32, NVENC, WASAPI).

## Usage Note

When actively referencing these instructions or any linked docs, prefix your response with:
- 📋 = Using copilot-instructions.md
- 📖 = Reading a reference doc
- 🔧 = Following a process doc
- ⚠️ = Applying a rule

This signals which documentation is guiding the work.

## Quick Context

- **Build:** `.\build.bat` from project root
- **Output:** `bin\lwsr.exe`
- **Logs:** `bin\Debug\lwsr_log_*.txt`

## Workflow: Design Before Code

For any non-trivial change (new feature, refactor, architecture change):

1. **Brainstorm first** → Read `docs/process/brainstorm.md`
2. **Discuss the design** with the user before writing code
3. **WAIT for explicit approval** — do NOT write/change code until user approves
4. **Then implement** the approved design

⚠️ **STOP**: You must have explicit user approval before writing any code for complex changes.

❌ Don't: Jump straight into coding a complex feature  
✅ Do: Propose the design, get feedback, iterate, then code only after approval

This prevents wasted effort on wrong approaches and ensures the user understands what's being built.

## When To Read What

| When you need to... | Read this file |
|---------------------|----------------|
| Know which source file to modify | `docs/reference/codebase.md` → File Ownership table |
| Understand what a file does | `docs/reference/file-manifest.md` |
| Follow coding patterns (cleanup, thread-safety) | `docs/rules/coding-rules.md` |
| Fix a bug systematically | `docs/process/debugging.md` |
| Plan a new feature | `docs/process/brainstorm.md` |
| Understand replay buffer internals | `docs/reference/replay-buffer-architecture.md` |

## Document Types

| Type | Purpose | Files |
|------|---------|-------|
| **Reference** | What is it / how does it work | `file-manifest.md`, `replay-buffer-architecture.md`, `codebase.md` |
| **Process** | How to do a task | `debugging.md`, `brainstorm.md`, `review.md` |
| **Rules** | What you must/must not do | `coding-rules.md` |
| **Tracking** | Current state / history | `code-review-tracker.md` |

## File Ownership Quick Reference

| Change | Look In |
|--------|---------|
| UI buttons/drawing | `overlay.c` |
| Settings controls | `settings_dialog.c` |
| Config load/save | `config.c` |
| Replay capture | `replay_buffer.c` |
| Video encoding | `nvenc_encoder.c` |
| Audio capture | `audio_capture.c` |

## Critical Rules

1. **Never start recording while replay buffer is running** - causes deadlock
2. **Check `SettingsDialog_IsVisible()` before handling mode button clicks**
3. **Use `SAFE_RELEASE()` and goto-cleanup pattern for resource management**
4. **Update `SETTINGS_HEIGHT` when adding controls to settings tabs**
