# Cloud book library sync

This feature is being developed in `feature/s3-library-sync`. See
[`spec.md`](../spec.md) for the full first-release contract. The current
desktop actions cover local import, opening managed books, manual sync, and
conflict resolution for PDF annotations and AI settings.
Mobile also supports importing, opening and syncing books, and resolving PDF
annotation conflicts through **Cloud Library…**. See the
[mobile setup](../README.md#mobile-cloud-library). AI settings sync remains a
desktop feature.

## Project identity and local import

One exact source file is one project. Its ID is the lowercase SHA-256 hash of
its bytes. Importing identical bytes reuses the existing project; different
bytes always create a separate project, even when the filename or title is the
same. Okular must never merge their source text or annotations.

On desktop, the reader chooses a managed library directory before the first import. Only
an explicit **Add to cloud library** action imports a local file. The import
copies the file into `books/<sha256>/source.<extension>`, writes a local
`manifest.json`, then removes the original file. A failed copy or manifest
write leaves the original in place. Other files that the reader opens remain
outside the library. All remote projects are downloaded into the selected
managed directory during sync.

Mobile keeps its managed library in the app's private data directory. Import
stages a copy and keeps the original document, including Android provider files.

The desktop UI integration must close an open source document before the final move on
platforms that do not permit removing an open file, then reopen the managed
copy. Existing PDF annotation sidecars and local AI conversations remain
associated because both already use the source bytes' hash.

## Remote data

On desktop, set `OKULAR_S3_BUCKET` and optionally `OKULAR_S3_PREFIX`. Set `AWS_REGION`,
and use either `AWS_ACCESS_KEY_ID` plus `AWS_SECRET_ACCESS_KEY` (with optional
`AWS_SESSION_TOKEN`) or a static profile selected by `AWS_PROFILE` in the AWS
shared credentials file. `AWS_ENDPOINT_URL_S3` selects a compatible endpoint;
otherwise Okular uses the regional AWS S3 endpoint. This transport currently
supports static shared-credentials profiles, not SSO or role-based credential
resolution. Secrets and Codex login state stay on each device.

With libcurl available at build time, the desktop **File** menu offers **Add
Current Book to Cloud Library**, **Open Cloud Book**, **Sync Cloud Library
Now**, and conflict resolution actions. The first action asks for a managed
directory and moves the current local file into it after closing the document.
The sync action runs in the background and reports source, annotation, and AI
setting changes.

`version.json` at the prefix root identifies the library format. Projects
live under `books/<sha256>/` and carry their own metadata. The global file
must not become a mutable list that every device rewrites.

The remote project contains the original file and PDF annotation state.
Immutable events under `settings/events/` contain public model profiles and
each book's selected model and default reading prompt. Each local book keeps
`ai-settings.json` with its
selected profile ID and default prompt. The AI sidebar offers **Book default
prompt** under **Models**; new conversations start with that prompt.
Conversation history and provider-side sessions are not part of sync. A device
missing the selected model's credentials prompts for local setup instead of silently
switching models.

PDF annotation changes are published as immutable, content-addressed events.
Each event names its annotation ID and the event it follows, so different IDs
merge independently. A consistent SQLite backup is uploaded to an immutable
snapshot key when the sidecar state is synced. Concurrent edits to the same ID
remain in cloud event objects and are listed in the book's local
`annotation-conflicts.json`. **Resolve Cloud Annotation Conflicts** lets the
reader choose a version and publishes an event naming every conflicting head
as its parent. If another device adds a version before that choice is
published, Okular rejects the stale selection and asks the reader to sync.
The sync action saves current unsaved PDF annotations before uploading. Remote
changes to a PDF currently open in the same Okular window are deferred; close
it and sync again to apply them. Per-book settings use the same rule at field
level: changes to distinct fields merge; conflicting
changes to one field appear in `model-settings-conflicts.json` and require a
choice through **Resolve Cloud Model Setting Conflicts**. Model profiles sync
as public records by profile ID; API keys in KWallet and Codex login state stay
local. The first release does not provide a book deletion action. It syncs
original files for every supported format, with annotation sync initially
limited to PDFs.

The model endpoint URL cannot contain a username or password. If a shared
profile changes protocol or endpoint, Okular invalidates its old local API key
for that profile and asks for a credential for the new destination.
Extra arguments are shared configuration; put credentials in the API key field.
