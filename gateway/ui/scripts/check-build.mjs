import { readdirSync, readFileSync, statSync } from 'node:fs'
import { basename, join, resolve } from 'node:path'

const output = resolve(import.meta.dirname, '../../data')
// `app` is the bundle every page load pays for; `deferred` covers the chunks
// only some sessions fetch (today: the pairing QR scanner with its jsQR
// dependency at ~48 KB gzip, and the administration page at ~5 KB). They are
// budgeted separately so a rarely used feature cannot quietly slow down the
// first paint -- jsQR alone is over half the size of the whole application and
// would not fit under the `app` limit at all.
const limits = { app: 90 * 1024, deferred: 56 * 1024, css: 20 * 1024, total: 250 * 1024 }
const files = readdirSync(output, { recursive: true })
  .map((name) => join(output, name))
  .filter((name) => statSync(name).isFile())
let total = 0
const compressed = { app: 0, deferred: 0, css: 0 }
for (const file of files) {
  const bytes = readFileSync(file)
  total += bytes.length
  if (file.endsWith('.js.gz'))
    compressed[basename(file).startsWith('app-') ? 'app' : 'deferred'] += bytes.length
  if (file.endsWith('.css.gz')) compressed.css += bytes.length
}
const failures = []
for (const kind of ['app', 'deferred', 'css'])
  if (compressed[kind] > limits[kind])
    failures.push(`${kind} gzip ${compressed[kind]} B exceeds ${limits[kind]} B`)
if (total > limits.total) failures.push(`total ${total} B exceeds ${limits.total} B`)
if (files.some((file) => ['.js', '.css', '.html'].some((extension) => file.endsWith(extension))))
  failures.push('uncompressed JS, CSS or HTML remains in the production image')
if (!compressed.app) failures.push('production image contains no compressed application JavaScript')
console.log(
  `Bundle: app JS ${compressed.app} B gzip, deferred JS ${compressed.deferred} B gzip, CSS ${compressed.css} B gzip, LittleFS total ${total} B`,
)
if (failures.length) throw new Error(`Bundle budget exceeded: ${failures.join('; ')}`)
