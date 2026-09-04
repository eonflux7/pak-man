# Commandos: Strike Force PAK format

Research date: 2026-09-04

## Confidence and sources

The layout below was independently checked against the 30 `.pak` files in
`C:\games\Commandos Strike Force`. It also agrees with the current
[CommDevToolkit format document](https://github.com/IanusInferus/cmdt/blob/fe69effc132f8ef44752c05133736f12c23cbbe2/Src/Doc/CSF_PAK.en.md)
and its
[reader implementation](https://github.com/IanusInferus/cmdt/blob/fe69effc132f8ef44752c05133736f12c23cbbe2/Src/FileSystem/CSF/PAK.vb).
The toolkit is BSD-3-Clause licensed, but the implementation should be written
from this specification and tests rather than copied from the VB.NET source.

The community's CSF PAK Creator independently reports that there are stored and
compressed variants and that the game accepts stored archives. Its public
version only writes the stored variant, which leaves compressed writing as a
useful differentiator for `pak-man`: [CSF PAK Creator discussion](https://forums.revora.net/topic/118162-csf-pak-creator/).

## Installed archive survey

| Property | Observed value |
|---|---:|
| Archives | 30 |
| `PAKA` / `PAKC` | 2 / 28 |
| Archive entries | 29,168 |
| On-disk bytes | 2,827,941,669 |
| Sum of original entry sizes | 4,301,413,863 |
| Header version/platform | `5` / `1` in every archive |
| Compressed blocks checked | 1,052,065 |
| Invalid index or block boundaries | 0 |
| Windows-1252 paths containing non-ASCII bytes | 64 |
| Duplicate or case-colliding path groups | 33 |
| Unsafe absolute/traversal paths | 0 |
| Maximum observed path length | 59 encoded bytes |
| Maximum observed entry size | 71,350,676 bytes |

All 33 duplicate/case-colliding groups contain identical uncompressed data.
Most are `Gfx/Textures/Noise.png` versus `gfx/textures/Noise.png`; a few are
exact duplicate model paths. Windows cannot represent case-only variants as
separate files in a normal extraction directory, so extraction and manifest
handling must account for this explicitly.

The largest file classes by count are `.anm` (12,737), `.dds` (7,863), `.png`
(2,808), `.rpc` (2,532), and `.sp` (1,509). A table-first browser with extension
filtering is therefore more useful than a tree alone.

## Byte layout

All integers are little-endian. Offsets and lengths in the format are 32-bit,
so writers must reject values that do not fit in `uint32_t` even when the host
uses 64-bit file APIs.

### Header

| Offset | Size | Type | Meaning |
|---:|---:|---|---|
| `0x00` | 3 | bytes | ASCII `PAK` |
| `0x03` | 1 | byte | `A` for stored or `C` for compressed |
| `0x04` | 4 | `uint32` | Version; retail is `5` |
| `0x08` | 4 | `uint32` | Platform; PC is `1`, PS2 is `2`, Xbox is `3` |
| `0x0c` | 4 | `uint32` | Entry count |

Version values documented by CommDevToolkit are prototype `3`, demo `4`, and
retail `5`. The v1 writer should intentionally emit only retail PC (`5`, `1`).
The reader may recognize other combinations but should label them unsupported
until fixtures are available.

### Index entry

The header is followed immediately by `entry_count` variable-length records:

| Size | Type | Meaning |
|---:|---|---|
| variable | bytes + `0x00` | Forward-slash path encoded in Windows-1252 |
| 4 | `uint32` | Stored-data-relative offset plus a version/platform correction |
| 4 | `uint32` | Original, uncompressed file length |
| 8 | `FILETIME` | UTC Windows file time (100 ns ticks since 1601-01-01) |

`data_base` is the byte immediately after the final index entry. For retail PC
archives, the physical start of an entry is:

```text
physical_position = data_base + entry.offset - 13
```

The first retail entry therefore normally has offset `13`. CommDevToolkit
documents a correction of `0` for PC demo archives and selected corrections for
console builds; these should not be generalized without samples.

Paths must be retained as original encoded bytes as well as decoded UTF-8 text.
This makes lossless manifest round trips possible and avoids dependence on the
machine's active Windows code page. Observed non-ASCII names use byte `0xF1`
(`ñ`) and `0xD1` (`Ñ`).

### `PAKA` data records

The data record is exactly `entry.length` raw bytes. There is no record header,
padding, alignment, or trailer. The next entry's logical offset is the current
offset plus the current original length.

Both installed stored archives satisfy this equation through the exact end of
file.

### `PAKC` data records

Each file is compressed independently as a sequence of blocks:

```text
repeat until original file length is produced:
    uint32 compressed_length
    uint32 original_block_length
    byte   zlib_stream[compressed_length]
uint32 zero_trailer
```

- The payload is a complete RFC 1950 zlib stream, not bare RFC 1951 deflate.
- Observed original block lengths are at most 4,096 bytes.
- The final block may be shorter.
- The sum of original block lengths must equal the index entry's original
  length exactly.
- The zero trailer must land exactly at the next entry's physical position (or
  EOF for the last entry).
- Compression is used even when it expands a block. In the installed data,
  84,859 blocks have a compressed payload larger than their original block.

Every installed compressed block begins with zlib bytes `58 C3`. In the zlib
header, this corresponds to deflate, an 8 KiB window (`CINFO=5`), and the
maximum compression-level hint. A compatible writer should use the equivalent
of:

```cpp
deflateInit2(stream,
             Z_BEST_COMPRESSION,
             Z_DEFLATED,
             13,                    // 8 KiB window; emits a 0x58 CMF byte
             8,
             Z_DEFAULT_STRATEGY);
```

Each 4 KiB input block gets a fresh zlib stream. Exact compressed bytes may
differ between zlib versions while decompressing to identical data; semantic
round-trip tests should compare entry bytes rather than require byte-identical
`PAKC` output. The zlib framing is standardized by
[RFC 1950](https://www.rfc-editor.org/rfc/rfc1950), and current zlib provides
the needed `deflateInit2`/`inflate` APIs.

## Validation rules

An archive is valid for v1 only when all of the following hold:

1. Magic is `PAKA` or `PAKC`, version is `5`, and platform is `1`.
2. The complete index fits in the file; paths are NUL-terminated and can be
   decoded as Windows-1252.
3. Corrected offsets are monotonic and every physical record lies within the
   archive.
4. For `PAKA`, adjacent offsets/EOF agree exactly with each entry length.
5. For `PAKC`, block headers and payloads remain inside the entry record, every
   zlib stream and checksum validates, each declared raw block is at most 4 KiB,
   raw block totals equal the index length, and the zero trailer ends the record.
6. Extraction paths are relative, contain no empty/`.`/`..` components, drive
   prefix, UNC/root prefix, alternate-data-stream colon, or Windows device name.
7. The normalized output path remains below the chosen extraction root.
8. Offset, file length, entry count, and index-size arithmetic is checked for
   overflow before allocation or seeking.

## Unknowns to keep visible

- The game has not yet been manually smoke-tested with a newly generated
  `PAKC`. Automated structural and semantic tests can establish format
  correctness, but a one-time in-game load test should be a release gate.
- Console/prototype/demo offset and endian behavior is outside v1 because the
  local fixtures are retail PC only.
- The engine's lookup rule for truly different duplicate entries is not proven.
  Preserve order and duplicates in manifests; do not silently invent a winner.
- Timestamps appear to be creation times in the reference implementation, but
  the game likely does not require them. Preserve original ticks from manifests;
  for new folders, use last-write UTC and document the fallback.

