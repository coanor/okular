# PDF annotation sidecars

For a local PDF with only local annotation changes, **Save** writes to one
SQLite database per exact PDF SHA-256 hash. The PDF bytes are unchanged. The
database is stored at `QStandardPaths::GenericDataLocation/okular/annotations/<hash>.sqlite`.
An identical copy of the PDF has the same hash and loads the same annotations.
Changing even one byte gives the PDF a different hash and a different database.

The database uses schema version 2:

- `metadata` records `schema_version` and `pdf_sha256`.
- `events` is the ordered replay log. Each row is an `upsert`, `hide`, or
  `delete` for one annotation ID on one page, with type, XML payload when
  applicable, and UTC timestamp. `hide` masks an annotation in the source PDF.
- `annotations` is the current-state query index. It exposes annotation ID,
  page, type, contents, author, color, whether a source annotation is hidden,
  and the complete XML payload when applicable.

The XML payload uses Okular's existing annotation serializer, so it retains
properties that are not represented by individual SQL columns. On open, Okular
replays `events`; it does not trust a copied database whose `pdf_sha256` differs
from the source PDF. A save compares the current local annotations with the
indexed state and writes any resulting operations in one SQLite transaction.

For example, to find highlights with a note on page 3:

```sql
SELECT annotation_id, contents, color
FROM annotations
WHERE page = 2 AND subtype = 4 AND contents <> '';
```

Page numbers in the database are zero-based. The `annotations` table is an
index maintained by Okular; edits should be made through Okular so that the
event stream remains replayable.

This version covers Okular's text and drawing annotation types, including
edits or removals of annotations already embedded in a local PDF. **Save As**
still uses Okular's native PDF save path. If form changes coexist with separate annotations,
**Save** asks for **Save As** so it does not embed the annotations in the source
PDF. A document archive keeps its existing behavior.
