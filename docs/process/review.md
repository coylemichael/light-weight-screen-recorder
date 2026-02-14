---
name: systematic-code-review
description: Use when auditing files against project standards, before flagging any issues
---

# Systematic Code Auditing

## What's In This File

| Section | Contents |
|---------|----------|
| [The Iron Law](#the-iron-law) | No issues flagged without citing standard |
| [The Manifest Match Rule](#the-manifest-match-rule) | Verify file does what manifest says |
| [When to Use](#when-to-use) | Review types this applies to |
| [The Four Phases](#the-four-phases) | Read standards → Check manifest → Review code → Report |

---

## Overview

Random issue-flagging wastes time and creates noise. Superficial reviews miss real problems while flagging non-issues.

**Core principle:** ALWAYS verify compliance against documented standards before flagging issues. Opinion-based feedback is failure.

**Violating the letter of this process is violating the spirit of auditing.**

## The Iron Law

```
NO ISSUES FLAGGED WITHOUT STANDARD REFERENCE FIRST
```

If you can't cite the specific standard being violated, you cannot flag it.

## The Manifest Match Rule

```
VERIFY THE FILE DOES EXACTLY WHAT THE MANIFEST SAYS - NOTHING MORE, NOTHING LESS
```

Before reviewing code quality, confirm the file's actual behavior matches FILE_MANIFEST.md:
- Read the manifest entry for this file
- Verify each claimed responsibility exists in the code
- Flag any functionality NOT mentioned in the manifest (scope creep)
- Flag any missing functionality that IS mentioned (incomplete implementation)

## When to Use

Use for ANY code review:
- New file audits
- Change reviews
- Architecture compliance checks
- Pre-merge reviews

**Use this ESPECIALLY when:**
- File looks "messy" (feelings aren't findings)
- You want to suggest a "better way"
- Something feels wrong but you can't articulate why

## The Four Phases

You MUST complete each phase before proceeding to the next.

### Phase 1: Standards Investigation

**BEFORE flagging ANY issue:**

1. **Read Standards Documents Completely**
   - Don't skim CODING_BEST_PRACTICE.md
   - Read FILE_MANIFEST.md for this file's purpose
   - Note specific rules, not general vibes
   - Understand the WHY behind each standard

2. **Verify File Context**
   - What is this file supposed to do per manifest?
   - Does the docstring ALIGN with the manifest? (can be more detailed, see example below)
   - Is the scope appropriate?

   **Good docstring example** (nvenc_encoder.c - manifest says "CUDA-based NVENC HEVC encoding"):
   ```c
   /*
    * NVENC Hardware Encoder - CUDA Path
    * Based directly on OBS nvenc-cuda.c and cuda-helpers.c
    * 
    * Flow:
    * 1. Load nvcuda.dll, get function pointers
    * 2. Create CUDA context (cuCtxCreate)
    * 3. Create CUDA arrays for input surfaces (cuArray3DCreate)
    * 4. Open NVENC session with CUDA device type
    * 5. For each frame: CPU buffer → CUDA array (cuMemcpy2D) → NVENC encode
    */
   ```
   This aligns with the manifest purpose while adding implementation context.

3. **Check Against Checklist**
   - Walk through each audit checklist item
   - Document findings with line numbers
   - If not on checklist → question if it's really an issue

### Phase 2: Evidence Gathering

**Find the evidence before flagging:**

1. **Locate Specific Violations**
   - Line numbers, not "somewhere in the file"
   - Exact code, not paraphrased

2. **Cite the Standard**
   - Which document? Which section?
   - Quote the rule being violated

3. **Verify It's Actually Wrong**
   - Is this a violation or a judgment call?
   - Could there be a valid reason?
   - Check if pattern is used elsewhere in codebase

### Phase 3: Issue Classification

**Categorize before reporting:**

1. **Form Clear Assessment**
   - State clearly: "Line X violates Y because Z"
   - Be specific, not vague
   - One issue per finding

2. **Classify Severity**
   - ⚠️ Violation of documented standard
   - 💡 Suggestion (not a standard, just improvement)
   - Keep these separate - don't mix opinions with violations

3. **Verify Before Reporting**
   - Re-read the standard
   - Re-read the code
   - Are you sure?

### Phase 4: Documentation

**Report findings, not opinions:**

1. **Structure Each Finding**
   - Line number(s)
   - Standard violated (with citation)
   - What's wrong
   - Suggested fix

2. **Summarize Compliance**
   - ✅ What's compliant (acknowledge good work)
   - ⚠️ Issues found (with evidence)
   - 🔧 Suggested fixes (actionable)

## Red Flags - STOP and Follow Process

If you catch yourself thinking:
- "This looks wrong to me"
- "I would have done it differently"
- "This seems inefficient"
- "Best practice says..."  (which best practice? cite it)
- "This could be cleaner"
- "I don't like this pattern"
- "Everyone knows you shouldn't..."
- Flagging issues without line numbers
- Suggesting rewrites without citing violations

**ALL of these mean: STOP. Return to Phase 1.**

## Common Rationalizations

| Excuse | Reality |
|--------|---------|
| "Obviously wrong, don't need citation" | If obvious, citation takes 10 seconds. Do it. |
| "Common knowledge best practice" | Not in our standards doc? Not a violation. |
| "I'm trying to help improve the code" | Undocumented suggestions go in 💡, not ⚠️ |
| "This file is a mess" | Feelings aren't findings. Cite specifics. |
| "I've seen better ways" | Your preference ≠ project standard |
| "This will cause bugs" | Prove it. Show the failure case. |
| "Senior dev said..." | Is it in the standards doc? No? Add it first. |

## Quick Reference

| Phase | Key Activities | Success Criteria |
|-------|---------------|------------------|
| **1. Standards** | Read docs, understand rules | Know what to check |
| **2. Evidence** | Find violations, cite standards | Specific line + specific rule |
| **3. Classification** | Categorize, verify | Clear severity, confirmed issue |
| **4. Documentation** | Report with evidence | Actionable, cited findings |
