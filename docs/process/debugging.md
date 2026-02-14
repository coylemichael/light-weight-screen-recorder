# Systematic Debugging

Use when encountering any bug, crash, or unexpected behavior.

## The Iron Law

```
NO FIXES WITHOUT ROOT CAUSE INVESTIGATION FIRST
```

If you can't explain WHY it's broken, you can't fix it properly.

## The Four Phases

### Phase 1: Root Cause Investigation

**Before attempting ANY fix:**

1. **Read error messages carefully**
   - Don't skip past errors or warnings
   - Note line numbers, error codes, stack traces
   - Check `bin\Debug\lwsr_log_*.txt` for context

2. **Reproduce consistently**
   - What are the exact steps?
   - Does it happen every time?
   - If intermittent → gather more data, don't guess

3. **Check recent changes**
   - `git diff` - what changed?
   - New dependencies, config changes?

4. **Trace data flow**
   - Where does the bad value originate?
   - Trace backward through call stack
   - Fix at source, not at symptom

### Phase 2: Pattern Analysis

1. **Find working examples**
   - Grep for similar code that works
   - What's different?

2. **Verify assumptions**
   - *"If this assumption is wrong, what breaks?"*
   - Don't assume API behavior - check actual usage

### Phase 3: Hypothesis and Test

1. **Form single hypothesis**
   - "I think X is the root cause because Y"
   - Be specific, not vague

2. **Test minimally**
   - ONE change at a time
   - Don't fix multiple things at once

3. **If it doesn't work**
   - Form NEW hypothesis
   - DON'T pile more fixes on top

### Phase 4: Implementation

1. **Fix the root cause** - not the symptom
2. **Verify** - build, test, check logs
3. **If 3+ fixes failed** - STOP and question the architecture

## Red Flags - STOP and Return to Phase 1

If you catch yourself thinking:
- "Quick fix for now, investigate later"
- "Just try changing X and see"
- "I don't fully understand but this might work"
- "Let me add multiple changes and test"

## Example (LWSR)

**Bug:** NVENC flush loop wasn't writing frames to muxer

**Wrong approach:** "The comment says callback handles it, so maybe the callback isn't registered?"

**Right approach:**
1. Read the code: `while (NVENCEncoder_Flush(..., &flushed)) { /* empty */ }`
2. Grep for working flush usage: `replay_buffer.c` uses the return value
3. Root cause: Flush returns frames synchronously, doesn't trigger callback
4. Fix: `EncoderCallback(&flushed, &g_encoderCtx)` inside loop

## Quick Reference

| Phase | Key Activity | Success Criteria |
|-------|-------------|------------------|
| 1. Investigate | Read errors, reproduce, trace | Know WHAT and WHY |
| 2. Analyze | Find working examples, compare | Identify differences |
| 3. Hypothesis | Single theory, test minimally | Confirmed or new theory |
| 4. Implement | Fix root cause, verify | Bug resolved |
