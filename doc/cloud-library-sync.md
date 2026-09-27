# Cloud book library sync

This feature is being developed in `feature/s3-library-sync`. See
[`spec.md`](../spec.md) for the full first-release contract. The current
desktop actions cover local import, opening managed books, and manual source
file and PDF annotation sync. Model settings are not synced yet.

## Project identity and local import

One exact source file is one project. Its ID is the lowercase SHA-256 hash of
its bytes. Importing identical bytes reuses the existing project; different
bytes always create a separate project, even when the filename or title is the
same. Okular must never merge their source text or annotations.

The reader chooses a managed library directory before the first import. Only
an explicit **Add to cloud library** action imports a local file. The import
copies the file into `books/<sha256>/source.<extension>`, writes a local
`manifest.json`, then removes the original file. A failed copy or manifest
write leaves the original in place. Other files that the reader opens remain
outside the library. All remote projects are downloaded into the selected
managed directory during sync.

The UI integration must close an open source document before the final move on
platforms that do not permit removing an open file, then reopen the managed
copy. Existing PDF annotation sidecars and local AI conversations remain
associated because both already use the source bytes' hash.

## Planned remote data

Set `OKULAR_S3_BUCKET` and optionally `OKULAR_S3_PREFIX`. Set `AWS_REGION`,
and use either `AWS_ACCESS_KEY_ID` plus `AWS_SECRET_ACCESS_KEY` (with optional
`AWS_SESSION_TOKEN`) or a static profile selected by `AWS_PROFILE` in the AWS
shared credentials file. `AWS_ENDPOINT_URL_S3` selects a compatible endpoint;
otherwise Okular uses the regional AWS S3 endpoint. This transport currently
supports static shared-credentials profiles, not SSO or role-based credential
resolution. Secrets and Codex login state stay on each device.

With libcurl available at build time, the desktop **File** menu offers **Add
Current Book to Cloud Library**, **Open Cloud Book**, and **Sync Cloud Library
Now**. The first action asks for a managed directory and moves the current
local file into it after closing the document. The sync action runs in the
background and reports source and annotation changes.

`version.json` at the prefix root identifies the library format. Projects
live under `books/<sha256>/` and carry their own metadata. The global file
must not become a mutable list that every device rewrites.

The remote project contains the original file, PDF annotation state, a
selected model profile and a default reading prompt. A shared library catalog
contains public model profile settings for multiple providers. Conversation
history and provider-side sessions are not part of sync. A device missing the
selected model's credentials prompts for local setup instead of silently
switching models.

PDF annotation changes are published as immutable, content-addressed events.
Each event names its annotation ID and the event it follows, so different IDs
merge independently. A consistent SQLite backup is uploaded to an immutable
snapshot key when the sidecar state is synced. Concurrent edits to the same ID
remain in cloud event objects and are listed in the book's local
`annotation-conflicts.json`. Conflict resolution in the GUI is still pending.
The sync action saves current unsaved PDF annotations before uploading. Remote
changes to a PDF currently open in the same Okular window are deferred; close
it and sync again to apply them. Per-book settings use the
same rule at field level: changes to distinct fields merge; conflicting
changes to one field require a choice. The first release does not provide a
book deletion action. It syncs original files for every supported format, with
annotation sync initially limited to PDFs.
