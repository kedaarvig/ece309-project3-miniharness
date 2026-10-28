# Project 3 Design Log
**Author:** Kedaar Vignesh · **P2 code used:** my own, copied over unchanged
**Tools used:** Claude Code (Anthropic's CLI assistant) wrote the first versions of the three pieces, the tests and this log from the spec, and ran the builds and a mutation check. Built on Ubuntu 24.04 (WSL2) with g++ 13, ASan + UBSan, `-Werror`.

## 1. Hash table

I used **separate chaining**, one `std::vector` of entries per bucket. A crowded bucket doesn't slow its neighbours the way a linear-probing cluster does, a miss ends at the end of one short chain, and the table can't fill up, so there's no "threshold must stay below 1" rule to get wrong.

The hash is 64-bit **FNV-1a** at full width. The index is `hash & (bucket_count - 1)`, valid because the count starts at 8 and only doubles. (Appendix A's offset basis is a digit short of the real constant, so I used `0xcbf29ce484222325`.) Each entry keeps its full hash, so a resize never calls `Tool::name()` again and `find` compares names only when hashes match.

The threshold is **3/4**, checked *before* inserting: if `(size + 1) * 4 > buckets * 3`, double the array and re-insert every entry at its new index. Integer math avoids float rounding, and 3/4 buys shorter chains for a few empty buckets. `grow()` reserves every new bucket before moving anything, so a failed allocation can't leave tools half-moved.

Measured on `tool_0` … `tool_9999`, all at 16384 buckets and load factor 0.610:

| Hash | longest chain |
|---|---|
| FNV-1a (mine) | 6 |
| djb2 | 4 |
| `s[0] + s.size()` | 9000 |

The weak hash gives only four distinct values ('t' plus a length of 6 to 9), so all 9,000 nine-character names share one bucket. FNV-1a's 6 is well under 64, and a recount in the test matches `max_probe_length()`.

**Why `add` is amortized O(1).** Without a resize, an add is one hash and a scan of a chain averaging under one entry. A resize from B to 2B buckets happens at exactly 3B/4 entries and moves them all. Sizes go 8, 16, 32, ..., a geometric series: the last resize moves 0.375·B_final entries, all together move under 0.75·B_final, and since n ≥ 0.375·B_final that's under 2n moves over n adds. A constant per add.

## 2. Pointer stability

Buckets hold `Entry { hash, name, std::unique_ptr<Tool> }`. A resize moves entries between vectors, but moving a `unique_ptr` copies only the address, so the `Tool` stays on the heap where `make_unique` put it. `find()` returns `tool.get()`, never the address of an `Entry` or a bucket (those are what move).

## 3. Calculator trace: `2 * (3 + 4) - 5`

| token | action | values | ops |
|---|---|---|---|
| `2` | push number | 2 | |
| `*` | `ops` empty, push | 2 | `*` |
| `(` | push | 2 | `* (` |
| `3` | push number | 2 3 | `* (` |
| `+` | top is `(` (precedence 0), push | 2 3 | `* ( +` |
| `4` | push number | 2 3 4 | `* ( +` |
| `)` | apply `+`: pop 4, 3, push 7; pop `(` | 2 7 | `*` |
| `-` | `*` (2) ≥ `-` (1): apply, pop 7, 2, push 14; push `-` | 14 | `-` |
| `5` | push number | 14 5 | `-` |
| end | apply `-`: pop 5, 14, push 9 | 9 | |

The `*` waits through the whole parenthesis because the `(` above it blocks anything inside from reaching it. The same `≥` makes `10 - 4 - 3` come out as 3: the first `-` is applied the moment the second shows up.

## 4. Errors as values

In an agent loop a tool failing is routine: the file is missing, the path leaves the workspace, someone divides by zero. The model is meant to read the message and try something else, so a failure is just more data headed for the conversation. Returning `ToolResult::failure(...)`, or `false` plus a message from `resolve` and `calc_evaluate`, keeps it on the normal path: the dispatcher writes `"ok":false`, appends it, and the loop goes on. Nothing unwinds past the dispatcher and no tool author can forget a `catch`.

Exceptions are for programmer errors, like `as_string()` on a JSON number, or running out of memory. Those mean the program is wrong or can't continue, and failing loudly beats handing the model a made-up answer. The dispatcher's catch-all is a backstop for a buggy tool, not the usual route.

## 5. What I would design differently

A vector per bucket is wasteful at scale. An empty `std::vector` is 24 bytes, so 16,384 buckets spend about 390 KB on headers for 10,000 tools, and each entry copies the tool's name because `Tool::name()` returns by value. A flat open-addressing array of `unique_ptr<Tool>` slots would cut both, at the cost of clustering risk. I'd measure first. With four tools it makes no difference.
