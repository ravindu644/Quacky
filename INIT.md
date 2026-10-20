# INIT: start building Quacky

You are starting the implementation of **Quacky**. Until now the repository is a blueprint:
the whole language and interpreter are designed in `docs/developer/`, but no code exists yet.
Your job is to lay the foundation and implement **Phase 1 (the lexer)** from the roadmap, then
delete this file.

Work like the house rules say: climb the "before you write it" ladder, keep the diff small, and
match the blueprint instead of inventing. If something in the docs is ambiguous, ask rather
than guess.

## Read first (in this order)

1. `AGENTS.md` at the repo root. These are hard rules: the framing and banned-words rule, the
   no-em-dashes style, `int64_t`/`size_t` everywhere, `-Werror`, commit prefixes and sign-off.
2. `docs/developer/ROADMAP.md`. You are doing the open items in **Phase 0** and all of
   **Phase 1**. Do not start Phase 2 (the parser) yet.
3. `docs/developer/00-overview.md` for the pipeline and the `src/` file layout.
4. `docs/developer/02-frontend.md`. This is the specification you implement. Sections 1, 2, 3
   (the lexer, incl. 3.1 the `type` raw mode) and 6 (the arena) are Phase 1 and Phase 0.
5. `docs/developer/01-language-spec.md` for the exact tokens: keywords (§2), integers, strings
   and their escapes, comments, operators (§3). Skim `06-errors.md` (§2 message format, §4
   `syntax_error`) and `07-build-and-cross.md` (§2 flags, §3 Makefile).

## Phase 0: scaffolding (finish these first)

- Create `src/` with a header and the file stubs named in `00-overview.md` and the `AGENTS.md`
  layout table. For this phase you only need to write `lex.c`/`lex.h` and the arena; create the
  other `.c` files as empty-ish stubs only if the build needs them, otherwise leave them for
  their phase. Do not scaffold code "for later".
- Write the `Makefile` from `07-build-and-cross.md` §3 (five musl cross targets + `native` with
  ASan+UBSan, bare `make` prints help). `SRC` is a `wildcard`, so new files are picked up.
- Add a `.gitignore` entry for `bin/` (and keep the existing `research/` ignore).
- Implement the **arena** bump allocator from `02-frontend.md` §6 (freed once at exit).
- Implement the shared **diagnostics**: `syntax_error(...)` that prints
  `file:line:col: error: message`, the offending source line, and a caret, then `exit(2)`
  (`06-errors.md` §2, §4). The lexer reports unterminated strings, unclosed `#[`, and bad
  escapes through it.

## Phase 1: the lexer

Implement `lex.c`/`lex.h` exactly as `02-frontend.md` §2 and §3 describe:

- Read the whole file into one NUL-terminated buffer, normalise CRLF to LF, keep the filename.
  Tokens are **slices** into this buffer (`start`, `len`), never copies.
- The `Token` and `TokKind` from §2, with `line`/`col` precomputed as you advance.
- Identifiers and keywords (the full reserved set in `01` §2, including `hold`, `release`,
  `delay`, `repeat`), integers (decimal, `0x`, `0b`, range-checked to `INT64_MAX`), booleans,
  strings to the closing quote on one line (defer escape **decoding** to the parser, but detect
  an unterminated string here), the `#` line comment and the `#[ ... ]#` block comment.
- All operators and punctuation, with greedy two-character tokens (`&&`, `||`, `==`, `!=`,
  `<=`, `>=`, `<<`, `>>`, `..`) and `+`.
- The **`type` raw mode** (§3.1): after emitting `T_TYPE`, consume one space and emit the rest
  of the line as one `T_RAW_LINE`, not tokenising `#`, `"` or `;` inside it.

Note: key names (`ctrl`, `enter`, `down`, ...) are **not** lexer keywords. They lex as plain
`T_IDENT`; the parser resolves them in Phase 2. Do not special-case them here.

## How to verify (Phase 1 has no parser yet)

The lexer cannot be exercised through the real CLI until Phase 7, so add a **throwaway** token
dump to drive it: a tiny `main` (guarded, e.g. behind a `QK_LEX_MAIN` compile flag, or a
temporary `tools/lexdump.c`) that reads a file and prints one line per token
(`kind start:len "text" @line:col`). Build it with `make native` and run it over small `.qk`
snippets that cover each token kind, the two comment forms, a `type` raw line, and the error
cases (unterminated string, unclosed `#[`), checking the error exits 2 with a caret. Keep this
driver clearly marked as temporary; it is replaced by the parser in Phase 2 and the CLI in
Phase 7. Full golden tests come in Phase 9, so do not build the `tests/run.sh` harness now.

## Definition of done

- `make native` compiles clean with the pinned flags (no warnings under `-Werror`), and the
  token dump lexes every token kind, both comments, a `type` raw line, and the error cases
  correctly (errors exit 2 with a caret).
- The `grep` from `AGENTS.md` (banned words over `docs README.md src`) prints nothing, and
  nothing you wrote uses an em-dash.
- Tick the Phase 0 and Phase 1 boxes you completed in `docs/developer/ROADMAP.md`, and add one
  dated line to its status log describing what landed and how you checked it.
- Commit on a branch (not `main`) with a `lex:` / `build:` / `docs:` prefixed, signed-off
  message, one logical commit per concern.
- **Delete this `INIT.md`** as part of the final commit. It has served its purpose.

Then stop and report what works, what you deferred, and anything in the blueprint that needs a
decision before Phase 2.
