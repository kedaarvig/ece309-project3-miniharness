# Design Log — Project 3

Kedaar Vignesh. P2 code used: my own, copied over unchanged. Tools used:
Claude Code (Anthropic's AI assistant) wrote the first versions of
`ToolRegistry`, `Workspace::resolve`, `calc_evaluate`, the tests and this
log from the spec, and ran the builds and a mutation check (g++ 13,
ASan + UBSan, `-Werror`, WSL2).

## Hash table: strategy, hash, and amortized cost

`ToolRegistry` uses separate chaining: each bucket is a `std::vector` of
entries. A crowded bucket doesn't slow its neighbours the way a
linear-probing cluster does, and the table can never fill up, so there is
no "threshold must stay below 1" rule to get wrong.

The hash is 64-bit FNV-1a, returned at full width. The index is
`hash & (bucket_count - 1)`, valid because the bucket count starts at 8
and only ever doubles. Appendix A's offset basis is a digit short of the
real constant, so I used `0xcbf29ce484222325`. Each entry stores its full
hash, so a resize never calls `Tool::name()` again and `find` compares
names only when the hashes already match.

The load-factor threshold is 3/4, checked *before* inserting: if
`(size + 1) * 4 > buckets * 3`, `grow()` doubles the array and re-inserts
every entry at its new index. 3/4 buys shorter chains for a few extra empty
buckets. `grow()` reserves
every new bucket before moving anything, so a failed allocation can't
leave tools half-moved.

Measured on `tool_0` … `tool_9999`:

```
16384 buckets, load factor 0.610
  FNV-1a (mine)      longest chain 6
  djb2               longest chain 4
  s[0] + s.size()    longest chain 9000
```

The weak hash produces only four distinct values ('t' plus a length of 6
to 9), so all 9,000 nine-character names land in one bucket. FNV-1a's 6
is well under the limit of 64.

**Claim:** over `n` calls to `add`, the total work spent resizing is
`O(n)`, so each `add` is `O(1)` amortized.

**Proof.** Without a resize, an `add` is one hash plus a scan of a chain
that averages under one entry. A resize from `B` to `2B` buckets happens
at exactly `3B/4` entries and moves all of them. Sizes go 8, 16, 32, ...,
so the moved-entry counts form a geometric series: the last resize moves
`0.375·B_final` entries, and all resizes together move under
`0.75·B_final`. Since `n >= 0.375·B_final`, that is under `2n` moves
across `n` adds — a constant per `add`.

## Pointer stability

Buckets hold `Entry { hash, name, std::unique_ptr<Tool> }`. A resize
moves entries from one vector to another, but moving a `unique_ptr`
copies only the address it holds, so the `Tool` object stays on the heap
exactly where `make_unique` put it. `find()` returns `tool.get()` — the
address of the `Tool`, never the address of an `Entry` or a bucket, which
are the things that actually move. `reg_10000_tools` checks it across growth from 8 to
16,384 buckets.

## Calculator trace: `2 * (3 + 4) - 5`

```
token   action                                     values   ops
2       push number                                2
*       ops empty, push                            2        *
(       push                                       2        * (
3       push number                                2 3      * (
+       top is '(' (precedence 0), push            2 3      * ( +
4       push number                                2 3 4    * ( +
)       apply +: pop 4, 3, push 7; pop '('         2 7      *
-       '*' (2) >= '-' (1): apply, pop 7, 2,       14       -
        push 14; then push '-'
5       push number                                14 5     -
end     apply -: pop 5, 14, push 9                 9
```

The `*` waits on the stack through the whole parenthesis because the `(`
above it blocks anything inside from reaching it; it only runs when the
`-` arrives. The same `>=` makes `10 - 4 - 3` come out as 3.

## Errors as values

In an agent loop a tool failing is routine: the file is missing, the path
leaves the workspace, someone divides by zero. The model is meant to read
the message and try something else, so a failure is just more data headed
for the conversation. Returning `ToolResult::failure(...)`, or `false`
plus a message from `resolve` and `calc_evaluate`, keeps it on the normal
path: the dispatcher writes `"ok":false`, appends it, and the loop goes
on. Nothing unwinds past the dispatcher.

Exceptions are for programmer errors, like `as_string()` on a JSON
number, or for running out of memory. Those mean the program is wrong or
can't continue, and failing loudly beats a made-up answer. The dispatcher's catch-all is only a backstop.

## What I would change differently

A vector per bucket is wasteful at scale. An empty `std::vector` is 24
bytes, so 16,384 buckets spend about 390 KB on headers for 10,000 tools,
and each entry copies the tool's name because `Tool::name()` returns by
value. A flat open-addressing array of `unique_ptr<Tool>` slots would cut
both, at the cost of clustering risk. With four tools it makes no difference.
