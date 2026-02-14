# Brainstorming Ideas Into Designs

Use before any non-trivial change: new features, refactors, architecture changes.

## Contents

| Section | What's There |
|---------|-------------|
| [The Process](#the-process) | Understand → Explore → Present → Approve |
| [Key Principles](#key-principles) | One question, multiple choice, YAGNI |
| [Example](#example-lwsr) | LWSR-specific good/bad example |

## The Process

### 1. Understand First
- Check current project state (files, docs, recent work)
- Ask questions **one at a time** to refine the idea
- Prefer multiple choice when possible
- Focus on: purpose, constraints, success criteria

### 2. Explore Options
- Propose 2-3 approaches with trade-offs
- Lead with your recommendation and explain why
- Don't jump to implementation

### 3. Present the Design
- Break into small sections (200-300 words each)
- Check after each section: "Does this look right?"
- Cover: architecture, data flow, error handling
- Be ready to revise based on feedback

### 4. Get Approval
- **WAIT for explicit "yes" before writing code**
- If unclear, ask clarifying questions
- Document decisions in chat (no separate design doc needed)

## Key Principles

| Principle | Why |
|-----------|-----|
| One question at a time | Don't overwhelm |
| Multiple choice preferred | Easier to answer |
| YAGNI ruthlessly | Remove unnecessary features |
| Explore alternatives | 2-3 options before settling |
| Incremental validation | Check each section |

## Example (LWSR)

**Bad:** "I'll implement NVENC recording with streaming muxer, GPU converter, and audio support."

**Good:** "For recording, I see three approaches:
1. **Streaming Muxer** - Write frames as they arrive (recommended - simpler)
2. **Batch at end** - Buffer then write (more RAM)
3. **Keep Media Foundation** - Less code change (but different path than replay)

I'd recommend Option 1 because it matches replay_buffer.c architecture. Thoughts?"
