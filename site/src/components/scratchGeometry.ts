export type BlockKind = 'stack' | 'hat' | 'cap' | 'c' | 'reporter' | 'boolean' | 'field';

const finite = (value: number, fallback: number) => Number.isFinite(value) && value > 0 ? value : fallback;
const n = (value: number) => Number(value.toFixed(2));

/** Translate our absolute M/L/H/V/Q/C/Z paths so a script can be stroked once. */
export function translatePath(path: string, dx: number, dy: number): string {
  const tokens = path.match(/[MLHVQCZ]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:e[-+]?\d+)?/gi) ?? [];
  let command = '';
  let index = 0;
  return tokens.map(token => {
    if (/^[MLHVQCZ]$/.test(token)) { command = token; index = 0; return token; }
    const value = Number(token);
    const offset = command === 'H' ? dx : command === 'V' ? dy : index % 2 === 0 ? dx : dy;
    index++;
    return String(n(value + offset));
  }).join(' ');
}

/** One continuous outline, in CSS pixels. Only the straight spans stretch. */
export function blockPath(kind: BlockKind, width: number, height: number, headHeight = 46): string {
  const w = n(Math.max(16, finite(width, 240)));
  const h = n(Math.max(16, finite(height, 48)));
  const right = w - 1;
  const bottom = h - 1;
  const roundRect = (radius: number) => {
    const r = n(Math.min(radius, (w - 2) / 2, (h - 2) / 2));
    return `M${1 + r} 1H${right - r}Q${right} 1 ${right} ${1 + r}V${bottom - r}Q${right} ${bottom} ${right - r} ${bottom}H${1 + r}Q1 ${bottom} 1 ${bottom - r}V${1 + r}Q1 1 ${1 + r} 1Z`;
  };
  if (kind === 'reporter') return roundRect(h / 2);
  if (kind === 'field') return roundRect(5);
  if (kind === 'boolean') {
    const point = n(Math.min((h - 2) / 2, (w - 2) / 3));
    return `M${1 + point} 1H${right - point}L${right} ${h / 2}L${right - point} ${bottom}H${1 + point}L1 ${h / 2}Z`;
  }
  // Tiny containers retain a valid outline instead of intersecting the notch.
  if (w < 80 || h < 30) return roundRect(4);
  const top = 'M5 1H18Q20 1 22 3L24 5Q25 7 28 7H40Q43 7 44 5L46 3Q48 1 50 1';
  const lower = (y: number) => `H50Q48 ${y} 46 ${y + 2}L44 ${y + 4}Q43 ${y + 6} 40 ${y + 6}H28Q25 ${y + 6} 24 ${y + 4}L22 ${y + 2}Q20 ${y} 18 ${y}H5Q1 ${y} 1 ${y - 4}`;
  if (kind === 'hat') {
    return `M1 22C16 1 46 -2 74 12H${w - 5}Q${right} 12 ${right} 16V${h - 11}Q${right} ${h - 7} ${w - 5} ${h - 7}${lower(h - 7)}V22Z`;
  }
  if (kind === 'cap') {
    return `${top}H${w - 5}Q${right} 1 ${right} 5V${h - 11}Q${right} ${bottom} ${w - 11} ${bottom}H11Q1 ${bottom} 1 ${h - 11}V5Q1 1 5 1Z`;
  }
  if (kind === 'c' && h >= 70) {
    const head = n(Math.min(h - 35, Math.max(24, finite(headHeight, 46) - 7)));
    const floor = h - 25;
    const rail = 19;
    // The two inner notches share the same coordinates as the nested stack.
    return `${top}H${w - 5}Q${right} 1 ${right} 5V${head - 4}Q${right} ${head} ${w - 5} ${head}H${rail + 49}Q${rail + 47} ${head} ${rail + 45} ${head + 2}L${rail + 43} ${head + 4}Q${rail + 42} ${head + 6} ${rail + 39} ${head + 6}H${rail + 27}Q${rail + 24} ${head + 6} ${rail + 23} ${head + 4}L${rail + 21} ${head + 2}Q${rail + 19} ${head} ${rail + 17} ${head}H${rail + 4}Q${rail} ${head} ${rail} ${head + 4}V${floor - 4}Q${rail} ${floor} ${rail + 4} ${floor}H${rail + 17}Q${rail + 19} ${floor} ${rail + 21} ${floor + 2}L${rail + 23} ${floor + 4}Q${rail + 24} ${floor + 6} ${rail + 27} ${floor + 6}H${rail + 39}Q${rail + 42} ${floor + 6} ${rail + 43} ${floor + 4}L${rail + 45} ${floor + 2}Q${rail + 47} ${floor} ${rail + 49} ${floor}H${w - 5}Q${right} ${floor} ${right} ${floor + 4}V${h - 11}Q${right} ${h - 7} ${w - 5} ${h - 7}${lower(h - 7)}V5Q1 1 5 1Z`;
  }
  return `${top}H${w - 5}Q${right} 1 ${right} 5V${h - 11}Q${right} ${h - 7} ${w - 5} ${h - 7}${lower(h - 7)}V5Q1 1 5 1Z`;
}
