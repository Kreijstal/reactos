# iwlwifi firmware-parser host harness

`ucode_parse_test.c` builds the real `fw/ucode_parse.c` for the host and
exercises it. It is **not** part of the driver build - nothing in
`CMakeLists.txt` refers to it.

```sh
cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
   -DIWL_HOST_HARNESS -I.. -o ucode_parse_test \
   ucode_parse_test.c ../ucode_parse.c

./ucode_parse_test [blob.ucode ...]
```

With no arguments it runs the synthetic cases (malformed, truncated,
overflowing, and boundary containers). Pass real `.ucode` files and it
parses and dumps each one as well, checking that every section it reports
lies inside the buffer it was handed.

## Why this exists

A `.ucode` file is parsed in kernel mode before anything has authenticated
it, so a missing bounds check is a kernel bug reachable from a file on
disk. Under ASan the progressive-truncation case feeds every prefix of a
valid container to the parser, which is what actually enforces that claim -
a `FAIL` line proves nothing on its own if the parser read past the end to
decide it.

It is also how the parser gets tested against hardware nobody here has:
the container format is identical whatever silicon it is destined for.

## What it has already caught

Both of these were found by pointing the harness at a real AX211 blob, and
neither would have been visible without one:

- **`IWL_NUM_API_WORDS` too small, and fatal.** Rejecting an API word past
  the end of our bitmap makes every firmware newer than the driver
  unloadable. Upstream warns and continues; the parser now counts the word
  in `TruncatedApiWordCount` and carries on.
- **`IWL_UCODE_SECTION_MAX` of 16.** Real firmware carries far more - see
  the measured table in `fw/ucode_file.h`. `iwlwifi-so-a0-gf-a0-89.ucode`
  has 60 sections in its regular image alone.

## Getting blobs

```sh
BASE=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/intel/iwlwifi
curl -LO $BASE/iwlwifi-so-a0-gf-a0-89.ucode
```

Note the `intel/iwlwifi/` path: linux-firmware moved these out of the
repository root into per-vendor directories, and the old root-level URLs
now 404. The pins in `boot/bootdata/packages/CMakeLists.txt` use the same
path.
