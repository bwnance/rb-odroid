#!/usr/bin/env python3
"""Read a rekordbox DeviceSQL export (export.pdb) -- just enough of it to name
a track from its id, and to find a track by title.

WHY THIS EXISTS.  rbp keeps two numbers per deck: a `TrackNo` that is where the
track sat in whatever browse list it was loaded from, and a `MusicID` (24-bit hi
+ 32-bit lo) that is the track's own identity.  The question this answers is
whether that id is the SAME NUMBER as this database's own track id -- which
decides whether a low-level "load track N" can be expressed as a plain call.

THE FORMAT is DeviceSQL, a page-based store Pioneer's crate-digger and
rekordcrate have reverse engineered.  Only the parts needed here are
implemented:

  * the file header -- page size, table count, and the table directory at 0x1c,
    sixteen bytes per entry: {type, empty_candidate, first_page, last_page};
  * a table is a linked list of pages, and traversal MUST stop at last_page --
    the `next_page` link runs on into the next table's pages;
  * each page has a 0x28-byte header, then a heap of rows, and an index that
    grows BACKWARDS from the end of the page in groups of 16;
  * rows of a given type have a fixed struct with 16-bit offsets to their
    variable-length DeviceSQL strings, relative to the row's own start.

A row is only real if the page's presence bitmask says so: deleted rows leave
gaps, and the spec is explicit that their contents are not merely stale but
possibly not well-formed.  Unset bits are skipped, never parsed.

    work/pdb.py <export.pdb>                 # summary + every track
    work/pdb.py <export.pdb> --find RUNNIN  # tracks whose title matches
    work/pdb.py <export.pdb> --id 24        # the track with that id
"""
import struct
import sys

TYPE_TRACKS = 0
HEAP_POS = 0x28              # rows begin here, past the page's own header
PAGE_FLAG_INDEX = 0x40       # bit 6: an index page, not a data page
N_OFS_STRINGS = 21
N_TITLE = 17                 # ofs_strings[N_TITLE] is the title
N_FILE_PATH = 20
N_DATE_ADDED = 10

# Offsets inside a track row (track_row in rekordbox_pdb.ksy).
R_SUBTYPE, R_BITMASK = 0x00, 0x04
R_SAMPLE_RATE, R_ARTWORK = 0x08, 0x1c
R_KEY, R_REMIXER = 0x20, 0x2c
R_BITRATE, R_TRACK_NUMBER, R_TEMPO = 0x30, 0x34, 0x38
R_GENRE, R_ALBUM, R_ARTIST = 0x3c, 0x40, 0x44
R_ID, R_DISC, R_PLAY_COUNT = 0x48, 0x4c, 0x4e
R_YEAR, R_DURATION = 0x50, 0x54
R_COLOR, R_RATING = 0x58, 0x59
R_OFS_STRINGS = 0x5e         # 21 x u2, then the variable-length strings


def ds_string(d, pos, end):
    """One DeviceSQL string: a flag/length byte, then 1- or 2-byte length."""
    if pos <= 0 or pos >= end:
        return ""
    lk = d[pos]
    if lk == 0x40:                                    # long ASCII
        n = struct.unpack_from('<H', d, pos + 1)[0]
        return d[pos + 4:pos + n].decode('ascii', 'replace')
    if lk == 0x90:                                    # long UTF-16LE
        n = struct.unpack_from('<H', d, pos + 1)[0]
        return d[pos + 4:pos + n].decode('utf-16le', 'replace')
    if lk & 1:                                        # short ASCII
        n = lk >> 1                                   # counts the flag byte
        return d[pos + 1:pos + n].decode('ascii', 'replace')
    return ""


class PDB:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.d = f.read()
        d = self.d
        self.len_page, self.num_tables, self.next_unused = \
            struct.unpack_from('<III', d, 4)
        if self.len_page == 0 or len(d) % self.len_page:
            raise SystemExit("not a DeviceSQL export (page size %d)" % self.len_page)
        self.tables = []
        for i in range(self.num_tables):
            t, empty, first, last = struct.unpack_from('<IIII', d, 0x1c + i * 16)
            self.tables.append((t, empty, first, last))

    def table(self, want):
        """Every page index of the table of that type, in link order."""
        for t, _empty, first, last in self.tables:
            if t != want:
                continue
            p = first
            while True:
                yield p
                if p == last:
                    break
                nxt = struct.unpack_from('<I', self.d, p * self.len_page + 0x0c)[0]
                # A malformed or self-referential link must not become a loop.
                if nxt == p or nxt * self.len_page >= len(self.d):
                    break
                p = nxt

    def rows(self, want):
        """Yield (page_index, row_ordinal, offset_of_row) for every present row.

        The ordinal is the row's position in the table's own numbering -- the
        count of index slots before it -- which is what a row index means.
        """
        for pg in self.table(want):
            base = pg * self.len_page
            page = self.d[base:base + self.len_page]
            if page[0x1b] & PAGE_FLAG_INDEX:
                continue
            v = page[0x18] | (page[0x19] << 8) | (page[0x1a] << 16)
            n_ofs = v & 0x1fff                        # offsets ever allocated
            n_rows = v >> 13                         # valid now
            if not n_rows:
                continue
            n_groups = (n_ofs - 1) // 16 + 1 if n_ofs else 0
            for g in range(n_groups):
                gbase = self.len_page - g * 0x24
                present = struct.unpack_from('<H', page, gbase - 4)[0]
                for i in range(16):
                    if not (present >> i) & 1:
                        continue
                    ofs = struct.unpack_from('<H', page, gbase - (6 + 2 * i))[0]
                    yield pg, g * 16 + i, base + HEAP_POS + ofs

    def track(self, row_off):
        """The fields of one track row, or None if it does not look like one."""
        d = self.d
        sub, _ishift = struct.unpack_from('<HH', d, row_off + R_SUBTYPE)
        if sub == 0 or sub == 0xffff:
            return None
        t = {}
        t['id'] = struct.unpack_from('<I', d, row_off + R_ID)[0]
        t['artist_id'] = struct.unpack_from('<I', d, row_off + R_ARTIST)[0]
        t['album_id'] = struct.unpack_from('<I', d, row_off + R_ALBUM)[0]
        t['tempo'] = struct.unpack_from('<I', d, row_off + R_TEMPO)[0] / 100.0
        t['duration'] = struct.unpack_from('<H', d, row_off + R_DURATION)[0]
        t['rating'] = d[row_off + R_RATING]
        end = min(len(d), row_off + 4096)
        ofs = struct.unpack_from('<%dH' % N_OFS_STRINGS, d, row_off + R_OFS_STRINGS)
        t['title'] = ds_string(d, row_off + ofs[N_TITLE], end)
        t['path'] = ds_string(d, row_off + ofs[N_FILE_PATH], end)
        t['added'] = ds_string(d, row_off + ofs[N_DATE_ADDED], end)
        return t


def name_table(pdb):
    """id -> name for artists and albums, so a track can be named.

    The near offset is ONE byte and the far one is TWO, which is the whole
    reason for the subtype flag; reading two bytes where one is meant gives a
    plausible-looking wrong offset and silently empty names.
    """
    out = {}
    for want, id_ofs, near_ofs, far_ofs in ((2, 0x04, 0x09, 0x0a),
                                           (3, 0x0c, 0x15, 0x16)):
        m = {}
        for _pg, _n, off in pdb.rows(want):
            sub = struct.unpack_from('<H', pdb.d, off)[0]
            if sub & 4:
                nofs = struct.unpack_from('<H', pdb.d, off + far_ofs)[0]
            else:
                nofs = pdb.d[off + near_ofs]
            i = struct.unpack_from('<I', pdb.d, off + id_ofs)[0]
            m[i] = ds_string(pdb.d, off + nofs, min(len(pdb.d), off + 4096))
        out[want] = m
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    pdb = PDB(argv[1])
    print("# %s: page=%d tables=%d next_unused=%d" %
          (argv[1], pdb.len_page, pdb.num_tables, pdb.next_unused))
    for t, empty, first, last in pdb.tables:
        print("#   type %2d  pages %d..%d  empty_candidate %d" % (t, first, last, empty))

    names = name_table(pdb)
    print("# artists=%d albums=%d" % (len(names[2]), len(names[3])))

    find = None
    want_id = None
    for i, a in enumerate(argv):
        if a == '--find':
            find = argv[i + 1].upper()
        if a == '--id':
            want_id = int(argv[i + 1])

    n = 0
    for pg, ordinal, off in pdb.rows(TYPE_TRACKS):
        t = pdb.track(off)
        if t is None:
            continue
        n += 1
        if want_id is not None and t['id'] != want_id:
            continue
        if find and find not in t['title'].upper():
            continue
        print("row %-5d page %-4d id %-6d %-34s | %-24s | %6.1f bpm | %s" %
              (ordinal, pg, t['id'], t['title'][:34],
               names[2].get(t['artist_id'], '?')[:24], t['tempo'], t['added']))
    print("# %d tracks" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
