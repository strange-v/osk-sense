import { readFileSync } from 'node:fs'
import { fileURLToPath, URL } from 'node:url'

// The Web UI ships in the same release as the firmware and carries its version.
// The gateway serves an image only when its major.minor matches, because the
// UI depends on /ui/*, which moves with the firmware.
const source = readFileSync(
  fileURLToPath(new URL('../include/FirmwareVersion.h', import.meta.url)),
  'utf8',
)
const match = /version\[\]\s*=\s*"(\d+)\.(\d+)\.(\d+)"/.exec(source)
if (!match) throw new Error('FirmwareVersion.h has no major.minor.patch version')

export const firmwareVersion = `${match[1]}.${match[2]}.${match[3]}`
export const firmwareSeries = `${match[1]}.${match[2]}`
