#!/usr/bin/env python3
"""One-off: remove album_inbox rows whose JPEG file was never written."""
import os
import sqlite3
import sys

db = sys.argv[1] if len(sys.argv) > 1 else "/opt/soti/data/parent.db"
c = sqlite3.connect(db)
c.row_factory = sqlite3.Row
rows = c.execute(
    "SELECT id, file_path FROM album_inbox WHERE acked_at IS NULL"
).fetchall()
n = 0
for r in rows:
    path = (r["file_path"] or "").strip()
    if not path or not os.path.isfile(path):
        c.execute("DELETE FROM album_inbox WHERE id=?", (r["id"],))
        n += 1
c.commit()
print("purged", n)
