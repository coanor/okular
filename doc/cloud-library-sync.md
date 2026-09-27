# Cloud book library sync

This feature is being developed in `feature/s3-library-sync`. The first
implemented slice is local import into a managed library; S3 transfer and UI
are not yet available.

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

The S3 bucket and prefix come from Okular-specific environment variables;
AWS credentials, profile, region and S3 endpoint use the standard AWS
environment variables. Secrets and Codex login state stay on each device.
`version.json` at the prefix root identifies the library format. Projects
live under `books/<sha256>/` and carry their own metadata. The global file
must not become a mutable list that every device rewrites.

The remote project contains the original file, PDF annotation state, a
selected model profile and a default reading prompt. A shared library catalog
contains public model profile settings for multiple providers. Conversation
history and provider-side sessions are not part of sync. A device missing the
selected model's credentials prompts for local setup instead of silently
switching models.

Different annotation IDs merge automatically. Concurrent edits to the same
annotation are retained for the reader to resolve. Per-book settings use the
same rule at field level: changes to distinct fields merge; conflicting
changes to one field require a choice. The first release does not provide a
delete action. It syncs original files for every supported format, with
annotation sync initially limited to PDFs.
