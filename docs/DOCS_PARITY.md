# Documentation parity map

Docs must stay standardized across English (primary) and Brazilian Portuguese.

Rule:

- what exists in English should exist in Portuguese;
- what exists in Portuguese should exist in English;
- structure, meaning, status, caveats, and decisions should match;
- if a document intentionally exists in one language only, this file must say
  why.

## Public docs

| Topic | English | Portuguese | Status |
|---|---|---|---|
| Usage guide | `docs/en/usage_guide.md` | `docs/pt/usage_guide.md` | Paired |
| Function manual | `docs/en/function_manual.md` | `docs/pt/function_manual.md` | Paired |
| Windows guide | `docs/en/guide_windows.md` | pending | Needs PT counterpart before a broad public release |
| Linux guide | `docs/en/guide_linux.md` | pending | Needs PT counterpart before a broad public release |
| Install (quick) | `docs/INSTALL.md` | covered inside `docs/pt/usage_guide.md` | Acceptable for now |
| Architecture | `docs/ARCHITECTURE.md` | n/a | Internal/developer doc (EN); Appendix C is historical |

## Release notes (EN, historical record)

`docs/RELEASE_NOTES_v0.4.md` … `docs/RELEASE_NOTES_v0.8.md` and
`RELEASE_NOTES_v0.7.1.md` are EN release evidence/notes. PT counterparts are
optional unless a note becomes public onboarding material.

## Project-management records

`docs/ROADMAP.md`, `docs/RESEARCH_AND_IMPROVEMENTS.md`,
`docs/PRE_COMMUNITY_CHECKLIST.md`, and `docs/community/description.yml`
are maintainer/PM records, not user manuals. English-only is acceptable.

## Required review before closing a doc task

1. Check whether the changed topic has both PT and EN surfaces.
2. Update both surfaces when both exist.
3. If only one language is updated, record the reason here or in the report.
4. Keep examples, settings, and status labels aligned across languages.
