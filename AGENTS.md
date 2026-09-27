# Agent guidance

This is Okular, a human-maintained KDE document viewer. Keep changes small, readable, and easy for maintainers to review. Follow the surrounding code and avoid unrelated refactoring or formatting.

## Code and verification

- Follow the existing C++ style in `_clang-format` and `README.clang-format`. CI uses `clang-format-19`; format only files you change.
- Follow the project's CMake, Qt 6, and KDE Frameworks 6 conventions. Keep optional document backends optional.
- Preserve compatibility across supported platforms, document formats, and existing user workflows. Treat document opening, rendering, navigation, and annotations as performance-sensitive; avoid unnecessary work on the UI thread.
- Add or update focused tests in `autotests/` for behavior changes. Run the relevant build and tests when dependencies are available, and report what could not be run. See `README.md` for build and test setup.

## Repository workflow

- For GitHub PRs, use `coanor/okular` `master` as the base, not the KDE upstream repository. Keep changes on a topic branch and use squash merge only.
- The fork should automatically delete a PR's head branch after merge. Do not delete another branch manually without checking its use.
- Existing KDE GitLab CI and project contribution instructions remain applicable; do not add GitHub workflows just to duplicate them.

## Communication

- Reply to the repository owner in concise Chinese. Keep technical terms in English when the Chinese translation is awkward or less clear.
- Write all text posted to GitHub in English, including issue titles, descriptions, and comments; pull or merge request titles, descriptions, reviews, and comments; and commit messages. Do not post Chinese or bilingual versions.
