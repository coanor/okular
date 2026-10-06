# Local reading data

Mobile and desktop store bookshelf entries, reading progress, AI conversations
and PDF annotations in one database:

`QStandardPaths::GenericDataLocation/okular/reading-data.sqlite`

On Linux this normally means `~/.local/share/okular/reading-data.sqlite`; Android
uses the application's private data directory. No server or S3 configuration is
required. Model profiles and credentials remain in their existing local settings
and credential stores, outside this database.

## Book identity

`books.hash` is the SHA-256 of the book's bytes. Names are display labels; paths
are local file locations. Identical files share progress, annotations and AI
history, even after renaming or moving them. Changing the bytes produces a new
identity. `book_locations` records a device ID and URL separately from progress.
The device ID lives in `okular/device.ini`, outside the portable database.

The tables are `books`, `reading_progress`, `book_locations`,
`ai_conversations`, `ai_messages`, `annotation_documents`, `annotation_events`
and `annotations`. AI history is additionally separated by model profile ID.
AI screenshots are JPEG BLOBs. The most recent question's image is retained;
earlier text remains. See [AI reading assistant](ai-reading-assistant.md) and
[PDF annotations](annotation-sidecars.md).

## Bookshelf and migration

Open **Bookshelf** from the mobile drawer to continue reading. **Open…** adds a
book and restores its last page. Progress is saved during reading, on document
changes and when the app enters the background. Hashing and history database
work run on worker threads; PDF annotation opening reuses the existing PDF hash.
Desktop local documents also use SQLite progress.

Earlier `reading-history.sqlite`, `ai-history.sqlite` and
`okular/annotations/<hash>.sqlite` files are imported into the shared database.
Original files are retained. Import does not replace newer shared records or
revive cleared AI histories or deleted annotations. Inaccessible URL-based
history stays in `legacy_reading_history` until the file becomes available and
its bytes can be hashed. Earlier recent files and downloaded local books are
also retained. Unknown progress is displayed as **Ready to read**.

## Moving to another device

Choose **Bookshelf → Export reading data…** on mobile. Export produces one
consistent SQLite snapshot, including committed WAL writes and embedded AI
images. It contains reading data, not book files or model credentials.

Close the destination Okular, back up its current database, and place the
exported file at the path above. Use a build containing this storage feature;
ordinary upstream Okular does not read these tables. Open the matching book
file on the destination to restore its data by hash and bind its local path.
An imported bookshelf entry without a path opens the file picker. AI history
also requires the same model profile ID. This is manual database transfer,
not automatic synchronization or merging of two independently edited copies.

For command-line testing, an absolute `OKULAR_READING_DATA_PATH` overrides the
database location. Back up before replacing an existing database.
