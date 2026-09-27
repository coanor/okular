# Cloud book library: first desktop release

## Goal

Keep a personal Okular book library in an S3 bucket or compatible endpoint.
Each device downloads every remote book, uploads its local changes, and can
read offline. A local file enters the library only through an explicit **Add
to cloud library** action.

## Project identity and local storage

- The SHA-256 hash of the exact source bytes is the project ID. Identical
  files attach to one project; changed bytes create independent projects.
  There is no source-version or cross-project merge feature.
- On first use, the reader chooses a managed local library directory. Import
  moves the source file there only after its managed copy and manifest are
  committed. Imported files and remote downloads use
  `books/<sha256>/source.<extension>` in that directory.
- Okular may open files outside the library without uploading them. On a
  device connected to the bucket, sync discovers and downloads all remote
  projects. Startup/open may trigger a background check; **Sync now** runs it
  explicitly. Offline work is queued for a later retry.
- A changed or missing managed source is reported. Okular never silently
  reassigns its annotations or model settings to another hash.

## Remote format and transfer

- `version.json` at the configured prefix contains a format version and
  stable library ID. It is not a mutable catalog of every book.
- Each project lives under `books/<sha256>/`: its original source, immutable
  metadata, and PDF annotation changes or snapshots. A shared model-profile
  catalog and each book's selected profile/default reading prompt are also
  synchronized. Remote object keys never contain raw local paths or secrets.
- The source is uploaded before the project is published. Downloads use a
  temporary file, verify the expected SHA-256, then atomically move into the
  managed library. A truncated or mismatched download does not replace a
  valid local file.
- Listing is paginated. Sync can be retried after interruption without
  duplicating books or losing queued changes. Cloud objects that represent
  changes are immutable or conditionally written; a last-writer-wins mutable
  whole-library index is not used.
- No delete operation is exposed in this release.

## Annotations and settings

- PDF sidecars remain the local annotation store. The original PDF bytes are
  unchanged. Before uploading a sidecar snapshot, Okular makes a consistent
  SQLite backup; it never copies a live database file directly.
- Independent annotation IDs merge automatically, including multiple notes
  on the same text. Concurrent edits of the same ID remain available and
  require a reader choice. Opening a book must not silently discard either
  version.
- The shared model-profile catalog contains provider kind, endpoint, model,
  extra arguments, and vision capability. API keys, Codex login state, and
  S3 credentials remain local. Each book stores its selected profile ID and
  default reading prompt. Distinct setting fields merge; competing edits of
  the same field require a reader choice. Missing local credentials do not
  silently change the selected model.
- AI conversations, Codex session IDs, and provider-side histories do not
  synchronize. A new conversation starts from the book's default prompt and
  may override it locally. AI answers saved as PDF annotations participate
  in normal annotation sync.
- Other Okular-supported source formats sync their original files in this
  release; annotation sync is PDF-only.

## Configuration and security

- `OKULAR_S3_BUCKET` is required; `OKULAR_S3_PREFIX` is optional. Use the
  standard AWS environment/profile settings for region, endpoint, and
  credentials. Configuration and authentication failures are visible in the
  UI. No key or token is written into project metadata or error messages.
- S3 uses HTTPS except when the reader explicitly configures an HTTP endpoint
  for a local/testing service. Storage-side encryption is the initial
  protection for remote data; client-side encryption is outside this release.
- A Codex subprocess does not inherit S3 secrets or unrelated AWS credential
  variables from Okular.

## Acceptance checks

1. Importing a file moves it to the chosen managed directory; importing
   identical bytes associates the existing project; different bytes produce
   another project. An import failure leaves the source intact.
2. With two devices and one bucket, a book imported on A appears on B after
   sync and opens with identical bytes. A disconnected B can read its copy.
3. A newly saved PDF annotation on either device reaches the other. Different
   annotations saved offline on both devices survive; competing edits to one
   annotation are both recoverable and visible for resolution.
4. Model profiles and per-book choice/default prompt reach the second device
   without secrets. A missing credential causes a setup prompt.
5. Interrupted upload/download, missing credentials, wrong bucket, corrupted
   remote source, and unsupported remote format version produce actionable
   errors without overwriting valid local files.
6. UI remains responsive during transfer and shows current, queued, failed,
   and conflicting work. Sync is manually retryable.

## Delivery slices

1. Local project import and managed library. (Started in `c41b0fb18`.)
2. Environment configuration, signed S3 transport, and remote format checks.
3. Full-library discovery plus source upload/download with offline retry.
4. PDF sidecar snapshots, merge and conflict resolution.
5. Model-profile and per-book setting sync, then desktop UI integration.
