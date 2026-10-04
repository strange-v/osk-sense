import { readFileSync } from 'node:fs'

const path = process.argv[2] ?? new URL('entropy-capture-samples.txt', import.meta.url)
const samples = readFileSync(path, 'utf8').trim().split(/\s+/).map(Number)
if (samples.length < 1024 || samples.some((value) => !Number.isInteger(value) || value < 0 || value > 65535)) {
  throw new Error('Need at least 1024 unsigned 16-bit timer samples')
}
const bits = samples.map((value) => value & 1)
const ones = bits.reduce((sum, bit) => sum + bit, 0)
const histogram = {}
for (const value of samples) histogram[value] = (histogram[value] ?? 0) + 1
let longestRun = 1, run = 1, transitions = 0
for (let i = 1; i < bits.length; i++) {
  if (bits[i] !== bits[i - 1]) { transitions++; run = 1 }
  else { run++; longestRun = Math.max(longestRun, run) }
}
const correlations = []
for (let lag = 1; lag <= 32; lag++) {
  let product = 0
  for (let i = lag; i < bits.length; i++) product += (2 * bits[i] - 1) * (2 * bits[i - lag] - 1)
  correlations.push(product / (bits.length - lag))
}
const extracted = []
for (let i = 0; i + 1 < bits.length; i += 2) {
  if (bits[i] !== bits[i + 1]) extracted.push(bits[i])
}
console.log(JSON.stringify({
  samples: samples.length, histogram, lsb_ones: ones, lsb_transitions: transitions,
  lsb_longest_run: longestRun, lsb_correlations_lags_1_to_32: correlations,
  von_neumann_bits: extracted.length, von_neumann_ones: extracted.reduce((sum, bit) => sum + bit, 0),
}, null, 2))
