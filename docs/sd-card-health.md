# SD card health

Open **Settings → System → SD card health**, then choose **Run 5-second test**. The test is read-only: it opens the mounted microSD's raw block device and reads 4K blocks. It does not create a file, change the filesystem, or write to the card, so it also works on a full or read-only-mounted card.

Cheap, worn and counterfeit cards are a common cause of unexplained interface freezes on Ingenic players. A card can still enumerate and show a plausible capacity while taking hundreds of milliseconds to answer a single 4K read, returning errors for some blocks, or wrapping high addresses onto low ones. This page is a quick way to see those symptoms before the music library starts corrupting.

## Running a useful test

1. Insert the card and wait for the library to settle.
2. Stop playback, downloads, Wi-Fi transfer, OTA downloads and library scans. Other I/O makes the numbers look worse than the card is.
3. Disconnect USB mass storage. The row says so and refuses to start while a computer owns the card; a run also stops if the card is removed or shared over USB mid-test.
4. Tap **Run 5-second test** and leave the page open. The interface stays responsive: the reads run on a background `SCHED_IDLE` thread and the page only polls a progress snapshot every 100 ms.
5. **Cancel test** stops between 4K reads. If a kernel read never returns, the page gives up after 15 seconds, invalidates that worker and reports **Card stopped answering** instead of waiting forever.

The simulator has no raw microSD block node, so the page reports **No readable card** there. Use a physical V1 for real numbers.

## What is measured

The five seconds of timed reads are split into two 2.5-second phases, both using 4096-byte reads:

- **Sequential 4K read** walks consecutive blocks from a point inside the card. It reports MB/s.
- **Random 4K read** walks distinct blocks with a deterministic pseudo-random stride coprime to the block count, so the sample is spread over the whole card without immediately repeating offsets. It reports MB/s and IOPS.
- Before the clock starts, five **sparse probes** read blocks near 0%, 12%, 50%, 87% and the last 4K block. The page shows read errors, slow blocks and the byte offset of the first one of each.

A read that does not return a whole 4K block is counted as a **read error**. A read taking at least 250 ms is counted as a **slow block**; these are the stalls that feel like a frozen UI even when average throughput looks acceptable. The slowest read and the number of 4K blocks sampled are shown too.

`O_DIRECT` is used when the kernel accepts it, so the page cache is bypassed. If it does not, the runner falls back to buffered reads, disables readahead and evicts each block after reading it. Either way, the numbers are from the card, not a second pass through RAM.

### The verdict

A short read-only sample cannot certify a speed class or a card: Class 10/U1/V10 ratings are defined using sustained sequential writes, and A1's application class includes both read and write requirements, whereas this test only reads raw 4K blocks. The split and comparison values below are therefore **explicit diagnostic choices**, not pass/fail criteria or a card certification. They are shown beside all measured rates, errors and stalls so a number can be interpreted from the card itself.

- **A repeatable split:** 2.5 seconds of sequential reads and 2.5 seconds of pseudo-random reads. The sequential phase catches a controller that cannot stream, while the random phase catches one that cannot seek; equal halves keep both samples meaningful in a five-second test.
- **10 MB/s sequential read** and **1500 random 4K IOPS**: conservative references borrowed from familiar SD ratings (10 MB/s is Class 10/U1/V10 *write* throughput; 1500 random read IOPS is the A1 rating). Neither is being certified here. They simply put a floor under "this sampled card is too slow to keep a library UI responsive".
- **250 ms** for a single slow block: an operational stall threshold, not an SD spec. A quarter-second pause is visible while scrolling; typical healthy 4K reads are far shorter.

| Verdict | What it means |
| --- | --- |
| **Looks healthy** | The sampled reads met 10 MB/s sequential and 1500 random 4K IOPS, with no read errors, 250 ms stalls or repeated probe data. It does **not** certify the whole card. |
| **Below diagnostic reference** | One of the two rates fell below 10 MB/s or 1500 IOPS. A slow, worn or counterfeit card can do this. Retest while idle, then back up the card. |
| **Slow blocks found** | At least one 4K read took over 250 ms. Back up the card and retest; replace it if the stalls return. |
| **Read errors found** | Some blocks could not be read. Back up now and replace the card before writing more data. |
| **Possible fake capacity** | Distant probe blocks returned identical non-uniform data. Fake-capacity cards often wrap high addresses onto low ones. This is a hint, not proof: identical filesystem structures can also repeat. |
| **Incomplete** | No card, USB sharing, cancellation, an unreadable device, a failed thread start or a hung read. The title says which. |

Read errors outrank repeated data, which outrank stalls, which outrank the diagnostic references. Rates are decimal MB/s and IOPS, not filesystem benchmark numbers; raw-device 4K access is deliberately harsher than a large cached music file read.

## Limits

This is a **five-second spot check**, not a full surface scan, a write test, a SMART reader (SD cards do not expose one) or a measurement of remaining flash life. It cannot certify a card as healthy, prove its advertised capacity, or find a block it never sampled. A card can pass here and still fail when written, or wrap addresses with data that this probe does not recognise.

For a suspected fake-capacity card, copy everything off it and run a full write-and-read verification on a computer (for example F3 or H2testw). That test writes, so it must be done off-player with a card you are prepared to erase. Replace any card that reports read errors or reproducible stalls; the controller's remapping has already run out of spare blocks.

## Implementation notes

- `src/system/device/sdbench.c` owns the read loop, timing, sparse probes, cancellation and verdict. Its I/O boundary is injectable, so `make -C sonix-player/tests sdbench` exercises bad blocks, 300 ms stalls, repeated probe data, cancellation, rate formatting and the real `O_DIRECT`/buffered file path without a card.
- `src/gui/settings/sdhealthpage.c` owns the worker thread, progress bar, translations and layout. `make -C sonix-player/tests sdhealth-ui` drives the production page at 240×320 with a synchronous fake worker, all verdicts, USB/card guards, the stall watchdog, theme refresh, larger text and all eleven shipped languages.
- The System page entry is built once at startup, beside the battery profiler; opening the card-health page does not start a test. Only an explicit tap does.
