# Performance

Measured 2026-11-12 with Rekarina 1.0.0 at 2560x1440, by `tools/bench_oot.py` (its header says how a run is made).

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 5070, 12227 MiB, 610.88 |
| CPU | AMD Ryzen 7 9700X 8-Core Processor |
| Memory | 62 GB |
| System | Microsoft Windows 11 Pro |

| Location | Mode | Mean fps | 1 % low | Shown on screen | Frames over 25 ms |
|---|---|---|---|---|---|
| The forest | Native | 98.8 | 63.5 | the same | 0 of 5928 |
| The forest | DLSS Quality + frame generation | 79.6 | 58.0 | 159 (1 % low 116) | 0 of 4778 |
| The field | Native | 101.3 | 67.8 | the same | 0 of 6077 |
| The field | DLSS Quality + frame generation | 79.0 | 53.3 | 158 (1 % low 107) | 2 of 4743 |
| The Market | Native | 110.8 | 65.6 | the same | 0 of 6650 |
| The Market | DLSS Quality + frame generation | 82.5 | 48.2 | 165 (1 % low 96) | 3 of 4953 |

## Measuring on your machine

Make a plan file as the header of `tools/bench_oot.py` shows (a copy of your profile folder per location and an input script that loads the save and turns the camera), then:

```
python tools/bench_oot.py --game <game folder> --plan plan.json --out results.json
python tools/make_perf_charts.py results.json
```

Close other programs that use the GPU first: the tool refuses to start while the GPU is busy.
