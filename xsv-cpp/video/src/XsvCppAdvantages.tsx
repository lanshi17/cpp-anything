import {
  AbsoluteFill,
  interpolate,
  spring,
  useCurrentFrame,
  useVideoConfig,
} from "remotion";

const BG = "#070b14";
const PANEL = "rgba(15, 23, 42, 0.78)";
const BORDER = "rgba(148, 163, 184, 0.18)";
const TEXT = "#e2e8f0";
const MUTED = "#94a3b8";
const CYAN = "#22d3ee";
const GREEN = "#34d399";
const RED = "#fb7185";
const AMBER = "#fbbf24";
const MONO = '"JetBrains Mono", "DejaVu Sans Mono", "Liberation Mono", monospace';
const SANS = '"Inter", "DejaVu Sans", "Liberation Sans", sans-serif';

const CLAMP = {
  extrapolateLeft: "clamp" as const,
  extrapolateRight: "clamp" as const,
};

type Tone = "win" | "loss";

type Metric = {
  name: string;
  scope: string;
  ratio: number;
  cpp: string;
  rust: string;
};

const WINS: Metric[] = [
  {name: "count", scope: "100M rows", ratio: 0.751, cpp: "25.6 s", rust: "34.1 s"},
  {name: "index", scope: "100M rows", ratio: 0.853, cpp: "28.9 s", rust: "33.9 s"},
  {name: "select", scope: "100M rows", ratio: 0.895, cpp: "32.3 s", rust: "36.1 s"},
  {name: "slice", scope: "indexed", ratio: 0.651, cpp: "2.3 ms", rust: "3.5 ms"},
  {name: "join", scope: "1M × 100k", ratio: 0.591, cpp: "0.196 s", rust: "0.338 s"},
];

const LOSSES: Metric[] = [
  {name: "search", scope: "100M rows", ratio: 1.83, cpp: "101.3 s", rust: "55.4 s"},
  {name: "stats", scope: "100M rows", ratio: 2.298, cpp: "29.7 s", rust: "12.9 s"},
];

const ramp = (frame: number, start: number, duration = 16) =>
  interpolate(frame, [start, start + duration], [0, 1], CLAMP);

const fadeOut = (frame: number, end: number, duration = 14) =>
  interpolate(frame, [end - duration, end], [1, 0], CLAMP);

const Grid = () => (
  <>
    <AbsoluteFill
      style={{
        backgroundColor: BG,
        backgroundImage:
          "linear-gradient(rgba(34, 211, 238, 0.055) 1px, transparent 1px), linear-gradient(90deg, rgba(34, 211, 238, 0.055) 1px, transparent 1px)",
        backgroundSize: "48px 48px",
      }}
    />
    <AbsoluteFill
      style={{
        background:
          "radial-gradient(circle at 18% 12%, rgba(34, 211, 238, 0.14), transparent 32%), radial-gradient(circle at 86% 78%, rgba(52, 211, 153, 0.10), transparent 34%)",
      }}
    />
    <AbsoluteFill
      style={{
        background:
          "linear-gradient(180deg, rgba(7, 11, 20, 0.0) 0%, rgba(7, 11, 20, 0.82) 100%)",
      }}
    />
  </>
);

const Header = ({frame}: {frame: number}) => {
  const opacity = ramp(frame, 0, 20) * fadeOut(frame, 720, 30);
  return (
    <div
      style={{
        position: "absolute",
        top: 28,
        left: 64,
        right: 64,
        display: "flex",
        justifyContent: "space-between",
        alignItems: "center",
        opacity,
        fontFamily: MONO,
        fontSize: 13,
        letterSpacing: 1.8,
        color: MUTED,
      }}
    >
      <span>
        <span style={{color: CYAN}}>XSV-CPP</span> / REAL REFACTOR
      </span>
      <span>100M ROWS · 40.4 GB · GCC 15.2</span>
    </div>
  );
};

const Title = ({frame}: {frame: number}) => {
  const {fps} = useVideoConfig();
  const enter = spring({
    frame: Math.max(0, frame),
    fps,
    config: {damping: 20, stiffness: 90},
  });
  const opacity = enter * fadeOut(frame, 88, 16);
  const y = interpolate(enter, [0, 1], [28, 0]);
  return (
    <div
      style={{
        position: "absolute",
        left: 64,
        right: 64,
        top: 178,
        opacity,
        transform: `translateY(${y}px)`,
      }}
    >
      <div
        style={{
          fontFamily: MONO,
          fontSize: 18,
          letterSpacing: 3.6,
          color: CYAN,
          marginBottom: 18,
        }}
      >
        xsv 0.13.0  →  C++23
      </div>
      <div
        style={{
          fontFamily: SANS,
          fontSize: 74,
          lineHeight: 1.02,
          letterSpacing: -2.6,
          fontWeight: 650,
          color: TEXT,
          maxWidth: 980,
        }}
      >
        Measured advantages,
        <br />
        not language slogans.
      </div>
      <div
        style={{
          marginTop: 24,
          fontFamily: SANS,
          fontSize: 22,
          color: MUTED,
          maxWidth: 840,
        }}
      >
        One real refactor. Seven commands. One honest caveat.
      </div>
      <div
        style={{
          display: "inline-flex",
          gap: 12,
          marginTop: 30,
          padding: "9px 14px",
          border: `1px solid ${BORDER}`,
          borderRadius: 999,
          fontFamily: MONO,
          fontSize: 12,
          letterSpacing: 1.2,
          color: AMBER,
          background: "rgba(251, 191, 36, 0.06)",
        }}
      >
        BASE = LEVERS OFF · ALL = LEVERS ON
      </div>
    </div>
  );
};

const Bar = ({
  label,
  width,
  color,
  track,
  labelWidth = 34,
}: {
  label: string;
  width: number;
  color: string;
  track: string;
  labelWidth?: number;
}) => (
  <div style={{display: "flex", alignItems: "center", gap: 8}}>
    <span
      style={{
        width: labelWidth,
        flex: `0 0 ${labelWidth}px`,
        fontFamily: MONO,
        fontSize: 10,
        color: MUTED,
      }}
    >
      {label}
    </span>
    <div
      style={{
        position: "relative",
        flex: 1,
        height: 9,
        borderRadius: 999,
        background: track,
        overflow: "hidden",
      }}
    >
      <div
        style={{
          width: `${width}%`,
          height: "100%",
          borderRadius: 999,
          background: color,
          boxShadow: `0 0 14px ${color}55`,
        }}
      />
    </div>
  </div>
);

const MetricRow = ({
  metric,
  index,
  frame,
  tone,
}: {
  metric: Metric;
  index: number;
  frame: number;
  tone: Tone;
}) => {
  const {fps} = useVideoConfig();
  const start = 118 + index * 19;
  const progress = spring({
    frame: Math.max(0, frame - start),
    fps,
    config: {damping: 22, stiffness: 95},
  });
  const scale = Math.max(1, metric.ratio);
  const cppWidth = (metric.ratio / scale) * 100 * progress;
  const rustWidth = (1 / scale) * 100 * progress;
  const color = tone === "win" ? CYAN : RED;
  const ratioColor = tone === "win" ? GREEN : RED;
  return (
    <div
      style={{
        display: "grid",
        gridTemplateColumns: "154px 1fr 216px",
        columnGap: 18,
        alignItems: "center",
        minHeight: 48,
        opacity: progress,
      }}
    >
      <div>
        <div
          style={{
            fontFamily: MONO,
            fontSize: 17,
            color: TEXT,
            letterSpacing: 0.4,
          }}
        >
          {metric.name}
        </div>
        <div
          style={{
            marginTop: 3,
            fontFamily: MONO,
            fontSize: 10,
            color: MUTED,
            letterSpacing: 0.8,
          }}
        >
          {metric.scope}
        </div>
      </div>
      <div style={{display: "flex", flexDirection: "column", gap: 5}}>
        <Bar label="C++" width={cppWidth} color={color} track="rgba(148,163,184,0.12)" />
        <Bar label="Rust" width={rustWidth} color="#64748b" track="rgba(148,163,184,0.12)" />
      </div>
      <div style={{textAlign: "right"}}>
        <div
          style={{
            fontFamily: MONO,
            fontSize: 24,
            fontWeight: 700,
            color: ratioColor,
            fontVariantNumeric: "tabular-nums",
            letterSpacing: -0.8,
          }}
        >
          {metric.ratio.toFixed(2)}×
        </div>
        <div
          style={{
            marginTop: 2,
            fontFamily: MONO,
            fontSize: 10,
            color: MUTED,
            fontVariantNumeric: "tabular-nums",
          }}
        >
          C++ {metric.cpp} / Rust {metric.rust}
        </div>
      </div>
    </div>
  );
};

const WinsPanel = ({frame}: {frame: number}) => {
  const {fps} = useVideoConfig();
  const inSpring = spring({
    frame: Math.max(0, frame - 82),
    fps,
    config: {damping: 24, stiffness: 90},
  });
  const opacity = inSpring * fadeOut(frame, 350, 16);
  const y = interpolate(inSpring, [0, 1], [22, 0]);
  return (
    <div
      style={{
        position: "absolute",
        top: 118,
        left: 64,
        right: 64,
        bottom: 62,
        padding: "26px 30px 22px",
        border: `1px solid ${BORDER}`,
        borderRadius: 22,
        background: PANEL,
        boxShadow: "0 26px 80px rgba(0,0,0,0.35)",
        opacity,
        transform: `translateY(${y}px)`,
        display: "flex",
        flexDirection: "column",
      }}
    >
      <div style={{display: "flex", justifyContent: "space-between", alignItems: "flex-start"}}>
        <div>
          <div
            style={{
              fontFamily: SANS,
              fontSize: 30,
              fontWeight: 650,
              color: TEXT,
              letterSpacing: -0.7,
            }}
          >
            Where C++ wins
          </div>
          <div style={{marginTop: 5, fontFamily: MONO, fontSize: 11, color: CYAN, letterSpacing: 1.5}}>
            BYTE-HEAVY SCANS + LAYOUT
          </div>
        </div>
        <div style={{fontFamily: MONO, fontSize: 11, color: MUTED, textAlign: "right", lineHeight: 1.8}}>
          <span style={{color: CYAN}}>● C++ all</span> &nbsp; <span>● Rust 0.13</span>
          <br />
          lower is faster
        </div>
      </div>

      <div style={{marginTop: 18, display: "flex", flexDirection: "column", gap: 7}}>
        {WINS.map((metric, index) => (
          <MetricRow key={metric.name} metric={metric} index={index} frame={frame} tone="win" />
        ))}
      </div>

      <div style={{display: "flex", gap: 8, marginTop: 15, flexWrap: "wrap"}}>
        {[
          ["mmap", "zero-copy"],
          ["std::pmr", "arenas"],
          ["AVX2", "scan"],
          ["oneTBB", "workers"],
          ["flat hash", "join"],
        ].map(([name, note], index) => {
          const appear = spring({
            frame: Math.max(0, frame - (286 + index * 5)),
            fps,
            config: {damping: 18, stiffness: 100},
          });
          return (
            <div
              key={name}
              style={{
                opacity: appear,
                transform: `translateY(${interpolate(appear, [0, 1], [10, 0])}px)`,
                display: "flex",
                gap: 7,
                alignItems: "baseline",
                padding: "6px 10px",
                border: "1px solid rgba(34,211,238,0.22)",
                borderRadius: 8,
                background: "rgba(34,211,238,0.055)",
                fontFamily: MONO,
              }}
            >
              <span style={{fontSize: 11, color: CYAN}}>{name}</span>
              <span style={{fontSize: 9, color: MUTED}}>{note}</span>
            </div>
          );
        })}
      </div>

      <div
        style={{
          marginTop: "auto",
          paddingTop: 14,
          borderTop: `1px solid ${BORDER}`,
          display: "flex",
          justifyContent: "space-between",
          gap: 20,
          fontFamily: MONO,
          fontSize: 10,
          lineHeight: 1.6,
          color: MUTED,
        }}
      >
        <span>
          <span style={{color: CYAN}}>mmap axis over base:</span> 2.21× select · 125× indexed slice
        </span>
        <span style={{textAlign: "right"}}>
          D4 n=3 · join from standard profile · <span style={{color: TEXT}}>base</span> is the control
        </span>
      </div>
    </div>
  );
};

const LossesPanel = ({frame}: {frame: number}) => {
  const {fps} = useVideoConfig();
  const inSpring = spring({
    frame: Math.max(0, frame - 352),
    fps,
    config: {damping: 24, stiffness: 90},
  });
  const opacity = inSpring * fadeOut(frame, 510, 16);
  const y = interpolate(inSpring, [0, 1], [22, 0]);
  return (
    <div
      style={{
        position: "absolute",
        top: 118,
        left: 64,
        right: 64,
        bottom: 62,
        padding: "26px 30px 22px",
        border: `1px solid ${BORDER}`,
        borderRadius: 22,
        background: PANEL,
        boxShadow: "0 26px 80px rgba(0,0,0,0.35)",
        opacity,
        transform: `translateY(${y}px)`,
        display: "flex",
        flexDirection: "column",
      }}
    >
      <div style={{display: "flex", justifyContent: "space-between", alignItems: "flex-start"}}>
        <div>
          <div
            style={{
              fontFamily: SANS,
              fontSize: 30,
              fontWeight: 650,
              color: TEXT,
              letterSpacing: -0.7,
            }}
          >
            Where Rust wins
          </div>
          <div style={{marginTop: 5, fontFamily: MONO, fontSize: 11, color: RED, letterSpacing: 1.5}}>
            REGEX + STATISTICS
          </div>
        </div>
        <div style={{fontFamily: MONO, fontSize: 11, color: MUTED, textAlign: "right", lineHeight: 1.8}}>
          <span style={{color: RED}}>● C++ all</span> &nbsp; <span>● Rust 0.13</span>
          <br />
          lower is faster
        </div>
      </div>

      <div style={{marginTop: 24, display: "flex", flexDirection: "column", gap: 10}}>
        {LOSSES.map((metric, index) => (
          <MetricRow key={metric.name} metric={metric} index={index} frame={frame} tone="loss" />
        ))}
      </div>

      <div
        style={{
          marginTop: 24,
          padding: "12px 14px",
          borderLeft: `2px solid ${AMBER}`,
          background: "rgba(251,191,36,0.055)",
          fontFamily: MONO,
          fontSize: 11,
          lineHeight: 1.7,
          color: MUTED,
        }}
      >
        Rust still owns streaming memory: ~5 MiB vs ~250 MiB at 100M.
        <br />
        C++ stats regressed even after the mapped-page fix; the gap is computational.
      </div>

      <div
        style={{
          marginTop: "auto",
          paddingTop: 18,
          borderTop: `1px solid ${BORDER}`,
          display: "flex",
          justifyContent: "space-between",
          gap: 20,
          fontFamily: MONO,
          fontSize: 10,
          lineHeight: 1.7,
          color: MUTED,
        }}
      >
        <span>
          A regex/statistics gap, not a language verdict.
        </span>
        <span style={{textAlign: "right"}}>
          Both builds pass <span style={{color: GREEN}}>144/144</span> differential cases.
        </span>
      </div>
    </div>
  );
};

const MemoryPanel = ({frame}: {frame: number}) => {
  const {fps} = useVideoConfig();
  const enter = spring({
    frame: Math.max(0, frame - 512),
    fps,
    config: {damping: 22, stiffness: 90},
  });
  const opacity = enter * fadeOut(frame, 622, 16);
  const y = interpolate(enter, [0, 1], [24, 0]);
  const arrow = ramp(frame, 545, 18);
  return (
    <div
      style={{
        position: "absolute",
        top: 146,
        left: 130,
        right: 130,
        bottom: 92,
        padding: "34px 38px",
        border: `1px solid ${BORDER}`,
        borderRadius: 22,
        background: PANEL,
        opacity,
        transform: `translateY(${y}px)`,
        display: "flex",
        flexDirection: "column",
        justifyContent: "center",
      }}
    >
      <div style={{fontFamily: MONO, fontSize: 11, letterSpacing: 2, color: AMBER}}>
        C++ FIXES ITS OWN BASELINE
      </div>
      <div
        style={{
          marginTop: 13,
          fontFamily: SANS,
          fontSize: 42,
          fontWeight: 650,
          letterSpacing: -1.2,
          color: TEXT,
        }}
      >
        4.10 GB → 24.9 MB
      </div>
      <div style={{marginTop: 8, fontFamily: MONO, fontSize: 12, color: MUTED}}>
        10M-row count · base → all · mapped-page window
      </div>

      <div
        style={{
          display: "flex",
          alignItems: "center",
          gap: 18,
          marginTop: 28,
        }}
      >
        <div style={{minWidth: 150}}>
          <div style={{fontFamily: MONO, fontSize: 10, color: MUTED, letterSpacing: 1.2}}>C++ BASE</div>
          <div style={{marginTop: 4, fontFamily: MONO, fontSize: 20, color: AMBER}}>4.10 GB</div>
        </div>
        <div
          style={{
            flex: 1,
            height: 2,
            background: "rgba(148,163,184,0.22)",
            position: "relative",
          }}
        >
          <div
            style={{
              position: "absolute",
              left: 0,
              top: -1,
              height: 4,
              width: `${arrow * 100}%`,
              background: `linear-gradient(90deg, ${AMBER}, ${GREEN})`,
            }}
          />
        </div>
        <div style={{minWidth: 150, textAlign: "right"}}>
          <div style={{fontFamily: MONO, fontSize: 10, color: MUTED, letterSpacing: 1.2}}>C++ ALL</div>
          <div style={{marginTop: 4, fontFamily: MONO, fontSize: 20, color: GREEN}}>24.9 MB</div>
        </div>
      </div>

      <div
        style={{
          marginTop: 26,
          paddingTop: 18,
          borderTop: `1px solid ${BORDER}`,
          fontFamily: MONO,
          fontSize: 11,
          lineHeight: 1.7,
          color: MUTED,
        }}
      >
        Rust at the same point: <span style={{color: TEXT}}>5.1 MB</span>. This is
        allocator and mapping control, not a memory win over Rust.
      </div>
    </div>
  );
};

const Outro = ({frame}: {frame: number}) => {
  const {fps} = useVideoConfig();
  const enter = spring({
    frame: Math.max(0, frame - 624),
    fps,
    config: {damping: 20, stiffness: 90},
  });
  const opacity = enter;
  const y = interpolate(enter, [0, 1], [26, 0]);
  return (
    <div
      style={{
        position: "absolute",
        left: 64,
        right: 64,
        top: 248,
        textAlign: "center",
        opacity,
        transform: `translateY(${y}px)`,
      }}
    >
      <div
        style={{
          fontFamily: SANS,
          fontSize: 58,
          fontWeight: 650,
          letterSpacing: -2,
          color: TEXT,
        }}
      >
        Implementation wins.
      </div>
      <div
        style={{
          marginTop: 10,
          fontFamily: SANS,
          fontSize: 34,
          fontWeight: 500,
          letterSpacing: -0.8,
          color: CYAN,
        }}
      >
        Not a language verdict.
      </div>
      <div
        style={{
          marginTop: 36,
          fontFamily: MONO,
          fontSize: 12,
          letterSpacing: 1.2,
          color: MUTED,
        }}
      >
        xsv-cpp · 144/144 differential · report/REPORT.md
      </div>
    </div>
  );
};

export const XsvCppAdvantages = () => {
  const frame = useCurrentFrame();
  return (
    <AbsoluteFill style={{backgroundColor: BG, color: TEXT, fontFamily: MONO}}>
      <Grid />
      <Header frame={frame} />
      <Title frame={frame} />
      <WinsPanel frame={frame} />
      <LossesPanel frame={frame} />
      <MemoryPanel frame={frame} />
      <Outro frame={frame} />
    </AbsoluteFill>
  );
};
