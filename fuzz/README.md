# fuzz

Three of audiaki's parsers read files it did not write, and all three walk a
format where a number in the file decides how far to reach into it:

| target | what it reads | why it is here |
| --- | --- | --- |
| `fuzz_wav` | `wav_read_open` and the two decoders | a chunk list is a length-prefixed walk, and a `data` size that says more than the file holds is what a crash leaves behind |
| `fuzz_meta` | `aud_meta_read_list`, `aud_meta_read_bext` | the same walk again, nested inside a chunk that has already been accepted |
| `fuzz_project` | `aud_project_load`, then `aud_project_save` | a clip names its audio by index and its span by offset and length, all read from the file |

`fuzz_project` saves what it loaded back out, because a session that opens and
will not save is the bug that has actually happened here.

## Running it

Each target builds twice.

```sh
make fuzz-replay          # every corpus entry, under ASan and UBSan, any compiler
make fuzz-run             # go looking, 60s a target; needs clang for libFuzzer
make fuzz-run FUZZ_SECONDS=600
```

`fuzz-replay` is part of `make check` and runs in CI on every change. It needs
no clang, which is the point: an input that crashed once and was fixed should be
a test from then on, not something rediscovered by luck.

`fuzz-run` writes anything it finds into `corpus/<target>/`, where the replay
picks it up from the next build onward. CI runs it for two minutes a target and
uploads the corpus if it fails.

## The corpus

Seeds are committed. Each one is either a shape the parser must accept - a plain
PCM take, an RF64 file, a stamped take, a full session - or one it must refuse
without reaching past the end of anything: a truncated header, a chunk claiming
more bytes than the file has, a clip whose source is not in the table.

Keep them small. A fuzzer runs the whole corpus every pass, and a seed that adds
nothing but length costs every run after it.
