# PDF annotation sidecars

For a local PDF with only annotation changes, **Save** writes to the shared
`QStandardPaths::GenericDataLocation/okular/reading-data.sqlite` database.
The PDF bytes are unchanged. Records are keyed by the exact PDF SHA-256 hash;
an identical copy loads the same annotations, while changed bytes have a new
identity.

The shared schema uses separate annotation tables:

- `annotation_documents` marks books with saved or migrated annotation state.
- `annotation_events` is the ordered replay log, partitioned by `book_hash`.
  Each row is an `upsert`, `hide` or `delete` for one annotation ID on one page,
  with type, XML payload and UTC timestamp. `hide` masks a source annotation.
- `annotations` is the current query index, keyed by book, page and annotation
  ID. It exposes type, contents, author, color, hidden state and full XML.

Legacy per-PDF schema version 2 databases are validated against their hash and
imported transactionally. Original files are retained.

The XML payload uses Okular's existing annotation serializer, so it retains
properties that are not represented by individual SQL columns. On open, Okular
replays only `annotation_events` matching the opened PDF hash. A save compares the current local annotations with the
indexed state and writes any resulting operations in one SQLite transaction.

For example, to find highlights with a note on page 3:

```sql
SELECT annotation_id, contents, color
FROM annotations
WHERE book_hash = '<PDF SHA-256>' AND page = 2 AND subtype = 4 AND contents <> '';
```

Page numbers in the database are zero-based. The `annotations` table is an
index maintained by Okular; edits should be made through Okular so that the
event stream remains replayable.

This version covers Okular's text and drawing annotation types, including
edits or removals of annotations already embedded in a local PDF. **Save As**
still uses Okular's native PDF save path. If form changes coexist with separate annotations,
**Save** asks for **Save As** so it does not embed the annotations in the source
PDF. A document archive keeps its existing behavior.
