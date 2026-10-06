import { useState, type CSSProperties } from 'react';
import { Box, Text } from '@radix-ui/themes';

/*
 * A one-series trend line for a stat tile: 2px line in the series blue,
 * a soft area under it, the latest point marked. Hovering shows the
 * value under the pointer and how long ago it was (the dataviz rule:
 * an HTML chart is interactive by default). One series, so no legend:
 * the tile's label names it.
 */
export function Sparkline({
  values,
  every,
  unit,
  digits = 1,
  label,
  minSpan = 0,
  floor,
}: {
  values: number[];
  every: number; // seconds between samples
  unit: string;
  digits?: number;
  label: string;
  // The smallest range the chart shows, so half a degree of noise does
  // not fill the tile like a heatwave; and an optional fixed bottom.
  minSpan?: number;
  floor?: number;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const n = values.length;
  if (n < 2) return <Box className="wv-spark" />;
  let lo = Math.min(...values);
  let hi = Math.max(...values);
  if (floor !== undefined) lo = floor;
  if (hi - lo < minSpan) {
    const mid = floor !== undefined ? lo + minSpan / 2 : (hi + lo) / 2;
    lo = mid - minSpan / 2;
    hi = mid + minSpan / 2;
  }
  if (hi - lo < 1e-9) {
    lo -= 1;
    hi += 1;
  }
  const W = 100;
  const H = 40;
  const pad = 4;
  const x = (i: number) => (i / (n - 1)) * W;
  const y = (v: number) => pad + (1 - (v - lo) / (hi - lo)) * (H - 2 * pad);
  const line = values.map((v, i) => `${i ? 'L' : 'M'}${x(i).toFixed(2)},${y(v).toFixed(2)}`).join('');
  const area = `${line}L${W},${H}L0,${H}Z`;
  const at = hover ?? n - 1;
  const ago = Math.round((n - 1 - at) * every);
  const tipVars = { '--wv-tip-x': `${(at / (n - 1)) * 100}%` } as CSSProperties;

  return (
    <Box
      className="wv-spark"
      onPointerMove={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        const i = Math.round(((e.clientX - r.left) / r.width) * (n - 1));
        setHover(Math.max(0, Math.min(n - 1, i)));
      }}
      onPointerLeave={() => setHover(null)}
    >
      <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" role="img"
        aria-label={`${label}, last ${Math.round((n - 1) * every / 60)} minutes: ` +
          `${lo.toFixed(digits)} to ${hi.toFixed(digits)} ${unit}`}>
        <path d={area} fill="var(--wv-viz-fill)" stroke="none" />
        <path d={line} fill="none" stroke="var(--wv-viz-line)"
          strokeWidth="var(--wv-spark-stroke)" vectorEffect="non-scaling-stroke"
          strokeLinejoin="round" strokeLinecap="round" />
        {hover !== null && (
          <line x1={x(at)} x2={x(at)} y1={0} y2={H} stroke="var(--wv-viz-guide)"
            strokeWidth="1" vectorEffect="non-scaling-stroke" />
        )}
      </svg>
      <Box className="wv-spark-dot" style={{
        '--wv-dot-x': `${(at / (n - 1)) * 100}%`,
        '--wv-dot-y': `${(y(values[at]) / H) * 100}%`,
      } as CSSProperties} />
      {hover !== null && (
        <Box className="wv-spark-tip" style={tipVars}>
          <Text size="1" weight="bold">{values[at].toFixed(digits)} {unit}</Text>
          <Text size="1" color="gray"> {ago ? `${ago} s ago` : 'now'}</Text>
        </Box>
      )}
    </Box>
  );
}
