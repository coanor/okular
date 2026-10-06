# Local bookshelf and reading data

Mobile provides a local bookshelf backed by SQLite, with each book's last page
and reading progress. Opening a document adds it to the bookshelf; tapping its
entry continues from the saved position. Existing recent files and downloaded
books remain accessible after migration.

Mobile and desktop keep AI assistant history and PDF annotations in local
SQLite. Conversations remain separate from annotations, and histories are
isolated by document identity and model profile. Previous JSON conversations
are migrated without overwriting newer SQLite data.

Book storage and reading data require no storage account. S3 integration,
remote book transfers and cross-device sync are removed. Existing local book
files, reading history, annotations and AI conversations are retained.

See [local reading data](doc/local-reading-data.md) for current behavior and
storage locations.

Bookshelf, progress, annotations and AI histories share one `reading-data.sqlite`
database with separate tables. Books use their bytes' SHA-256 as identity;
filenames are display labels and paths are device-local locators. Identical
files opened from different paths or devices reuse the same reading data.
Legacy databases are imported transactionally and retained as backups. Mobile
can export a consistent database snapshot, including JPEG BLOBs for AI images,
for manual transfer to another build supporting this schema.
