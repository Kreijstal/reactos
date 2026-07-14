# Bug 7 — gcc/ld "could not read symbols: invalid operation" (NTFS unaligned-read bounce-window bug)

**Status:** FIXED + VERIFIED (2026-07-14, 58 rounds).  Root cause: `NtfsReadFile`'s
sector-aligned bounce window did not cover the requested range (see Round 57).
Fix committed as `4885d738b10` ([NTFS] Cover the whole request in NtfsReadFile's
aligned bounce window); instrumentation removed in `fa3c2aedd9f`.  Verified twice
on target: instrumented fix build `OK=25 FAIL=0`, then the exact committed clean
driver (md5 `94383d489034ea9c584af70345ffb8e6`) again `OK=25 FAIL=0` — previously
`OK=0 FAIL=25` in every round.  Leftover (separate, NOT bug7): the
`Fcb->CachedFileRecord` lifetime hole from round 53/56 remains unfixed.

The remainder of this file is a chronological investigation record.  Any
"current plan", "next step", or tentative conclusion inside an older round is
historical unless the status above explicitly carries it forward.

## Round 57 — ROOT CAUSE: NtfsReadFile's sector-aligned bounce window doesn't cover the request

**The bug (drivers/filesystems/ntfs/rw.c, unaligned-read path in `NtfsReadFile`):**

```c
RealReadOffset = ROUND_DOWN(ReadOffset, BytesPerSector);   /* 0x194 -> 0x000 */
RealLength     = ROUND_UP(ToRead, BytesPerSector);         /* 0x14c -> 0x200  <-- ignores the
                                                              intra-sector offset misalignment */
if (RealLength + RealReadOffset < ReadOffset + Length)     /* shortfall detected, but... */
    if (RealReadOffset + RealLength + BytesPerSector <= AttributeAllocatedLength(pRecord))
        RealLength += BytesPerSector;                      /* ...the one-sector patch-up is DENIED
                                                              for resident attributes: allocated
                                                              length = ValueLength aligned to 8
                                                              (0x2e0) < window end 0x400 */
```

The copy-out then slices `ReadBuffer[ReadOffset-RealReadOffset ..][ToRead]` =
`bounce[0x194..0x2e0)` out of a pool buffer only *filled* to `0x200`: the tail
(here 0xE0 bytes) is **never-written NonPagedPool garbage**.  Trigger condition:
`(ReadOffset % 512) + ToRead > ROUND_UP(ToRead, 512)` **and** the one-sector
extension denied — i.e. resident attributes (small fresh files), or reads whose
window would exceed the allocated length.  For non-resident files the +1-sector
hack always sufficed (shortfall is provably ≤ 1 sector), which is why only tiny
resident `cc*.o` files died.

**How round 57 caught it** (file-keyed staged tracing, gcc/binutils 16.1.0, 25×,
`OK=0 FAIL=25`): the new toolchain's `cc*.o` is 0x2e0 bytes → resident; ld reads
it 13× per link, all `irp=900` non-paging reads by `ld.exe`.  Every staged hash
(`R1REC` file record, `R2CTX` context, `R3RES` resident copy, `R4EXIT` slice,
`R5COPY` post-bounce) was **identical across all 25 files at every offset except
`off=0x194 len=0x14c`** — the symtab+strtab read — where `R4EXIT` had 25 distinct
hashes.  Its chain shows the smoking gun directly: `R3RES roff=0 len=200` (bounce
filled to 0x200 only) but `R4EXIT off=194 len=14c got=200` (slice reaches 0x2e0).
The 17 `BUG7_NTFSREAD_TAG` hits (`RMAP`/`NtfC`/`MMSS` pool tags at `hit=0x70` of
that read) are recycled pool contents in the unwritten tail.  ld's own behavior
confirms delivered-correct earlier reads: it read `(0x2c6,4)` then `(0x2c6,0x1a)`
— it can only know the string-table size 0x1a by parsing those 4 bytes correctly
(that read's window `[0x200,0x2e0)` was computed right); the *final* one-shot
symtab+strtab read at `0x194` is the only one that crosses a sector boundary with
`ROUND_UP(len) < off%512 + len`, and bfd parses the strtab size from *that*
buffer → "bad string table size <pool garbage>".

**Why every discriminator fits:** disk bytes always correct (in-memory window
math, nothing wrong on disk); fresh-file-only (must be resident = small); NTFS-only
(fastfat/Cc path has correct math); toolchain-confined (bfd's unaligned
seek-to-symtab read shape); 100% deterministic single-CPU (no race at all — the
53-round "UAF/race" framing was wrong); garbage varies per run = whatever pool
block gets recycled (RMAP/MmSt/Ntf tags, zeros, `MARE`, `0x230A00FF`); round 51's
"garbage already present in the aligned destination before the bounce copy" was
exactly this (unwritten bounce tail), misread as a stale-source clue.

**Also exonerated on the way:** round-56 A/B rejected `CachedFileRecord` UAF
(still a real locking hole, separate fix); round-57 alias probes showed 0
MDL-vs-PTE mismatches and stable `R1REC` record hashes (no MM page theft in this
path); writes never traverse `NtfsWriteFile` for these files at all (they go
`CcCopyWrite` + flush — why `BUG7_WR` stayed silent).

## Round 58 — the fix

`NtfsReadFile`: `RealLength = ROUND_UP(ReadOffset + ToRead, BytesPerSector) -
RealReadOffset` (window covers the full request; safe: `ToRead` is already
clipped to `StreamSize`, so the window end ≤ `ROUND_UP(StreamSize)` ≤ allocated
for both resident and non-resident); the one-sector patch-up hack is deleted; an
`ASSERT(RealLengthRead >= (ReadOffset - RealReadOffset) + ToRead)` guards the
copy-out choke point.  Deployed as md5 `7bd843db13c54f36008997ed24d40c23`;
the instrumented build passed the same 25× repro (`OK=25 FAIL=0`).  After
committing the fix and removing all BUG7 instrumentation, the exact clean
driver (md5 `94383d489034ea9c584af70345ffb8e6`) passed another independent
25× run (`OK=25 FAIL=0`).

## Round 56 — CachedFileRecord A/B REJECTED; probes are blind to the new toolchain's reads

**A/B (step 1 of the round-56 plan): NEGATIVE.**  `NTFS_BUG7_DISABLE_RECORD_CACHE`
made every `NtfsReadFile` fetch a private MFT record copy (never consuming or
installing `Fcb->CachedFileRecord`), so no reader could dereference a record freed
by `NtfsInvalidateCachedFileRecord` mid-request.  Built, deployed guest-side
(rename-swap, md5-verified pre- and post-reboot), and run: **`RESULT OK=0 FAIL=25`**
with the genuine symptom set (`bad string table size 0/65535/4261281277`,
`(NULL)`/garbage symbols incl. a `!\xMRGN` storage-class error, `could not read
symbols: invalid operation`).  The FCB cached-file-record lifetime hole is a real
locking bug (readers use it under `MainResource` shared; `NtfsWriteFile` frees it at
entry; paging writes never take `MainResource`) but it is **not bug7's producer**.

**Second finding — the committed BUG7 probes never fired (vacuousness alert):**
across all 25 failing links, serial had **zero** `BUG7_NTFSREAD_LIVE`, `BUG7_RDH`,
or `BUG7_STAGE` lines, although all are compiled in (verified via `strings` on the
deployed ntfs.sys).  The repaired guest now runs a **new MSYS2 with gcc/binutils
16.1.0** (temp dir `C:\Documents and Settings\Administrator.001\Local Settings\
Temp`), and the probes filter on non-paging reads by `ld*` processes: this
toolchain's linker evidently does not traverse that path (likely maps the object →
paging I/O, or reads under a different process shape).  Consequences:
- The bug reproduces **identically under a different gcc/binutils version** and
  (probably) a different read mechanism — further evidence the producer is in the
  kernel/NTFS side, not toolchain-version-specific behavior.
- All old offset-keyed (`off=0x194 len=0x14c`) and process-keyed probe results do
  not transfer to the current environment; round-57 instrumentation must key on the
  FILE (`cc*.o`), include paging I/O, and log Irp flags + requestor.

**Round-56 harness gotchas (cost ~1h):** luagent argv reconstruction breaks nested
quotes AND `2>nul` redirects (agent-side parse: "Can't redirect to file /dev/null");
direct luagent spawn of msys binaries EOFs the session (wrap in `cmd /c`); ReactOS
`cmd` `ren` mishandles full paths (garbled source, MoveFile error 2) — `cd /d` +
in-dir `ren` works; `start /min bash.exe script.sh` works for detaching; a repro
script must keep `/c/msys64/usr/bin` in PATH (else `seq`/`nohup` vanish and the
loop silently runs 0 iterations — always log per-iteration progress).

## Completed round-57 plan: file-keyed staged tracing

This plan found the short bounce window described above.  It is retained to
show how the first divergent stage was isolated; it is not pending work.

1. Restore the record cache (A/B off).  Add `Bug7WatchedFcb()` matching any
   `cc*.o` file, regardless of process, paging and non-paging alike.
2. Read side: log Irp flags + process + (off,len) for every watched read;
   staged hashes — file record at fetch, attribute context after `FindAttribute`
   (resident flag, resident value hash / decoded runs), every disk transfer
   inside `ReadAttribute` (VCN→LCN, length, post-read hash), request slice at
   `NtfsReadFile` exit, post-bounce-copy slice.
3. Write side: same staging for `WriteAttribute` on watched files (VCN→LCN +
   payload hash) so a read from a *different* LCN than the write (stale
   mapping/MCB) or a hash divergence between stages is directly visible.
4. Deploy, run the 25× repro, and classify the first divergent stage.  Only
   then design the fix experiment.

## Superseded round-56 plan (steps 2-3 assumed an A/B-positive; kept for rationale)

Rationale: round 51 pinned the boundary — the garbage is already in NTFS's
aligned `ReadAttribute` destination before the bounce copy, while disk and
small reads of the same offset are correct; round 42 saw no store of garbage
into the watched destination frames, so the read's *source* is stale.  The
round-53 `CachedFileRecord` hole (readers use it under `MainResource`; paging
writes free it via `NtfsInvalidateCachedFileRecord()` under only
`PagingIoResource`) matches every §2 discriminator: NTFS-only (FAT has no such
cache); single-CPU deterministic (the reader *blocks* on synchronous disk I/O
mid-request, the lazy writer runs, invalidates and frees the record, the
reader resumes on freed pool — scheduling, not SMP); freshly-written-file
discriminator (only a just-written `cc*.o` has lazy-writer paging writes
racing ld's first reads); confined to the toolchain (only its temp files are
dirty).  One producer also explains every garbage variant: resident $DATA →
copied bytes are reallocated pool (RMAP/Ntf/MmSt tags, zeros); non-resident →
mapping pairs decoded from freed pool yield a garbage LCN → wrong cluster read
(the `.s` text / `0x230A00FF`).  A garbage length decoded from freed pool can
drive an oversized fill into adjacent pool — a candidate for §4's push-lock
victim (bidirectionality).

1. **Step 1 — one-run A/B: neuter the cache.** Make `NtfsReadFile` (and the
   prefetch consumer) always miss: fresh `ReadFileRecord` per read, freed
   locally, never installed.  Build `ntfs.sys`, deploy guest-side to the
   repaired `reactos.qcow2` (no host mount), run 25× repro on `C:`.
   OK=25 → region proven, go to step 2.  OK=0 → cache exonerated, go to step 4.
2. **Step 2 — prove the interleaving.** Restore the cache; the generation
   probe (`CacheGenerationBefore/AfterFind/AfterRead` + `BUG7_STAGE`) is
   already committed in `rw.c`.  Add logging of every
   `NtfsInvalidateCachedFileRecord` caller (path + resources held) and assert
   decoded `LCN < volume clusters` at the `ReadAttribute` choke point.  A
   generation bump between lookup and the attribute copy on the failing
   `off=0x194,len=0x14c` request is the smoking gun.
3. **Step 3 — proper fix (root, matching Windows).** The cached record must
   not be freed while any I/O may dereference it: reference-count it (readers
   take a ref under a spinlock; invalidate drops the cache's ref; free on
   last release) or copy-out under a spinlock the invalidator also takes.
   Verify: 25× repro on `C:`, FAT `D:` control, `bug7_process_chain`, and the
   §5e guest binutils build as independent confirmation.
4. **Step 4 — fallback if step 1 doesn't fix:** staged hashes through
   `NtfsReadFile` (raw record at lookup → attr context after `FindAttribute` →
   decoded LCN/len to `NtfsReadDisk` → aligned buffer post-read → post-bounce
   copy), all with liveness markers; the first divergent stage names the
   producer.  Only then escalate to a frame-armed TCG watch on the CRT
   buffer's frames (§5aj), not the read destination.

Push-lock crash stays classified as a downstream victim unless step 2's logs
show an oversized fill / wild write.

**Round 55 -- ReactOS in-place update really repairs the original qcow2's
registry; the repro environment is restored:** The updater's normal kernel
accepted a structurally invalid value list because `CmpValidateValueList()`
only validated lists where `Count > 0`.  The failing SYSTEM hive instead had
`Count == 0` and a stale non-NIL list cell (`0x1ed8f0`).  The next native boot
then tried to add the first value and asserted at `cmvalue.c:254`
(`ChildList->List == HCELL_NIL`).

`CmpValidateValueList()` now rejects that impossible state as a corrupt value
list.  The existing `CmCheckRegistry(... FIX_HIVE)` self-healer consequently
detaches the stale cell before the update writes the hive; it is a targeted
repair, not a replacement guest or host-side file modification.  The update
of the **original** `reactos.qcow2` logged the stale cell, repaired the
additional damaged hive bins, completed normally, wrote `SYSTEM.ALT`, and
shut NTFS down.  Direct boot then passed the prior assertion, reached desktop
services, and launched luagent.  An in-guest command confirmed
`C:\\msys64\\mingw64\\bin\\gcc.exe` is present (`MSYS2_OK`).

Validation was entirely guest-side QEMU I/O: no host mount, loop/NBD,
guestfs, partition extraction, or offline injection.  The temporary fresh
qcow2 used while diagnosing the updater has been deleted and must not be used
for bug 7.  The repaired original image is now the sole repro target.

**Round 54 -- historical intermediate: guest-only ReactOS update repaired the boot path,
but the test
image's registry remains structurally damaged:** No host mount, loop device,
NBD, guestfs access, partition extraction, or offline file injection was used.
The rebuilt `bootcdntfs` was attached only as QEMU's secondary CD and its normal
in-place updater selected `C:\\ReactOS` on disk 0/partition 1.  With the normal
`BootLoaderLocation=2` (MBR plus VBR) option it completed the copy, verified the
SYSTEM/SOFTWARE/DEFAULT/SAM/SECURITY hives, wrote `SYSTEM.ALT`, and shut NTFS down.
Direct QEMU boot then reached FreeLdr and the kernel, proving the guest update
repaired the formerly unusable MBR/VBR path.

The native boot subsequently asserts in `sdk/lib/cmlib/cmvalue.c:254`
(`ChildList->List == HCELL_NIL`) while loading the existing registry.  Setup's
`VerifyRegistryHive` reported that hive valid and therefore did not rebuild it;
this is a separate registry-checker limitation, not evidence for bug 7.  The
qcow2 was consequently not yet a usable MSYS2 repro target.  Continue
all deployment/recovery through the guest's ReactOS setup/update mechanism;
do not work around it by mounting or extracting the image on the host.

**Round 52 -- rejected MDL double-free hypothesis:** Source review initially
misread the normal completion APC's MDL cleanup as applying to synchronous
paging I/O too. It does not. `IofCompleteRequest()` handles
`IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO` in an earlier branch: it copies the
IOSB, signals the event, frees only the IRP, and returns before the normal
`MmUnlockPages()`/completion-APC path. Therefore `NtfsReadDisk()` and
`NtfsWriteDisk()` must retain their post-wait `MmUnlockPages()` + `IoFreeMdl()`
cleanup. Commit `983fa9e8072` removed it based on the false hypothesis and was
immediately reverted by `8a033df0b11`; the focused NTFS rebuild passed after
the revert. Do not repeat this lead.

There is no existing kmtest for this precise ownership path. `IoMdl` covers MDL
allocation primitives and the Cache Manager tests observe paging IRPs, but none
drives NTFS's APC-disabled synchronous storage-I/O path. A useful regression
test should perform aligned, noncached reads on a writable NTFS volume while
inside a critical region, verify the bytes, and churn the pool under verifier;
that tests the actual completion/cleanup contract without baking in the false
double-free assumption.

**Round 53 -- cached-file-record lifetime is a concrete race candidate (not a
root cause yet):** The `RMAP` spelling is `TAG_RMAP = 'PAMR'`, the MM
reverse-mapping lookaside tag; it is not an NTFS allocation tag.  Separately,
the FCB-level `CachedFileRecord` optimization is only protected by the
resource held by the current I/O.  Ordinary reads hold `MainResource`, whereas
paging reads and paging writes hold `PagingIoResource`.  A paging write can
therefore call `NtfsInvalidateCachedFileRecord()` and free the cached record
while an ordinary reader is using it under `MainResource`; the read-ahead path
also consults the pointer while holding only `PagingIoResource`.  That is an
independent use-after-free bug in the optimization.

It is **not yet the established explanation** for bug 7.  `FindAttribute()`
copies the selected attribute into a private `NTFS_ATTR_CONTEXT` before
`ReadAttribute()` produces the bad bytes.  A trace must therefore show a
generation change before or during that attribute copy for the failing FCB,
not merely an invalidation later in the request.  The next diagnostic records
that generation at the cache lookup, immediately before/after `FindAttribute`,
and around `ReadAttribute` for the known bad `off=0x194,len=0x14c` request.

**Round 51 -- real in-place A/B on `reactos.qcow2` (inconclusive about the
volume cache):** The boot-CD unattended
upgrade path installed the test driver into the existing NTFS installation without
formatting.  Reverting `44f79ec8647`'s metadata-read route for this A/B
(`ReadAttribute`: volume-stream `CcMapData` -> direct `NtfsReadDisk`) **did not
fix the bug**: `ld.bfd bad.o` still failed, and the exact aligned
`ReadAttribute` slice (`off=0x194,len=0x14c`) already contained different
pool-looking garbage (`... 00 00 24 01`) before its bounce-buffer copy.  A
separate small read of the same `bad.o` COFF string-table-size field returned
the correct `1a 00 00 00`.

This does **not** reject the volume-cache route: replacing a coherent cached
read with raw disk reads after recent writes is not a valid equivalence test,
because raw storage may legitimately lag the dirty volume cache.  It does
strengthen the actual boundary: the corruption is read-pattern-dependent and
present in the NTFS aligned pool/IRP read destination before data reaches ld;
it is not caused by GCC, MSYS2, BFD parsing, or the NTFS bounce copy.  The next
trace must distinguish the resident/non-resident source and capture its exact
producer before comparing cached and raw storage paths.

**Round 44–45 — what is EVIDENCED vs. HYPOTHESIZED (see §5ac–§5ae):**

*Evidenced (built/booted/tested or direct A-B):*
- A `Segment->SystemMapCount==0` guard mirroring the fixed sibling `22a138dc`, applied to
  the *other* clean-page reclaim site (`MmUnsharePageEntrySectionSegment`), was
  **built + booted + tested end-to-end and REJECTED — `OK=0/25`, bug unchanged** (§5ac).
  Reverted.
- Zeroed-list-source verification (`BUG7_ZL` in `MiRemoveZeroPage`) found **0 nonzero
  deliveries** — but is **partly vacuous** under the memory pressure the bug needs (the
  zeroed list is usually empty then, so `MiRemoveZeroPage` takes the free-list `Zero=TRUE`
  branch and never enters the checked path) (§5ad).
- A value-filtered TCG store-watch for the exact garbage bytes `FF 00 0A 23` found
  **0 hits across a failing 3/3 run** (§5ae).  That run's failures were `(NULL) has no
  section` / `invalid operation` — i.e. **the garbage value varies run to run**, so the
  earlier `0x230A00FF` was one instance, not a constant.  Value-filtering is therefore the
  wrong axis; the corrupting write must be caught **frame-wise**, not by content.
- Robust delivery conclusion (from §5aa demand-zero + §5ad zeroed-list + the fact that
  free-list allocations zero inline): **every user-page delivery path zeroes the frame**,
  so ld's frame is genuinely zero at delivery and the foreign bytes appear **after**
  delivery — i.e. a **post-delivery write through a second live mapping of ld's frame
  (a double-map)**.  This much is supported.

*Hypothesized (NOT yet evidenced — do not treat as established):*
- That the second mapper is specifically a **Cc VACB whose frame was reused while its
  system-space PTE survived**.  This is only the *leading* suspect, motivated by the
  NTFS/FAT discriminator below; no probe has yet observed such a VACB write.  The 0-hit
  store-watch neither confirms nor refutes it.  Equally open: another system-space view,
  an MDL from file I/O, or a stale mapping created by the NTFS driver's own paths.

**Volume discriminator (verified this round):** `C:` = **NTFS** fails; `D:` = **FAT32**
(MBR partition type 0x0c) passes 10/10.  A *generic* MM fix (`section.c`) governs both
filesystems equally, yet FAT passes — so the producing free/reuse site is most likely
reached via an **NTFS-specific** path, not generic MM.  This is consistent with (but does
not prove) the VACB hypothesis, and is why the generic `SystemMapCount` guard did not help.

**Next experiment (planned, not yet run):** a value-independent, frame-wise catch — a
per-PFN reverse-count of *system-space* mappings (the blind spot rmap leaves, since rmap
is user-only), asserted zero at `MiInsertPageInFreeList`, to fire at the exact free site if
a frame is returned to the list while a kernel/VACB PTE still maps it.

The strongest current result is that NTFS returns the correct bytes for the exact COFF
field that BFD later reports incorrectly.  That read destination keeps the same physical
frame, and TCG sees no corrupting store to it.  Source/disassembly review of
`_bfd_coff_read_string_table` then corrected the next target: for the "bad string table
size" symptom, BFD validates a 4-byte local stack buffer (`extstrsize[4]` at
`rsp+0x5c`) before any string-table heap allocation or cache assignment.  The next
capture must timeline that stack dword, the `H_GET_32` return value, and the saved
`rdi` `strsize` register inside `ld`.

Rejected by direct A/B or liveness-controlled instrumentation: hyperspace FIFO/TLB
reuse (§5g), NTFS prefetch (§5m), the push-lock stale-CAS fix (§5u), CcCopyRead
(§5v; ld never calls it), NTFS returning corrupt bytes (§5x, §5z), corruption of
the observed read-buffer frame (§5aa), demand-zero delivery (§5aa), and the legacy
section resident-map/COW pool-tag probes (§5u).

**Do not repeat:** rebuilding the current `BUG7_SECTION` probe, adding another
`CcCopyRead` probe, or retesting the hyperspace/push-lock changes.  Those experiments
already produced valid negative results.  Do not chase a BFD string-table heap/cache
allocation for the bad-size diagnostic until the stack/register timeline proves the bad
value survives past validation; allocation happens after the failing check.

---

## 0. Rejected lead: the Vista+ hyperspace mapping (`MiMapPageInHyperSpace`)

**This was a hypothesis, not an established producer.** The Vista+ (`NTDDI_VERSION >=
NTDDI_LONGHORN`) hyperspace path reserved a per-process FIFO slot by a **non-atomic
read/modify/write** of the counter PTE's `PageFrameNumber` bit-field, and `MiUnmapPageInHyperSpace`
tore the slot down **without a TLB invalidate** (and the reserve path never invalidated a
slot's stale translation from a prior FIFO pass). So a hyperspace page-copy can land on the
**wrong physical frame**. The three consumers that then do a full-page write through that VA:
- `MiCopyFromUserPage` (`section.c:1271`) — the **COW copy** on `MmAccessFaultSectionView`.
- `MiZeroPhysicalPage` (`ARM3/pfnlist.c:127`) — zeroes a page (→ `bad string table size 0`,
  `(NULL)` symbol names).
- the section demand-fault delivery.

`RtlCopyMemory`/`KeZeroPages` to the wrong frame **swaps content between two physical
frames** — one a freshly-faulted section page mapped in `ld`, the other a nonpaged-pool page
(RMAP/Ntf/MmSt) or a kernel stack (pushlock wait block). That is precisely the bug's
**bidirectional** kernel↔user corruption.

Why it matches every discriminator that killed the earlier models:
- **Bidirectional** (§4): the wrong-frame write can target a pool/stack frame while the
  source is a user page, or vice-versa. ✓
- **Corruption lands AFTER fault-in** (§3): fault-in delivers a clean page; the damage is a
  *later* hyperspace copy landing on the wrong frame — so the ARM3 fault-resolve content
  scans were clean. ✓
- **Scales with reader memory activity** ld≫nm≫md5sum (§2.5): more faults ⇒ more COW/zero
  hyperspace copies ⇒ more windows. ✓
- **Freshly-written file is the discriminator** (§2.6): a just-written `.o` has resident dirty
  section pages that ld maps **write-copy**, driving `MiCopyFromUserPage`; a long-resident file
  is already private/clean and copies far less. ✓
- **Contained to the toolchain's own processes** (§2.7): hyperspace is per-process
  (`ASSERT(Process == PsGetCurrentProcess())`); a quiescent bystander in another process never
  enters the window. ✓
- **Transient VA alias, invisible to 1 Hz PTE sampling** (§5): a hyperspace slot is a
  transient per-CPU VA held only across one `RtlCopyMemory`; the round-11 samplers could not
  see it — exactly as §5's model predicted. ✓
- **NOT free-list double insert/alloc, NOT free-while-mapped, NOT stale-at-fault-in** (§3):
  correct — no frame is freed or double-allocated; it is a VA-level alias from a non-atomic slot
  reservation + stale hyperspace TLB. ✓

Mechanism (two variants; the diff's own comment states the first):
1. **SMP:** two threads of the *same* process on two CPUs share the process's hyperspace PTEs;
   the non-atomic counter RMW makes both pick the **same** slot → two concurrent copies to the
   same VA with different target PFNs → "one page copy silently changes the physical page below
   another copy." (msys2/cygwin processes are multithreaded; the live repro runs `-smp 2`.)
2. **Stale-TLB reuse:** a slot torn down without TLB invalidate keeps a valid translation; on
   reuse before the FIFO-wrap flush the copy hits the previously-mapped frame.

**Hardening change tested (uncommitted `hypermap.c`):** reserve the slot with
`InterlockedCompareExchangePte` (atomic), flush the whole hyperspace TB only after a successful
wrap reservation, `KeInvalidateTlbEntry` the slot VA before publishing the new mapping, and
`KeInvalidateTlbEntry` on unmap.

**Rejected by A/B:** §5g rebuilt and deployed a stronger global-shootdown variant and
the exact GCC reproducer still failed.  Keep the discussion below as the rationale for
the hardening, but do not treat it as bug7's cause or fix.

(Rounds 1–14 below retained as the diagnostic record that led here.)

**One-line:** Inside ReactOS's MSYS2 environment, `gcc x.c -o x.exe` usually fails because
`ld.exe` sees stale-looking data in its private COFF/BFD state even though the `.o` on
disk and the exact bytes returned by NTFS are correct.  Pool tags have appeared in some
failures and assembly text appeared in another.  A push-lock crash containing a user
address may be related, but a common bidirectional producer has not been proved.

This is the last blocker for compiling `github.com/kreijstal/wine-kreijstal` on ROS.

---

## 1. Symptom

On the msys2-on-ReactOS install (amd64, NT10 target), compiling anything fails at the
**link** step:

```
$ gcc g.c -o g.exe
ld.exe: C:\msys64\tmp\ccXXXX.o: could not read symbols: invalid operation
collect2.exe: error: ld returned 1 exit status
```

Other observed garbage from the same failure:
- `local symbol '!RMAP' has no section`
- `!RMMSS`, `!NtfC`, `!\MMSS`, `!\x..ANtf0`, `(NULL)` as symbol names
- `bad string table size 2583580737`, `bad string table size 0`, `778567680`, `2010710015`
- `could not read symbols: invalid operation` (bfd_error_invalid_operation)

The garbage is **different each run**. The tag bytes are real kernel pool tags:
- `TAG_RMAP = 'PAMR'` (ntoskrnl/include/internal/tag.h) → reads "RMAP" in memory
- `Ntf*` = NTFS pool tags
- `MmSt` / `MMSS` = MM section / MM tags

### 100% reproducer (run in the guest msys2 shell)
```bash
printf 'int main(){return 42;}\n' > g.c
ok=0; fail=0
for i in $(seq 1 25); do
  rm -f g$i.exe
  if gcc g.c -o g$i.exe 2>/dev/null; then ok=$((ok+1)); else fail=$((fail+1)); fi
done
echo "RESULT OK=$ok FAIL=$fail"     # => OK=0 FAIL=25
```

`gcc -c x.c` (compile to `.o`) and `as` (assemble) **succeed**. Only linking (`ld`) fails.
A loop of separate `gcc -c x.c; nm x.o` does **not** reproduce — the `as`→`ld` chain
**within one gcc invocation** is what triggers it.

---

## 2. Hard facts (high confidence, evidence-backed)

1. **Single-CPU, deterministic ~100%.** `-smp 1` fails 25/25 (re-confirmed round 11).
   Not an SMP race, not hyperspace-per-CPU, not a PFN-lock race.
2. **The `.o` on disk is correct.** `md5sum` of the failing `.o` is always correct and
   byte-identical to a known-good reference; a 3rd-party reader sees perfect bytes.
3. **Even a pristine, pre-existing `ref.o` fails every link.** So the failure is NOT
   triggered by the specific object being linked.
4. **Corruption is in ld's PRIVATE in-memory image**, never in any file / Cc cache /
   shared view. Round 8 md5'd all of ld's real large inputs (crt2.o, libmingw32.a,
   libmsvcrt.a, libkernel32.a, …) twice per iteration across two reader processes +
   vs reference: `unstable_reads=0 drifted_reads=0` over 20/20 failing links.
5. **Corruption rate scales with the reader's memory activity / size:**
   `ld ~100%` ≫ `nm ~15%` ≫ `md5sum ~0%` ≫ `objdump` on a stable file `0%`.
6. **Discriminating variable = freshly-written vs stable file.** `nm sd.o` (just written
   by `as`) intermittently garbles; `objdump libmingw32.a` (long-resident) never does.
7. **The corruption is confined to the toolchain's own processes.** An innocent
   "bystander" verifier process running *concurrently with the real gcc churn* (3552+
   verify passes over its 96 MB) was **never** corrupted (round 11). So it is NOT a
   generic "freed frame reused by the next process" leak that would hit any process.
8. **A kernel push-lock crash occurs under the same workload** (see §4).  Its
   relationship to the ld corruption is unproved.

---

## 3. What has been RULED OUT

### Kernel-side instrumentation negatives (rounds 1–10, all reverted/stashed)
- **NOT memory pressure / trim.** Fails 25/25 with MemFree=1.5 GB and with `-m 8192`.
- **NOT a free-list double-insert.** DBLFREE detector in `MiInsertPageInFreeList`: no page
  inserted onto the FreePageList twice.
- **NOT a free-list double-alloc.** Per-PFN allocated/free bitmap (set in
  `MiRemoveAnyPage`/`MiRemoveZeroPage`, cleared on free-list insert): zero re-hand-outs.
  The two owners get genuinely different frames.
- **NOT free-while-user-mapped.** Per-frame live-USER-rmap counter checked at free: 0 hits.
  Combined with a non-firing `MmDereferencePage` rmap!=NULL assert: no rmap-mapped frame
  is ever freed.
- **NOT a shared physical frame double-use between rmap pool and ld.** rmap pool entries
  cluster in a tight physical band (0x9cxx–0x9fxx, the nonpaged-pool zone) disjoint from
  the user frame range; nonpaged pool is frame-monotonic (`MiFreePoolPages` keeps freed
  blocks mapped in the pool VA, frames not returned to MM).
- **NOT pool-frame → user-PTE.** Pool-tagged-frame trap at the 3 ARM3 fault-resolve sites
  (`pagfault.c`): 0 hits over 25/25.
- **NOT stale content at fault-in.** Hyperspace content-scan (first 512B for Ntf/MMSS/MmSt/
  RMAP) at demand-zero/proto/transition fault-resolve: 0 hits. Pages arrive CLEAN at
  fault-in via every ARM3 fault path. Corruption lands AFTER fault-in.
- **NOT stale-TLB-on-syspte-release.** `KeFlushEntireTb` added to `MiReleaseSystemPtes`:
  gcc still 0/25.
- **NOT the amd64 WS-lock nesting bug.** That was a real, SEPARATE bug found+FIXED this
  campaign (commit `dc5f770d21e`: `MiUnlockWorkingSetForFault`/`MiRelockWorkingSetForFault`
  were `_M_ARM64`-only; enabled for amd64). Verified fixed, but gcc still fails 25/25 →
  independent of the gcc corruption.
- **Round 11 pool-side scan (stashed):** a background thread walking every nonpaged-pool
  PTE for a frame owned by a ROS/user PFN — 0 hits (incl. boot baseline).
- **Round 11 user-side scan (stashed):** a background thread attaching to every user
  process and walking its user half for a valid user PTE whose frame's PFN is pool-owned
  (`Pfn->PteAddress` in the pool PTE window) — 0 hits. (Note: 1 Hz sampling can miss a
  transient alias, and short-lived `ld` may not overlap a sample.)

### Userspace synthetic negatives (round 11 — six host-built Win64 probes)
All ran on-target, several *concurrently with the real gcc churn*; none reproduced:

| Probe | What it exercises | Result |
|---|---|---|
| **zerocheck** | fresh `VirtualAlloc(MEM_COMMIT)` pages must be zero | bad=0 / 491520 pages |
| **filecheck** | pattern file via `FILE_MAP_READ` (shared RO mmap) **and** `ReadFile`, under 96 MB pressure ×60 | mm=0 rd=0 |
| **memcheck** | 384 MB resident private region, re-verified intact under 128 MB churn + malloc/free ×80 | bad=0 |
| **bystander** | innocent 96 MB verifier **during** the real gcc churn (50/50 fail), 3552+ passes | bad=0 |
| **fcow** | `FILE_MAP_COPY` (COW = cygwin `mmap MAP_PRIVATE`) over a file written by a separate **exited** child; real COW writes triggered ×40 | mismatch=0 |
| **forkcheck** | cross-process inject: `CreateProcess(SUSPENDED)` + `VirtualAllocEx` at fixed VA + `WriteProcessMemory` parent→child + resume; child verifies under pressure (mimics cygwin fork's memory transfer) | bad=0 |

Plus, using the REAL `ld`:
- **`-Wl,--no-keep-memory`** (makes BFD re-read symbol tables instead of caching/mmapping
  them) does **not** help: baseline 25/25 fail, nokeep 25/25 fail. So it is NOT ld's
  symbol-table cache/mmap-keep strategy.

**Conclusion of round 11:** NO isolated Win32 primitive reproduces. The bug needs the real
cygwin/msys2 runtime's specific *combination/sequence* of operations (the fork+exec chain
`gcc → cc1 → as → collect2 → ld`, cygheap, cygwin's fhandler I/O), not any single primitive.

---

## 4. Possible second symptom (round 11): kernel-side push-lock crash

On a **clean** kernel (all diagnostics stashed — so NOT an instrumentation artifact),
during the gcc churn (process creation of `tzset.exe` / actctx manifest loading), a KDB
last-chance crash fires:

```
Entered debugger on last-chance exception (Exception Code: 0xc0000005) (Page Fault)
Memory at 0x000007FFB643A008 could not be accessed
Frame: ntoskrnl.exe:365b8
```

Resolved: **`ExfWakePushLock + 0xf1`** (symbol at RVA 0x364c7, target 0x365b8;
ImageBase 0x400000).

Key insight: the faulting pointer `0x000007FFB643A008` is a **user-mode address** (the
`0x7FF…` range where ntdll/kernel32 load on Win64). A **kernel** pushlock wait-block's
`Next` pointer is holding a **user DLL address**. So:

- ld's user pages get **kernel pool** bytes, AND
- kernel pushlock structures get **user-range** bytes.

→ This is evidence of kernel memory corruption during the same workload.  It is
consistent with a bidirectional/common producer, but does not prove that it shares the
producer responsible for ld's bad BFD value.

`ExfWakePushLock` walks the pushlock wait-block chain on release to wake waiters. Wait
blocks (`EX_PUSH_LOCK_WAIT_BLOCK`) live on the **waiter thread's kernel stack**. Prime
suspects:
1. A thread terminated during teardown while still linked in a pushlock wait chain (its
   stack freed but not delinked) → `ExfWakePushLock` walks into freed/garbage memory.
2. The same page-alias write that corrupts ld also clobbers a kernel-stack wait block.

This crash is **more deterministic** than the ld failure and is a better entry point.
(`-serial file:` cannot drive kdb; the VM halts on the crash.)

---

## 5. Current evidence model

What is proved:

- The producer object and the object stored on disk are correct.
- NTFS direct I/O returns the correct bytes for the exact field that BFD later rejects.
- The observed read-buffer VA remains on one physical frame, and all stores to the
  relevant dword are zero or correct.
- BFD nevertheless validates a different value.  The bad value was not written to any
  frame armed in round 42, because only NTFS read-destination frames were armed.
- The failure requires the real MSYS2 toolchain sequence and `C:` NTFS temporary files;
  isolated Win32 primitives and the dependency-free topology do not reproduce it.

Inference (as of round 12) — **partly superseded, read with §5ab and §5ad**: the bad value
is read from a *private* location that holds stale/foreign data, not from the watched NTFS
read destination.  **Correction (§5ab, round 43):** for the "bad string table size" symptom
the location BFD reads is a 4-byte *stack* buffer (`extstrsize[4]`), and the bad-size check
runs **before** any string-table heap/cache allocation — so the earlier "BFD caches the
input into a private *heap* allocation and rereads it" wording is DISPROVED; do not pursue a
string-table heap/cache allocation.  **Refinement (§5ad, round 45):** every kernel page-
delivery path zeroes the frame, so that private location is written with foreign bytes
*after* a clean delivery (a post-delivery double-map), rather than being copied/cached
wrong by BFD itself.  The open question is which second mapping performs that write.

The pool-tag and push-lock observations may share a kernel-memory producer, but that link
is still a hypothesis.  Do not use it to assume a physical-frame swap before the watched
BFD allocation provides a writer PC or shows that it arrived already poisoned.

---

## 5b. Reproduction attempts that FAILED to reproduce (round 12, 2026-07-13)

Two more approaches, both exercised end-to-end on target; neither reproduced — each
sharpens the model.

- **kmtest `ExPushLockStress`** (new: `modules/rostests/kmtests/ntos_ex/ExPushLockStress.c`,
  wired into CMakeLists + `kmtest_drv/testlist.c`). Storms a single `EX_PUSH_LOCK` with 6
  exclusive + 4 shared persistent contenders (constant wait-chain build/collapse through
  `ExfWakePushLock`), a churner rapidly creating one-shot waiter threads that block then
  self-terminate (teardown-while-waiting), and 2 pool-churn threads hammering nonpaged
  pool with the exact tags (`RMAP`/`Ntf`/`MmSt`/`MMSS`). Built into `kmtest.img`, booted
  `-smp 4`, ran 30 s. **PASSED, no crash.** → generic pushlock contention + thread teardown
  + pool churn does NOT trigger `ExfWakePushLock+0xf1`. **The pushlock is a *victim* of the
  MM producer, not the root**; reproducing the crash needs the actual MM corruption
  producer active (the real cygwin section/pool/process-create combination), which a
  standalone kernel stress test does not recreate. (Test left uncommitted; passes cleanly.)
- **Instrumented cygwin-runtime reproducer** (`/tmp/cygrepro.c`, built with the host
  `x86_64-pc-cygwin-gcc` 15.2.0 in `~/git/msys-cross/prefix`). Intent: use the REAL cygwin
  `fork()`/`mmap()`/`read()` (cygwin1.dll == same codebase as msys-2.0.dll) to test cygwin
  fork's address-space replication — the one primitive the Win32 probes can't exercise.
  **Blocked** by a cross-prefix ABI skew: the prefix's import lib embeds `fhandler` size
  0x288 while its `cygwin1.dll` is 0x298, and cygwin fatals at init/fork on the mismatch
  (`fatal error - fhandler size mismatch detected - 0x288/0x298`). That is a `~/git/msys-cross`
  build inconsistency (import lib older than the built dll), not a ReactOS bug. To pursue
  this path the msys-cross prefix must be rebuilt so libcygwin.a matches cygwin1.dll (or an
  msys2-runtime-matching import lib used). Deploy needs `cygwin1.dll` shipped adjacent to
  the exe (served from the HTTP dir, not a subdir).

Net: the producer is confirmed to require the real cygwin toolchain's *combination* of
operations; neither an isolated kernel stress nor an isolated userspace primitive recreates
it. The `ExfWakePushLock` crash remains the most deterministic *observation* but is
downstream of the producer.

## 5c. Native no-MSYS reproducer (round 13, 2026-07-13)

The round-12 conclusion that the MSYS2/Cygwin runtime itself was required is **superseded**.
An instrumented upstream MSYS2 3.6.9 runtime was first made to load on ReactOS; its
`fork()` consistently reached `first-success` before every failing compiler run.  A standalone
MSYS program performing `fork() -> execl(gcc) -> waitpid()` passed 10/10, so plain runtime
fork/exec is not sufficient.

A **CRT-free native launcher** then reproduced the failure with neither Bash nor
`msys-2.0.dll` loaded in its parent process.  It imports only `KERNEL32.dll`, sets
`PATH=C:\\msys64\\mingw64\\bin;C:\\ReactOS\\system32`, and calls:

```c
CreateProcessW(L"C:\\msys64\\mingw64\\bin\\gcc.exe",
               L"\"C:\\msys64\\mingw64\\bin\\gcc.exe\" @D:\\msys64\\gcc-single.rsp",
               NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
```

The response file supplies GCC's `-B` prefixes plus one source and output path.  The one-line
source is simply `int main(void) { return 42; }`.  On target, its first run failed:

```
ld.exe: ...ccsw6O3u.o: local symbol `!' has no section
ld.exe: ...ccsw6O3u.o: could not read symbols: invalid operation
collect2.exe: error: ld returned 1 exit status
```

This is the current **minimal reproducer**: native `CreateProcessW(gcc.exe)` plus GCC's
unavoidable `cc1 -> as -> collect2 -> ld` child-process chain.  A truly single-process test
is impossible because GCC itself launches those tools.  The result removes MSYS2/Cygwin
runtime code from the causal set and makes native process-creation/MM interaction a stronger
kernel-side suspect; it does **not** by itself prove that `CreateProcessW` is the corrupting
operation.

## 5d. Instrumentation control and pipeline reduction (round 14, 2026-07-13)

The native reproducer was rerun after removing both diagnostic layers:

- GCC's `-wrapper` option was removed from the response file; and
- the path-level `as.exe` and `ld.exe` wrappers were replaced by their original
  MSYS2 executables (`as.real.exe` and `ld.real.exe`).

All three unwrapped runs failed in the same way, with a fresh temporary `cc*.o` and
`ld.exe: ... could not read symbols: invalid operation`.  The wrappers are therefore
not participating in the corruption.

Further controls narrowed, but did not eliminate, the live handoff:

- `-pipe` still fails, so the preceding `cc1 -> as` temporary assembly file is not
  required;
- `-fno-use-linker-plugin` still fails, placing the observation in normal BFD/COFF
  processing rather than the LTO plugin; and
- `-save-temps=obj` succeeds on both `C:` and `D:` **and when forced into the same
  long `%TEMP%` directory** as the failing `cc*.o`, while a fresh GCC/ld invocation
  links the preserved object successfully.  Thus neither filesystem nor path length is
  the differentiator; it is the live `cc*.o` temporary-file lifecycle.

The dependency-free `bug7_process_chain` rostest now models a direct child-writer ->
fresh-reader handoff and verifies the generated file in the new process.  It passes
12/12, so generic file handoff is not enough; the remaining trigger requires the
GCC/ld-specific live temporary-object state.  Its controller reserves a closed `cc*.o`
file with `CREATE_NEW` before the writer child truncates it, matching GCC/libiberty's
`mkstemps -> close -> child _O_TRUNC` path; it also
performs both a fresh `CreateFileMapping`/`MapViewOfFile` verification and BFD's
string-table seek/read/allocation sequence.  Each reader recreates and verifies the
file mapping 16 times to exercise cached-page reuse.  That stricter control still
passes, including four concurrent fresh reader processes per iteration, isolating the
remaining dependency to the real BFD/ld process state rather than generic file or heap
operations.

## 5e. Persistent source-file corruption during guest-native binutils build (round 15, 2026-07-14)

While building the patched MSYS2 binutils tree inside ReactOS, the compiler diagnosed a
previously valid upstream source line as containing a long run of NUL bytes:

```
E:/binutils-2.46.0/libiberty/md5.c:418:28: warning: null character(s) ignored
OP (B, C, D, A, 1, 2<NUL...>21, (md5_uint32) 0x4e0811a1);
```

The host source at that line is valid (`..., 1, 21, ...`).  A *fresh* guest `sed -n
'418p'` process immediately read the same NUL-filled data.  This rules out a GCC-only
view of memory: the corruption was persistent and independently observable through a
new file reader.  A direct extraction of the same `md5.c` from the powered-off FAT
source image subsequently compared byte-for-byte equal to the host source.  Therefore
the NULs are **not persisted on disk**: they are a persistent corrupted guest view of
cached/file-backed pages, visible to independent guest processes.  It also prevents the
current guest-native binutils build from completing, so its linker instrumentation must
not be treated as available yet.

## 5f. Clean four-stage MSYS2 instrumentation (round 16, 2026-07-14)

A native `cmd.exe` parent, with PATH restricted to the MSYS2 MinGW64 toolchain and
ReactOS system directory, ran the normal MSYS2 `gcc.exe @response` invocation.  Small
native wrappers renamed the real executables to `*.real.exe` and recorded command line
and exit code for `gcc`, `cc1`, `as`, and `ld`.  The clean trace is:

```
gcc.exe  exit 1   @D:\msys64\gcc-single.rsp
cc1.exe  exit 0
as.exe   exit 0   ...as.exe -o ...\ccaZ341A.o ...\ccHLZqoi.s
ld.exe   exit 1   ...ld.exe "@...\ccHc6d8m"
```

`ld.real.exe` then reported:

```
...\ccaZ341A.o: could not read symbols: invalid operation
```

Thus the compiler front end and assembler both complete successfully; the failure is
the linker reading GCC's live temporary object through its response file.  The result
uses only the MSYS2 DLL/runtime and native ReactOS process creation.

## 5g. Hyperspace A/B result (round 17, 2026-07-14)

The hypothesized ARM3 hyperspace change was tested rather than assumed: the rebuilt
kernel added a synchronous cross-CPU shootdown for every hyperspace slot map and unmap,
was copied into the live test guest, the guest rebooted, and the clean native instrumented
GCC response-file reproducer was run again on the same two-vCPU setup.  It still produced
`ld.real.exe: ...ccI69Zuh.o: could not read symbols: invalid operation`.  This rejects
hyperspace FIFO slot reuse as the sole cause of the reproducible linker failure.  Keep
the atomic reservation/local invalidation change as independent hardening, but do not use
it as a resolution of bug7.

## 5h. Fresh executable-image process-chain control (round 18, 2026-07-14)

`bug7_process_chain` was strengthened so that every iteration copies its own native,
dependency-free executable to four new temporary PE files named for the `gcc`, `cc1`,
`as`, and `ld` roles.  The freshly loaded `gcc` image then owns the nested
`cc1 (private-memory churn) -> as (direct cc*.o writer) -> four concurrent ld
(fresh map plus BFD-like reader)` chain.  This adds fresh image sections, nested process
creation/teardown, the GCC-style reserved-and-closed `cc*.o` lifecycle, and the previous
file-map/string-table checks without loading MSYS2.

The target passed both the default 12-iteration run and `stress 256` on the two-vCPU
guest with no mismatch.  Consequently, native image-section creation and ordinary nested
`CreateProcess` are not sufficient by themselves.  The exact triggering state still lies
in the real MSYS2 toolchain/runtime path captured by the four-stage trace.

## 5i. MSYS2 runtime fork control (round 19, 2026-07-14)

The upstream MSYS2 3.6.9 `msys-2.0.dll` was rebuilt with `OutputDebugStringA` markers at
`fork()` and `dofork()`, temporarily substituted for the installed **MSYS2** DLL (the
original was saved and restored afterward), and the clean native GCC response-file
reproducer was run again.  It failed in the usual place:

```
ld.real.exe: ...ccNL97Sq.o: could not read symbols: invalid operation
```

There were no `ROS_MSYS_FORK` markers at all.  Therefore the MSYS2 POSIX runtime fork
path is not executed by this MinGW64 `gcc -> cc1 -> as -> ld` chain; those binaries use
native process creation.  The dependency-free fresh-image rostest is consequently a
valid control for the broad process/image mechanics, although it still lacks whatever
tool-specific native process state triggers the corruption.  No Cygwin DLL was used.

## 5j. COFF-aware dependency-free chain (round 20, 2026-07-14)

The dependency-free `as` role now writes an AMD64 COFF-shaped `cc*.o`: it contains an
`IMAGE_FILE_HEADER`, `.text` section header and raw data, two symbol records, and a
string table immediately after the symbol table.  The `ld` role validates the object
through both repeated fresh mappings and a BFD-like file traversal of those exact COFF
regions before checking the full expected payload.  The controller also runs each role
from a fresh PE image and gives the `gcc` role ownership of the nested sequence.

On the live two-vCPU guest this completed `stress 128` without a mismatch.  The system
volume had only 8 KiB free due to existing binutils artifacts, so the test's `TEMP` was
redirected to the MSYS2 data volume for that run; no existing guest data was deleted.
Thus a valid COFF object and native image/process lifecycle are still insufficient to
trigger bug7.  The exact instrumented MSYS2 MinGW64 compiler chain remains the only
reproducer.

## 5k. Exact GCC result depends on the temporary-file volume (round 21, 2026-07-14)

The clean instrumented native GCC response-file invocation was repeated with only its
temporary directory changed:

```
set TEMP=D:\\msys64\\
set TMP=D:\\msys64\\
gcc.exe @D:\\msys64\\gcc-single.rsp
```

All ten independent runs exited `0`.  The normal temporary directory on `C:` reproduces
the `ld.real.exe: cc*.o: could not read symbols: invalid operation` failure.  The guest's
system volume is now nearly full due to existing binutils artifacts, but a further exact
run on `C:` still reached the same BFD invalid-operation error (not an out-of-space
error) while creating a sub-kilobyte object.  The original failure was also observed
while `C:` had substantial free space.  The result makes the `C:` NTFS file/section
lifecycle the primary remaining discriminator.  It is not an MSYS2 runtime-fork issue
(see §5i), nor ordinary process/image creation or generic COFF handoff (see §§5h–5j).

## 5l. Reconfirmed four-tool trace after the NTFS candidate build (round 22, 2026-07-14)

The live guest's normal `C:` temporary directory was used again, with a native `cmd.exe`
parent and PATH restricted to `C:\\msys64\\mingw64\\bin;C:\\ReactOS\\system32`.
The installed **MSYS2 MinGW64** wrapper binaries recorded the real programs' exit
statuses and their hand-off arguments:

```
gcc.exe  exit 1   @D:\\msys64\\gcc-single.rsp
cc1.exe  exit 0
as.exe   exit 0   ... -o "...\\ccaAmrHx.o" "...\\ccNXpoSm.s"
ld.exe   exit 1   ... "@...\\ccnE4CRO"
```

`ld.real.exe` reported `ccaAmrHx.o: could not read symbols: invalid operation`.
This is a new capture of the exact native MSYS2 chain, not a Cygwin-DLL test, and
again proves the producer/assembler completed before the object was rejected by its
first linker consumer.  It remains the positive reproducer; the dependency-free
COFF-aware rostest in §5j remains a negative control.

## 5m. NTFS paging-read prefetch A/B rejected (round 23, 2026-07-14)

The candidate that disabled NTFS's speculative `MmPrefetchPages` call while completing
a paging read was built as `ntfs.sys`, then installed **only** into a disposable clone
of the system disk.  The installed file's SHA-256 matched the built driver before the
clone booted.  The normal `C:`-TEMP instrumented MSYS2 chain still failed:

```
ld.real.exe: ...\\cccQ1gdI.o: bad string table size 3687055837
ld.real.exe: warning: ...\\cccQ1gdI.o: local symbol `(NULL)' has no section
ld.real.exe: ...\\cccQ1gdI.o: could not read symbols: invalid operation
```

The wrappers again recorded `cc1.exe` and `as.exe` exit 0 and `ld.exe` exit 1.  This
rules out that NTFS prefetch recursion as the cause of the reproducible corruption; the
source change was removed after the A/B test.  The original guest disk was never changed.

## 5n. BFD COFF-reader instrumentation built and run (round 24, 2026-07-14)

The native MinGW64 binutils 2.46 tree was instrumented in
`bfd/coffgen.c` at `_bfd_coff_read_string_table`.  When `BFD_BUG7_LOG` is
set, it records the object filename, string-table offset, and the exact four
bytes returned by `bfd_read` before BFD decodes and validates the length.
`bfd/coffgen.lo`, `bfd/libbfd.la`, and `ld/.libs/ld-new.exe` were rebuilt.
The resulting native Windows linker runs in the disposable ReactOS guest.

Its direct `ld -r` read of a fresh `C:\\bug7-direct.o` generated by the
normal MSYS2 assembler recorded:

```
coff-string-size file=C:\\bug7-direct.o pos=2c6 bytes=1a000000
```

so the instrumentation is in the real BFD COFF reader and sees the expected
little-endian length (26) on that control object.  This independently-built
linker cannot yet replace the packaged MSYS2 linker for a full GCC link: it
hits an unrelated `crt2.o: lseek: Bad file descriptor` (and a later COFF-link
assertion in `ld -r`).  It is therefore an observation tool, not a valid
fix/control for the packaged-linker result.  The packaged MSYS2 four-tool
wrapper trace in §5l remains the authoritative positive reproduction.

## 5o. Dependency-free topology now includes collect2 (round 25, 2026-07-14)

`bug7_process_chain` now uses five fresh executable images per iteration:

```
gcc -> cc1 (private-memory churn) -> as (fresh COFF cc*.o writer)
    -> collect2 -> four concurrent ld readers
```

The new `collect2` role, rather than the gcc role, creates the fresh linker
processes.  This matches the extra real GCC process boundary while retaining a
dependency-free native test.  The rebuilt target completed `stress 128` on a
two-vCPU disposable ReactOS guest with `TEMP` on the FAT data volume:

```
BUG7_PROCESS_CHAIN: no mismatch in 128 iterations
```

Thus adding `collect2`, fresh PE sections, fresh COFF input, and the full
native process topology is still insufficient.  The real MSYS2 toolchain trace
is the positive reproducer, while this test is the strengthened negative
control.

## 5p. NTFS create/map/delete exhaustion is separate (round 26, 2026-07-14)

A disposable clone was prepared offline with 14.9 MiB free on its `C:` NTFS
volume, then ran the normal 64 KiB dependency-free process-chain test with
`TEMP=C:\\`.  It completed 182 iterations without a COFF mismatch; iteration
183 failed in the **writer** (status 4), before any reader observed data.
`dir C:\\` then reported only 40 KiB free and no surviving `cc*.o` files.

This is an NTFS allocation/deallocation exhaustion behavior under repeated
create/map/read/delete, worth investigating separately, but it is not a valid
bug7 reproduction: it is a deterministic storage failure rather than the
real linker's transient in-memory symbol/string-table corruption.  The clone
was discarded and the original disk was not modified.

## 5q. MSYS2-package-layout BFD linker reaches the reader, but not cc*.o (round 27, 2026-07-14)

The local `mingw-w64-binutils` MSYS2 packaging recipe was used to build an
instrumented binutils 2.46.1 linker with the package's static-BFD configuration
(`--disable-shared`, 64-bit BFD).  As in §5n, the probe records the four bytes
returned by BFD when it reads a COFF string-table length.  In a disposable
guest, the real MSYS2 `gcc` was redirected only at its `ld` dispatch to this
linker.  The probe ran and recorded the startup object's valid string table:

```
coff-string-size file=.../crt2.o pos=62cc bytes=340a0000
```

It then stopped at the independent ReactOS failure
`crt2.o: lseek: Bad file descriptor`, before opening GCC's fresh `cc*.o`.
The result rules out the earlier shared-versus-static BFD build layout as the
reason the standalone linker could not complete the packaged GCC invocation.
It does not replace the packaged MSYS2 linker for bug7 diagnosis; the exact
four-tool wrapper trace remains the positive reproducer.

## 5r. Instrumented assembler -> packaged linker reproduces (round 28, 2026-07-14)

The same 2.46.1 package-layout build was instrumented in GAS
`output_file_close`: with `AS_BUG7_LOG` set, it hashes the completed output
only after BFD has closed it.  In a disposable guest the GCC `as.exe` dispatch
was redirected to that native assembler; **GCC, cc1, collect2, and the
packaged MSYS2 `ld.real.exe` were otherwise unchanged.**  The trace was:

```
as-output file=...\\ccOmVGUP.o size=736 fnv64=04cacf47428693e9
as.exe exit 0
ld.real.exe: ...\\ccOmVGUP.o: could not read symbols: invalid operation
ld.exe exit 1
```

Thus a successfully closed, directly instrumented assembler output still
drives the normal packaged linker into the bug.  This is a positive
reproduction with real code-level instrumentation at `as` and the real
MSYS2 linker consumer; it is not a Cygwin-DLL test.  The on-disk object is
complete at the producer boundary, leaving the failure in the later linker/
MM view of that live temporary object.

## 5s. Push-lock shared-release stale-CAS fix (round 29, 2026-07-14)

`ExfReleasePushLockShared` retried a failed wake-handshake compare-exchange
without loading the value returned by that compare-exchange.  Once another CPU
modified the lock word, the retry could therefore spin forever against an
obsolete expected value while a stack-resident wait block remained linked.
The retry now reloads `OldValue` from the observed value, matching the already
corrected exclusive/general release path.  `ntoskrnl.exe` and `kmtest_drv.sys`
(including `ExPushLockStress`) build successfully.  This is a real push-lock
race fix, but it remains a Bug 7 candidate until the exact MSYS2 chain is A/B
tested in a disposable guest.

## 5t. Same-guest positive MSYS2 trace and rostest control (round 30, 2026-07-14)

The rebuilt dependency-free `bug7_process_chain.exe` was copied to the live
two-vCPU guest's `D:\\msys64` volume and run with `TEMP`/`TMP` on that volume:

```
BUG7_PROCESS_CHAIN: no mismatch in 128 iterations
```

Immediately afterward, the normal `C:` temporary directory was restored and
the real MSYS2 MinGW64 response-file invocation was run once more.  Its
instrumented wrappers recorded:

```
gcc.exe exit 1
cc1.exe exit 0
as.exe exit 0  ...\\ccJJK3vB.o ...\\ccixEYVX.s
ld.exe exit 1  ...\\ccNzRSGP
ld.real.exe: ...\\ccJJK3vB.o: could not read symbols: invalid operation
```

This is the requested current A/B: the dependency-free rostest exercises the
fresh-image `gcc -> cc1 -> as -> collect2 -> ld` COFF handoff without a
mismatch, while the instrumented, **MSYS2-only** GCC/cc1/as/ld chain reproduces
the linker failure in the same guest.  The wrapper result agrees with the
source-level GAS close/hash instrumentation in §5r; no Cygwin DLL is involved.

## 5u. Fault-delivery scan clean + push-lock fix A/B-negative (round 31, 2026-07-14)

One kernel carried three changes: the §5s push-lock stale-CAS fix, the hyperspace
hardening, and a `BUG7_SECTION` pool-tag scan (RMAP/NtfC/MmSt/MMSS) at the legacy
section delivery points — `MmNotPresentFaultSectionView` resident-before-map and both
sides of the `MmAccessFaultSectionView` COW copy — filtered to `ld*` processes.  The
scan helpers fall back to `PsGetCurrentProcess()` when the faulting address space is
the kernel's, so Cc cache-view faults taken inside `ld`'s `CcCopyRead` are covered.
Deployed offline (kernel sha `9379229e…`, readback-verified) and booted on the live
two-vCPU guest:

- Three response-file links: FAIL, OK, FAIL — the usual intermittent-high failure.
  The push-lock fix is therefore **not** the bug7 fix (consistent with §2's single-CPU
  determinism).
- `BUG7_SECTION` hits in the serial log: **zero**, including during the failing links.

Combined with the round-10 ARM3 `pagfault.c` scans, these probes found no selected pool
tags at the covered fault-delivery sites.  This did not prove arbitrary page content was
clean.  The then-remaining suspected read path was a VACB
still mapped from `as`'s just-completed `CcCopyWrite` is reused by `ld`'s `CcCopyRead`
**without any fault**.  If the cache page is corrupted after the lazy-writer flush
(disk stays correct) and the VACB is later trimmed and re-faulted from disk, later
readers see clean bytes again — matching §2.2 (md5sum correct) while §5e keeps its
persistent-corrupt-view case (VACB survived).  Round 32 adds a `BUG7_CC` scan of the
source bytes inside `CcCopyRead` itself, immediately before the copy to the user
buffer.

## 5v. CcCopyRead source scan vacuous — ld never uses Cc (rounds 32–33, 2026-07-14)

Round 32 added a `BUG7_CC` pool-tag scan of the source bytes inside `CcCopyRead`
immediately before the copy to the user buffer.  Four failing links produced zero hits —
but round 33's liveness control (`BUG7_CC_LIVE`, logging the first 16 `CcCopyRead`s by
any `ld*` process) ALSO logged **zero**, so the round-32 negative was vacuous:

**`ld`'s reads never traverse `CcCopyRead` at all.**  The reason is in
`drivers/filesystems/ntfs/rw.c` (`NtfsRead`): the cached read path is disabled with a
literal `if (FALSE && !PagingIo && !NonCachedIo ...)`.  Every normal read takes the
`NtfsReadFile → ReadAttribute` direct-from-disk path through kernel intermediate
buffers, then a copy back to the caller's buffer.

This temporarily reframed the producer search: `ld`'s file bytes pass through NONPAGED-POOL
intermediate buffers adjacent to the very RMAP/Ntf/MmSt allocations whose tags appear in
the corruption.  A short-fill or length-mismatch there would (a) hand back stale pool
neighbour bytes in the unwritten tail of a read — pool tags in `ld`'s data while the
on-disk file is byte-perfect — and (b) an overlong fill could scribble file bytes (COFF
data full of `0x7FF…` user-range constants) over adjacent pool.  This was a hypothesis
for connecting §4's push-lock symptom to the ld failure, not proof of one producer.  It
coheres with §5k (only the `C:` NTFS volume reproduces; `D:` FAT uses fastfat's real Cc
path) and with the known NTFS premature-VDL/cluster-tail weakness on freshly written
files.

Round 33 also A/B-confirmed on the live guest that the §5s push-lock fix does NOT cure
the link failure, and a first version of a PTE-ownership audit (private-page PFN
back-pointer check at read + at `ld` process teardown) asserted on an unheld PFN lock
(`freelist.c:467`) — fixed for round 34.

## 5w. Round 34 plan: NTFS direct-read exit probe

`NtfsRead`'s success path now scans, for `ld*` processes, the exact bytes about to be
returned for the four pool tags (`BUG7_NTFSREAD_TAG`), logs short reads
(`BUG7_NTFSREAD_SHORT` want vs got), and proves liveness (`BUG7_NTFSREAD_LIVE`).  The
PFN-lock-corrected PTE audit rides along.  Outcomes: TAG hits → the producer is inside
the NTFS read/buffer path (chase `ReadAttribute`'s fill logic); liveness>0 with zero
TAGs and failing links → the read hand-off is clean and the corruption postdates the
copy into `ld`'s private page (PTE audit / TCG watchpoint next).

## 5x. Rounds 34–35: NTFS returns clean bytes INTO ld's own user VA; PTE audit clean

Round 34 (`BUG7_NTFSREAD_*` probes at `NtfsRead`'s success exit, kernel sha `db7b46f7…`,
ntfs.sys `b73ccd2a…`): liveness proven (12 `_LIVE` lines — cc*.o, crt2.o, sysmain.sdb),
`_SHORT` hits are benign EOF clamps (want=0x10000 got=0x80e on the 0x80e-byte object),
**zero `_TAG` hits** across 3/3 failing links.  The bytes the filesystem hands back are
clean of pool tags.

Round 35 (kernel `9f7c24c5…`, ntfs.sys `eac8503a…`): the NTFS device is `DO_DIRECT_IO`,
so the data is written through an MDL system mapping of the requestor's pinned frames.
A new `BUG7_NTFSREAD_ALIAS` probe re-read every completed read through `Irp->UserBuffer`
(ld's own VA) and compared: **zero divergences** in 3/3 failing links.  The PFN-lock-fixed
PTE-ownership audit ran at every ld exit (`BUG7_SWEEP_LIVE checked=2503` for ld.real.exe)
with **zero** back-pointer mismatches.

Net: correct bytes are present in ld's private pages at every read completion; page
tables are consistent at exit; disk, Cc (unused), NTFS buffers, and the instrumented
fault sites are clean — yet BFD's parsed tables contain stale-looking bytes.  At this
point probe-site guessing was exhausted, so the investigation escalated to TCG
instruction-level capture.  Round 36 then established that TCG reproduces.

## 5y. Round 36: TCG reproduces; ALIAS=0 was vacuous; store-watch plugin built

- **TCG gate: the bug reproduces under `-accel tcg -smp 1`** (2/2 links failed, then
  again 2/2 under the plugin build).  All KVM/virtualization-timing suspects are
  eliminated, and instruction-level physical tooling is now available.
- **Round-35 correction:** the `BUG7_NTFSREAD_ALIAS` compare was gated on
  `Irp->UserBuffer != NULL`, which is NULL on this direct-I/O path — it never executed
  (same vacuous-negative trap as §5v; liveness markers are now mandatory on every probe).
  The user VA must come from `MmGetMdlVirtualAddress(Irp->MdlAddress)` and the pinned
  frames from `MmGetMdlPfnArray`; comparing the MDL PFN against the requestor's current
  PTE resolution of the same VA is the numeric alias check (round 37).
- **New tool:** `/tmp/bug7watch.{c,so}` — QEMU TCG plugin logging every guest store to a
  set of watched guest-physical pages (hwaddr-based via `qemu_plugin_get_hwaddr`, so it
  sees writes through ANY va alias, kernel or user) with the storing instruction's PC.
  Watched pages are armed at runtime through mmap'd `/tmp/bug7_ctl.bin`; the ntfs.sys
  probe advertises `Temp\cc*.o` read-destination frames over serial (`BUG7_ARM pa=…
  curpa=…`) and delays 200 ms so the host armer (`/tmp/bug7_tcg_watch.py`) can set the
  slots before ld parses.  Rounds 37–42 used this mechanism for the armed captures
  described below.

## 5z. Round 40: kernel returns correct bytes for the exact failing read

The `NtfsRead` probe now FNV-hashes every `Temp\cc*.o` read (`BUG7_RDH`, with file
size and `CurrentByteOffset`).  In a failing TCG run, `ld` failed on `ccH5EceG.o` with
`bad string table size 587792383` (= `0x230A00FF`).  The same run's probe shows the
string-size read — `off=2c6 got=4` — was served TWICE, both times hashing
`8c9aef585f433aaf`, which equals FNV-1a of the **valid** bytes `1a 00 00 00` (string
table size 26).  It does NOT match the garbage.  Every other (off,len) group of that
file is single-hash consistent, and the only `BUG7_RDFAIL`s are benign `STATUS_END_OF_FILE`
at exactly the response file's size.

**Therefore the kernel read path (NTFS → MDL → ld's pinned frames, PTE-verified) delivered
correct bytes for the very read whose value BFD then saw as garbage.**  The corruption is
in ld's user-visible memory after syscall return.  Round 39's kernel-only full-link store
log shows no kernel store writing garbage to the armed read-buffer frames.  Later round
42 evidence narrows this further: BFD does not validate the watched read-buffer dword.

Decode of the garbage: `0x230A00FF` = bytes `FF 00 0A 23`, resembling `"\n#"`-leading
assembly text from the temporary `.s` file.  At this point the working hypothesis was
**demand-zero faults delivering stale, un-zeroed frames**:
- `.s` text in a fresh BFD/heap page (this run),
- freed pool pages with live-looking RMAP/Ntf/MmSt content (tag runs),
- true zero/partially-zeroed frames (`(NULL)`, `bad string table size 0` runs).
Round 11's standalone `zerocheck` might have missed it if stale frames required the
toolchain's own exit churn.  Code inspection of `MiResolveDemandZeroFault` shows the
trust point: a page from `MiRemoveZeroPageSafe` (ZeroedPageList) sets `NeedZero=FALSE`
("guaranteed to be zero-filled") and is mapped without verification.  Round 41 tested
that guarantee at the choke point (`BUG7_DZ`: hyperspace-scan every
zeroed-list page delivered to a user process; log pfn, VA, process, first nonzero
qwords).  Round 41 subsequently found demand-zero delivery clean, so this hypothesis is
rejected for the observed failure.

## 5aa. Round 42: bad value is not from the watched read destination

Unfiltered TCG store-watch (all stores, value + RSP return-addresses) on the failing
`ccHR9oXc.o` link.  The string-size read (off=2c6 got=4) again hashed to the **valid**
0x1a, into buffer VA 0xC2F8FC / pa 0x361f8fc.  Findings:
- VA 0xC2F8FC stayed on the SAME physical frame (0x361f000) for all 9627 of its stores —
  **no transient VA→frame remap** of the read buffer (refines §5's alias model: the buffer
  itself isn't re-pointed).
- Every store to that dword writes 0 or valid data.
- The garbage value BFD reported (`bad string table size 587792383` = 0x230A00FF) is
  **never stored to any armed frame** — searched all byte-orders across 2.47M stores: 0 hits.

Conclusion from round 42 alone: the value BFD validated did not come directly from the
watched NTFS/read-buffer dword.  It came from another address or from state derived
elsewhere.  Demand-zero delivery was clean in round 41.  Standby/modified-list reuse and
section/CoW delivery remain possible only for the as-yet-unidentified user-private
location; the tag-only section probes do not establish general cleanliness.

## 5ab. Round 43: BFD heap/cache was a false next target; use GDB stack/register timeline

Review of `/tmp/binutils-2.46.1-bug7/bfd/coffgen.c` and the built
`/tmp/binutils-2.46.1-bug7-build/ld/ld-new.exe` shows the bad-size check runs before
any string-table heap/cache allocation:

```c
char extstrsize[STRING_SIZE_SIZE];
...
bfd_read (extstrsize, sizeof extstrsize, abfd);
strsize = H_GET_32 (abfd, extstrsize);
...
if (strsize < STRING_SIZE_SIZE || (filesize != 0 && strsize > filesize))
  ... "bad string table size" ...
strings = (char *) bfd_malloc (strsize + 1);
```

The relevant disassembly in the current instrumented `ld-new.exe` is:

```text
base 0x140000000, _bfd_coff_read_string_table RVA 0x71fd0
0x7204d  call bfd_read              ; writes extstrsize at rsp+0x5c
0x72052  cmp rax,0x4                ; post-read, stack bytes should be valid size
0x72061  call [abfd->xvec+0x88]     ; H_GET_32, expected bfd_getl32 RVA 0x4ed00
0x72067  return from H_GET_32       ; rax is decoded strsize
0x7206e  mov rdi,rax                ; save strsize
0x72076  return from getenv
0x7208c  return from bfd_get_file_size
0x72092  invalid low-size path setup; r8/rdi is rejected strsize
0x720db  invalid high-size branch   ; filesize < strsize
0x720dd  allocation path            ; only reached after validation passes
```

`.gdbinit` now has `ros-bug7-bfd-trace [base] [proc] [log]`.  It arms those instruction
addresses, filters to `ld` by current EPROCESS image name, and logs the stack dword
`rsp+0x5c`, its CR3-translated physical address, `rax`, `rdi`, `rbx`, and
`abfd->xvec+0x88`.  The default base is the image's preferred `0x140000000`; if ASLR
loads it elsewhere, pass the actual base.

Expected classification from one failing GDB run:

- stack bytes bad at `post_bfd_read`: corruption is in `bfd_read`/MSYS2 stdio copy or
  the stack was already overwritten before BFD decodes it.
- stack bytes good but `rax` bad at `post_h_get32`: wrong decoder pointer or decode path.
- `rax` good but `rdi`/invalid setup bad later: register/control-flow corruption after
  decode, not the file read or heap allocation.
- all values good through validation: the bad diagnostic is from a different object/file
  than the watched NTFS read, so correlate by BFD filename and offset before adding more
  MM probes.

## 5ac. Round 44: SystemMapCount guard on the sibling reclaim site — BUILT+BOOTED+REJECTED

The fixed sibling of this bug family is `22a138dc` ("[NTOS:MM] Don't page out a clean
segment page that a system-space view still maps").  It added `Segment->SystemMapCount == 0`
to the page-out reclaim in `MmCheckDirtySegment`, because a Cc VACB (system-space view)
carries **no per-process rmap** and `MmMakeSegmentResident` installs its pages at
`SHARE_COUNT == 0` until the view's VA is first faulted — so a clean, `SHARE_COUNT==0`
page can still be live under `Vacb->BaseAddress`.

A static audit found the **other** unguarded clean-page reclaim: `MmUnsharePageEntry-
SectionSegment` (section.c, in the `IsDataMap` last-view-drop branch, introduced by
`2d09d9cf2e3`).  It frees a clean data-file page when the last **per-process** SHARE_COUNT
drops to 0 under memory pressure (`MmAvailablePages < MmLowMemoryThreshold`), reached from
`MmUnmapViewOfSegment` (the comment names `CcRosTrimCache` VACB-free).  No `SystemMapCount`
check.  Fix applied: add `Segment->SystemMapCount == 0` to the reclaim condition, and move
the kernel-AS `SystemMapCount` decrement to *before* the page-unshare loop so the legitimate
last-VACB case still reclaims (no memory-exhaustion regression).

**Result: built `ntkrnlmp`, deployed offline to `/tmp/ros.raw`, booted clean (`5A5A=0`,
no WS-assert), ran 25× `gcc g.c -o gN.exe` — `RESULT OK=0 FAIL=25`.**  Every link still
failed with `could not read symbols: invalid operation` / `local symbol has no section`.
The `SystemMapCount` hypothesis is **empirically rejected**; the fix was reverted.  A
generic MM guard also cannot explain the volume discriminator (FAT `D:` passes).

## 5ad. Round 45: zeroed-list source is (apparently) clean; all delivery paths zero

Round 41 verified ARM3 demand-zero *delivery*.  But `MmAllocPage` (the ROS-Mm section
allocator used by cygwin/MSYS2 `mmap` heap = pagefile-backed anonymous sections) pulls from
`MiRemoveZeroPage`, which **trusts the zeroed list without re-zeroing** — a source round 41
never checked.  Added `BUG7_ZL` in `MiRemoveZeroPage`: when a page is taken from the zeroed
list (the `!Zero` branch), hyperspace-map it and scan for any nonzero qword; log pfn +
content.

**Result: 0 `BUG7_ZL` hits during a failing 25/25 run.**  Caveat: this is **partly vacuous**
— under the memory pressure the bug requires, the zeroed list is usually empty, so
`MiRemoveZeroPage` falls back to the free list with `Zero=TRUE` and zeroes inline
(`MiZeroPhysicalPage`), bypassing the `!Zero` branch entirely.  So the probe rarely runs in
the failing scenario.

Robust conclusion independent of the vacuous branch: **every** user-page delivery path
zeroes the frame — free-list pages inline, zeroed-list pages by construction (and verified
where sampled).  Therefore ld's frame is genuinely **zero at delivery**, and the `.s`
garbage appears **after** delivery.  Combined with round 40 (kernel delivered the correct
`0x1a`), round 42 (the garbage `0x230A00FF` is never stored to the watched read frame), and
round 41 (demand-zero clean), the best-supported model is a **post-delivery write through a
second, live mapping of ld's frame** (a double-map).  The NTFS-vs-FAT discriminator makes a
**Cc VACB whose frame was reused while its system-space PTE survived** the *leading
hypothesis*, but this is NOT established — no probe has yet observed such a write, and other
second-mappers (another system-space view, a file-I/O MDL, or an NTFS-driver mapping) remain
open.

## 5ae. Round 45: value-filtered TCG store-watch — 0 hits; poison value is not constant

Attempt to catch the write by **value**.  Plugin `/tmp/bug7watch_val.c`
(uses `qemu_plugin_mem_get_value`, filtering cheaply on every store) logs every guest STORE
whose bytes contain `FF 00 0A 23` (little-endian `0x230A00FF`), with PC, guest-physical
target, size, value, and up-to-4 stack return addresses.  Built and run under
`-accel tcg -smp 1 -plugin /tmp/bug7watch_val.so` against a failing link.

**Result: `RESULT OK=0 FAIL=3` (bug reproduced) with `PLUGIN_HITS=0`.**  No store anywhere
wrote that 4-byte pattern.  This run's failures were `(NULL) has no section` and
`invalid operation` — **not** the `0x230A00FF` value.  Conclusion: **the garbage value is
run-dependent** (it is whatever stale content the reused frame happened to hold; `0x230A00FF`
was one instance from an earlier run, not a constant).  Therefore **value-filtering is the
wrong axis** — the corrupting write must be caught **frame-wise**.  (`.s` text in ld's memory
can only come from physical-frame reuse of a page `as` used for the `.s` file, since `as` is a
separate, already-exited process and `ld` never opens `.s` — but the *specific* stale bytes
differ each run.)

Planned value-independent successor: a per-PFN reverse-count of **system-space** mappings
(the blind spot rmap leaves — rmap tracks only user mappings), incremented on kernel-AS
`MmCreateVirtualMapping`, decremented on the matching delete, and **asserted zero in
`MiInsertPageInFreeList`**.  If a frame is returned to the free list while a system/VACB PTE
still maps it, that assert fires at the exact free site with the leaker's stack — proving or
refuting the VACB hypothesis by construction rather than by guessing sites.

## 5af. Round 46: "section fault maps an ld-private frame" — BUILT+BOOTED+0 hits

Value-independent, frame-wise detector.  New per-PFN owner map (`MiBug7Owner`, a byte per
PFN in `ntoskrnl/mm/ARM3/pfnlist.c`): set when a frame is delivered as a user *private*
(ARM3 demand-zero) page (`MiResolveDemandZeroFault`), cleared when the frame is freed
(`MiInsertPageInFreeList`).  Check (`BUG7_DBLMAP`) at the resident-SSE map in
`MmNotPresentFaultSectionView`: if a section/VACB fault maps a frame currently flagged
user-private, that is a stale resident SSE pointing at a reused frame == the page double-use.

**Result: built, deployed, booted clean, 25× repro `OK=0 FAIL=25` (bug reproduced), and
`BUG7_DBLMAP` = 0 hits.**  So **no ARM3-private frame delivered to ld is ever mapped by a
section fault.**  Consequences:

1. The "a Cc VACB *faults* and maps ld's frame" path is **ruled out** — further weakening
   the VACB-as-producer hypothesis in the specific fault-time form.
2. A fault-time detector structurally cannot see the write if the corrupting store goes
   through an **already-valid stale PTE** (a `memcpy`/`RtlCopyMemory` via `Vacb->BaseAddress`
   or an I/O MDL system VA — no page fault, so `MmNotPresentFaultSectionView` never runs).
   This is now the leading shape.
3. ld's corrupted frame may not be ARM3-private at all: MSYS2/cygwin's `mmap` heap (BFD
   cache and stdio buffers) is **pagefile-backed anonymous *section* memory**, delivered via
   `MmNotPresentFaultSectionView`, not `MiResolveDemandZeroFault` — so the private-only
   marking would not flag it.

Net: the detector's silence is itself informative — the write is not a section *fault*, so the
next experiment must catch a **no-fault kernel store** to one of ld's frames.

## 5ag. Round 46 next: frame-armed TCG watch for a no-fault kernel store

Reuse the kernel's `MiBug7Owner` map to arm frames, and catch **kernel-PC stores** to them in
the TCG plugin (value-independent AND fault-independent).  Plan:
1. Allocate `MiBug7Owner` **physically contiguous** (`MmAllocateContiguousMemory`) and emit its
   physical base once on serial (`BUG7_OWNER pa=...`).  Extend the marking to also flag
   **anonymous (pagefile-backed) section pages** delivered to a user process in
   `MmNotPresentFaultSectionView`, so ld's cygwin `mmap` frames are covered — not just ARM3
   private pages.
2. Plugin reads the owner byte for each store's target PFN directly from guest physical memory
   (`qemu_plugin_read_memory_hwaddr(base + pfn, 1)`); if the store's PC is a **kernel** address
   and the target frame is owned by ld, log PC + pa + stack.  A kernel `memcpy`/Cc/MDL store
   into an ld-owned frame is THE producer, and its PC names the function — no dependence on the
   (run-varying) poison value or on a page fault.

## 5ah. Round 47: GDB proves corruption is present on BFD's stack immediately after `bfd_read`

A controlled single-vCPU KVM run (required because x86 hardware breakpoints are
per-vCPU) ran the normal MSYS2 `gcc` reproducer and failed with:

```
ld.real.exe: ccYwlPEB.o: bad string table size 0
ld.real.exe: ccYwlPEB.o: could not read symbols: invalid operation
```

The first invalid-size timeline is conclusive:

```text
post_bfd_read  extstrsize @ 0x00c2fa04 = 00 00 00 00, bfd_read return rax = 4
post_h_get32   extstrsize = 00 00 00 00, H_GET_32 return rax = 0
post_load      extstrsize = 00 00 00 00, saved strsize r9 = 0
invalid_size   extstrsize = 00 00 00 00
```

The same `abfd` (`rbx=0x00f15f20`) had just read a valid `0x1a` string
table size in another invocation.  The package `ld.real.exe` uses
`extstrsize[4]` at `rsp+0x44` (not the `rsp+0x5c` offset of the separately
built `ld-new.exe`).

Trace setup corrections made while obtaining this result:

- The running guest executes the packaged 1,969,145-byte
  `C:\\msys64\\mingw64\\x86_64-w64-mingw32\\bin\\ld.real.exe`
  (MD5 `a868ebc92ae677429bcf2ada3d044877`), not the separately built 17 MB
  `/tmp/binutils-2.46.1-bug7-build/ld/ld-new.exe`.  Its relevant code RVAs
  are `0x75c5c`, `0x75c75`, `0x75c82`, and `0x75d40`.
- A suspended-process probe verified its loaded image base is its preferred
  `0x140000000`; ASLR was not the cause of the initial zero-hit trace.
- x86 provides four hardware debug-register slots.  The original eleven-point
  helper could arm but could not run reliably; it was reduced to the four
  points above.
- Hardware breakpoints are per-vCPU.  The two-vCPU capture missed the `ld`
  thread; the single-vCPU configuration reproduced and traced it.
- At a user-mode breakpoint `$gs_base` is the MSYS2 TEB, not the kernel KPCR.
  The process-name filter therefore returned unknown and discarded every hit.
  The helper now retains unknown-process hits; the four absolute addresses
  are unique to `ld.real.exe`.

The first attempt to transfer the base probe through guest `curl` triggered
an unrelated TCP/IP `Endpoint != NULL && Endpoint->AddressFile != NULL`
assertion and required a VM reboot.  The successful probe was deployed
offline to the guest disk; this assertion is not evidence about bug7.

**Conclusion:** this failure is not a BFD decode/register/heap-cache issue.
`bfd_read` reports a successful four-byte read while the destination stack
buffer already contains zero.  This narrows the producer to the packaged
`ld.real.exe` BFD/CRT/Win32 I/O copy path used by `bfd_read`, or a write that
overwrites that stack slot before its return breakpoint.  Do not call this an
MSYS2 POSIX-runtime path: §5i proved this MinGW64 chain does not execute
`msys-2.0.dll`'s `fork()` path.  The earlier NTFS probes remain
consistent: they proved the kernel delivered correct bytes to its direct-I/O
destination, not that a later MSYS2 userspace copy used those bytes correctly.
Do not build the round-46 frame-owner/TCG detector until this userspace copy
path has been instrumented.

## 5ai. Round 48: the BFD file-iovec callback itself writes the bad value

GDB then traced the unmodified packaged `ld.real.exe` below `bfd_read`.  Its
four-byte reads do **not** take BFD's buffered `memcpy` path.  They call the
file-iovec `bread` callback at `0x1400fe3f0`; that callback ultimately calls
the CRT file-read routine and writes directly to BFD's stack destination.

In a failing `ccEqvXoZ.o` link (`bad string table size 0`), the same callback
returned four bytes and wrote the following values:

```text
post_iovec dst=0x00c2f464  rax=4  dst8=1a000000...  # valid string size
post_iovec dst=0x00c2fa64  rax=4  dst8=00000000...  # failing string size
```

There is no intervening BFD copy and no later stack overwrite: the direct
file-iovec callback is the writer of the zero.  The kernel's simultaneous
`BUG7_RDH` records for that exact `ccEqvXoZ.o` offset `0x2c6` still report
the valid `1a 00 00 00` bytes twice.  Thus the divergence occurs above the
NTFS direct-I/O completion and inside the packaged linker's BFD file-iovec /
CRT file-read path (or its userspace buffer state), not in BFD decoding.

## 5aj. Round 49: zero is already in MSVCRT's nonempty file buffer

The next GDB capture traced the imported MSVCRT `fread` call made by the
unmodified packaged linker.  It reproduced the exact `bad string table size
0` variant for `cc8Z4bwj.o`.  The failing four-byte copy was:

```text
pre   dst=0x00c2fa04 file=0x7ffb5081320
      ptr=0x0269c112 ptr8=0000000000004d41 cnt=0x1a
post  rax=4 dst8=000000008281982c ptr=0x0269c116 cnt=0x16
```

The low four bytes copied from `FILE->_ptr` were already zero while the CRT
reported `FILE->_cnt = 0x1a`; this was its normal buffered-copy branch, not a
fresh `ReadFile` failure.  The CRT buffer base was `0x0269bfe0`.  It had been
filled by the same object's NTFS read `off=0x194 len=0x14c`, which spans the
string-size offset `0x2c6` (`0x2c6 - 0x194 = 0x132`).  The source `g.c`
object retained on the guest has
`1a 00 00 00` at `0x2c6`; the bad buffer instead had zero there.

The kernel also logged a distinct `BUG7_RDH` hash for that same larger fill,
whereas its separate small direct reads at `off=0x2c6` were correct.  A retained
`g.c` object confirms the expected string-size bytes, but the complete temporary
object is not assumed byte-identical across compiler invocations; that whole-read
hash is therefore a correlation marker, not an expected-data comparison.

**Historical round-49 conclusion (superseded by rounds 57-58):** MSVCRT is conclusively a consumer of already-bad buffer
contents, so this is not an MSYS2 runtime, GCC/BFD, or ABI problem.  It is not
yet proven that NTFS produced the bad contents: a kernel or userspace writer
could still change the pinned destination after `NtfsReadFile` returns.  A staged
hash probe has been added immediately after `ReadAttribute` and after NTFS's
aligned bounce-buffer copy; its first successful deployment/run will identify
the first bad stage and determine whether MM remains in the producer path.

## 6. Historical next steps (completed by rounds 57-58)

The staged hashes below ultimately proved that the aligned bounce buffer was
only filled through `0x200` while copy-out consumed through `0x2e0`.  No MM
mapping corruption was involved.

1. Add staged hashes inside `NtfsReadFile` for the `off=0x194 len=0x14c`
   buffered fill, specifically before and after each lower filesystem/cache
   operation, to identify the first producer of the wrong bytes.
2. At the first bad stage, log the source and destination mappings/physical
   frames and trace its copying routine; that is the point at which an MM
   mapping corruption can be proved or rejected.
3. Treat the push-lock crash as separate until an actual writer connects it.

Do not rerun the section, CcCopyRead, NTFS hand-off, hyperspace, or push-lock A/B probes.
Their results are already recorded above.

## 6a. Round 50: ReactOS's update path is now selected; this test volume is full

The boot CD had two configuration defects which made its unattended update
settings unreachable: `bootcd.ini` defaulted to `LiveImg_Debug` rather than a
`Setup` entry, and `bootcd/unattend.inf` disabled unattended setup, requested a
fresh install, formatting, and automatic partitioning.  The CD now defaults to
`Setup_Debug` and requests `UnattendSetupEnabled=yes`, `Upgrade=yes`,
`FormatPartition=0`, and `AutoPartition=0`.

The rebuilt CD was booted against the existing disk. `usetup` entered its
actual in-place-update branch and logged:

```text
Unattended upgrade: selected installation "ReactOS" ; DiskNumber = 0 , PartitionNumber = 1
```

It then failed its first destination-file extension with `STATUS_DISK_FULL`.
This is not a routing or formatting fallback: a historical offline read-only inspection of
the NTFS `$Bitmap` found 13,106,772 allocated clusters out of 13,106,944, i.e.
only 172 free 4-KiB clusters (688 KiB), while the update CD is about 1.2 GiB.
No guest data was deleted, no partition was reformatted, and no external
file-copy deployment was used.  A successful non-destructive update test needs
a source installation with sufficient free space.  Do not repeat that host-side
inspection; all current image access is guest-only as specified below.  Later
guest-side cleanup/update work made the original image usable and rounds 55-58
completed on it.

---

## 7. Reproduction / harness notes (how to iterate)

- **In-guest linking is fixed.**  The final regression command is the real
  MSYS2 gcc/link chain on NTFS; it passed two independent 25× runs.  Small
  dependency-free diagnostic probes may still be built on the host with
  `x86_64-w64-mingw32-gcc -O1 -o probe.exe probe.c` and transferred over the
  guest network.
- **Deploy** by serving `/tmp` over `python3 -m http.server 8099` and, in the guest,
  `curl -s -o probe.exe http://10.0.2.2:8099/probe.exe`. Beware a stale http.server on
  8099 serving the wrong dir (curl silently fetches a 404 HTML page).
- **luagent streams empty on long spawns.** Robust pattern: fetch a guest script, launch
  it detached (`nohup bash script >out 2>&1 &`), and **poll a `/c/*_final.txt` file for a
  sentinel** in small reads (`tail -c 3500`). Large `cat` over luagent desyncs the frame
  protocol ("bad magic" / "ROS kdb during recv").
- **Guest-only deployment rule (current):** do not mount, extract, modify, or
  inject files into `reactos.qcow2` from the host.  Use ReactOS setup/update or
  normal guest networking.  The old `/tmp/offline_install.py` procedure is
  superseded and must not be used.
- Resolve a crash RVA: `x86_64-w64-mingw32-nm -n <ntkrnlmp.exe>`, nearest symbol below
  `(ImageBase 0x400000 + RVA)`.
- **GOTCHAS:** `RtlWalkFrameChain` under the PFN spinlock bugchecks 0x0F; unguarded
  `*PteAddress` deref at DIRQL page-faults (use `MmIsAddressValid`); DPRINT-heavy kernels
  are slow and aggravate the intermittent `ExfWakePushLock` crash.

---

## 8. Artifacts (throwaway, in /tmp — do NOT commit)

- Probe sources: `/tmp/{zerocheck,filecheck,memcheck,bystander,fcow,forkcheck}.c`
- Host-built exes staged in `/tmp/httproot/`
- Harnesses (boot + deploy + poll): `/tmp/run_*.py`, guest scripts `/tmp/*_test.sh`
- Obsolete `/tmp/offline_install.py`: prohibited by the guest-only deployment
  rule; do not run it.
- Round-11 kernel scanner diff: `git stash@{0}` ("gcc-corruption round11 user-alias pool
  scanner (throwaway)"); round 5–8 diagnostics in `stash@{1}`.

Related fixes already landed this campaign: `b7fd4e677f0`
(GetFinalPathNameByHandleA — fixed a *different* gcc blocker: cc1 path resolution),
`dc5f770d21e` (amd64 WS-lock nesting — real MM bug, independent of this corruption).
