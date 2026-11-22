# xsv-cpp advantages animation

Remotion source for the 24-second README animation. It is data-driven from the
final D4 and standard-profile results; it deliberately ends on the caveat that
Rust still wins search and stats.

- Composition: `XsvCppAdvantages`
- 1280×720, 30 fps, 24 s, no audio
- Source: `src/XsvCppAdvantages.tsx`
- README asset: `docs/assets/xsv-cpp-advantages.gif`

```sh
npm install
npm run dev       # Remotion Studio
npm run render    # out/xsv-cpp-advantages.mp4
```

Render stills for layout review:

```sh
npx remotion still src/index.ts XsvCppAdvantages qa/wins.png --frame=250 \
  --browser-executable=/usr/bin/google-chrome
```

Convert the rendered MP4 to the README GIF (840 px, 15 fps, 128-colour palette):

```sh
ffmpeg -y -i out/xsv-cpp-advantages.mp4 \
  -vf "fps=15,scale=840:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=4" \
  -loop 0 ../docs/assets/xsv-cpp-advantages.gif
```

Numbers: `results/d4/timing.jsonl` (D4, n=3) and the formal standard run in
`results/runs/20261120T102645Z-formal/formal/timing.jsonl` (join and mechanism
ablations). The report is `report/REPORT.md`.
