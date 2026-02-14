# Systematic Code Review

Use when auditing files against project standards.

## Contents

| Section | What's There |
|---------|-------------|
| [The Iron Law](#the-iron-law) | No issues without citing standard |
| [Manifest Match](#the-manifest-match-rule) | Verify file matches manifest |
| [Phase 1](#phase-1-standards-investigation) | Read standards, verify context |
| [Phase 2](#phase-2-evidence-gathering) | Line numbers, cite rules |
| [Phase 3](#phase-3-issue-classification) | ⚠️ violations vs 💡 suggestions |
| [Phase 4](#phase-4-documentation) | Report with evidence |
| [Red Flags](#red-flags---stop-and-return-to-phase-1) | Opinion-based thinking |
| [Example](#example-lwsr) | Good/bad review findings |

## The Iron Law

```
NO ISSUES FLAGGED WITHOUT CITING THE STANDARD
```

If you can't cite the specific rule being violated, you cannot flag it.

## The Manifest Match Rule

Before reviewing code quality, verify the file matches [file-manifest.md](../reference/file-manifest.md):
- Does the docstring align with the manifest entry?
- Flag functionality NOT mentioned in manifest (scope creep)
- Flag missing functionality that IS mentioned (incomplete)

## The Four Phases

### Phase 1: Standards Investigation

**Before flagging ANY issue:**

1. **Read standards documents**
   - [coding-rules.md](../rules/coding-rules.md) - patterns, YAGNI, cleanup
   - [file-manifest.md](../reference/file-manifest.md) - file purposes

2. **Verify file context**
   - What is this file supposed to do?
   - Does docstring match manifest?

3. **Check against checklist**
   - Walk through audit checklist items
   - Document findings with line numbers

### Phase 2: Evidence Gathering

1. **Locate specific violations**
   - Line numbers, not "somewhere in the file"
   - Exact code, not paraphrased

2. **Cite the standard**
   - Which document? Which section?
   - Quote the rule being violated

3. **Verify it's actually wrong**
   - Is this a violation or judgment call?
   - Check if pattern is used elsewhere

### Phase 3: Issue Classification

1. **Categorize findings**
   - ⚠️ Violation of documented standard
   - 💡 Suggestion (not a standard, just improvement)
   - Keep these separate

2. **Verify before reporting**
   - Re-read the standard
   - Re-read the code
   - Are you sure?

### Phase 4: Documentation

Report with structure:
- ✅ What's compliant
- ⚠️ Issues found (line number + standard citation + fix)
- 💡 Suggestions (optional improvements)

## Red Flags - STOP and Return to Phase 1

If you catch yourself thinking:
- "This looks wrong to me"
- "I would have done it differently"
- "Best practice says..." (which one? cite it)
- "Everyone knows you shouldn't..."

## Example (LWSR)

**Good docstring** (nvenc_encoder.c - manifest says "CUDA-based NVENC HEVC encoding"):
```c
/*
 * NVENC Hardware Encoder - CUDA Path
 * Flow: Load nvcuda.dll → Create CUDA context → NVENC session → encode
 */
```
This aligns with manifest while adding implementation context. ✅

**Bad review finding:** "This function is too long"
**Good review finding:** "Line 245: 150-line function violates coding-rules.md 'Max 100 lines; extract helpers'"

## Quick Reference

| Phase | Key Activity | Success Criteria |
|-------|-------------|------------------|
| 1. Standards | Read docs, understand rules | Know what to check |
| 2. Evidence | Find violations, cite standards | Line + rule |
| 3. Classify | Categorize, verify | ⚠️ vs 💡 |
| 4. Document | Report with evidence | Actionable findings |
