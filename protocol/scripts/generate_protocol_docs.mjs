import { mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const protocolRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const generatedDirectory = resolve(protocolRoot, 'generated')
const manifest = JSON.parse(readFileSync(resolve(protocolRoot, 'protocol-manifest.json'), 'utf8'))

const dimensions = {
  uint8: 1,
  int8: 1,
  int16_le: 2,
  uint16_le: 2,
  uint32_le: 4,
  uint64_le: 8,
}

const encodingLabels = {
  constant: 'constant',
  uint8: 'uint8',
  int8: 'int8',
  int16_le: 'int16 LE',
  uint16_le: 'uint16 LE',
  uint32_le: 'uint32 LE',
  uint64_le: 'uint64 LE',
  bytes: 'bytes',
}

const shortNames = {
  stream_version: 'version',
  message_kind: 'kind',
  sequence: 'sequence',
  gateway_id: 'gateway ID',
  boot_id: 'boot ID',
  registry_generation: 'registry gen',
  node_id: 'node',
  profile_id: 'profile',
  received_at_unix_ms: 'received at',
  payload_size: 'size',
  supply_voltage: 'VCC',
  radio_state: 'radio',
  downlink_rssi: 'downlink',
  temperature: 'temperature',
  humidity: 'humidity',
  pressure: 'pressure',
  state: 'state',
  count: 'count',
  payload: 'payload',
}

function escapeXml(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
}

function displayName(name) {
  return shortNames[name] ?? name.replaceAll('_', ' ')
}

function fieldLength(field, variableLength = 5) {
  if (field.encoding !== 'bytes') return dimensions[field.encoding]
  return field.length ?? variableLength
}

function fieldClass(field) {
  return field.const_raw !== undefined ? 'constant' : field.encoding
}

function byteRange(offset, length, variable = false) {
  if (variable) return `${offset}…`
  return length === 1 ? `${offset}` : `${offset}–${offset + length - 1}`
}

function documentStart(width, height, title, description) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" role="img" aria-labelledby="title description">
  <title id="title">${escapeXml(title)}</title>
  <desc id="description">${escapeXml(description)}</desc>
  <defs>
    <pattern id="unused" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
      <line x1="0" y1="0" x2="0" y2="8" class="unused-line" />
    </pattern>
  </defs>
  <style>
    :root { color-scheme: light dark; }
    text { font-family: system-ui, -apple-system, "Segoe UI", sans-serif; fill: #172033; }
    .title { font-size: 22px; font-weight: 600; }
    .axis, .secondary { font-size: 12px; fill: #596579; }
    .row-name { font-size: 14px; font-weight: 600; }
    .field-name { font-size: 13px; font-weight: 600; }
    .field-type { font-size: 11px; fill: #3f4b5e; }
    .cell { stroke: #ffffff; stroke-width: 2; }
    .constant { fill: #d8dee8; }
    .uint8 { fill: #fde0a6; }
    .int8 { fill: #e3ecb8; }
    .uint16_le { fill: #bfe7fa; }
    .int16_le { fill: #bfead8; }
    .uint32_le { fill: #ddcdf9; }
    .uint64_le { fill: #ffd0b5; }
    .bytes { fill: #f7c9df; }
    .unused { fill: url(#unused); stroke: #d8dee8; stroke-width: 1; }
    .unused-line { stroke: #c5cedb; stroke-width: 1; }
    .legend-label { font-size: 12px; fill: #596579; }
    @media (prefers-color-scheme: dark) {
      text { fill: #eef2f7; }
      .axis, .secondary, .field-type, .legend-label { fill: #b8c2d1; }
      .cell { stroke: #111827; }
      .constant { fill: #465268; }
      .uint8 { fill: #75571c; }
      .int8 { fill: #5a6424; }
      .uint16_le { fill: #185b78; }
      .int16_le { fill: #23664e; }
      .uint32_le { fill: #59418b; }
      .uint64_le { fill: #844429; }
      .bytes { fill: #7a3459; }
      .unused { stroke: #465268; }
      .unused-line { stroke: #465268; }
    }
  </style>`
}

function renderField(field, x, y, width, height, offset, length, variable = false) {
  const cssClass = fieldClass(field)
  const compactNames = {
    stream_version: 'ver',
    message_kind: 'kind',
    node_id: 'node',
    payload_size: 'size',
  }
  const compact = width < 54
  const name = compact
    ? (compactNames[field.name] ?? displayName(field.name))
    : displayName(field.name)
  const type =
    field.const_raw !== undefined
      ? field.name === 'header'
        ? `0x${field.const_raw.toString(16).toUpperCase()}`
        : `${field.const_raw}`
      : encodingLabels[field.encoding]
  const range = byteRange(offset, length, variable)
  if (compact) {
    return `  <g>
    <rect class="cell ${cssClass}" x="${x}" y="${y}" width="${width}" height="${height}" />
    <text class="field-name" x="${x + width / 2}" y="${y + 25}" text-anchor="middle">${escapeXml(name)}</text>
    <text class="field-type" x="${x + width / 2}" y="${y + height - 9}" text-anchor="middle">${escapeXml(range)}</text>
  </g>`
  }
  return `  <g>
    <rect class="cell ${cssClass}" x="${x}" y="${y}" width="${width}" height="${height}" />
    <text class="field-name" x="${x + width / 2}" y="${y + 20}" text-anchor="middle">${escapeXml(name)}</text>
    <text class="field-type" x="${x + width / 2}" y="${y + 37}" text-anchor="middle">${escapeXml(type)}</text>
    <text class="field-type" x="${x + width / 2}" y="${y + height - 8}" text-anchor="middle">byte ${escapeXml(range)}</text>
  </g>`
}

function renderLegend(x, y) {
  const entries = [
    ['constant', 'constant'],
    ['uint8', 'uint8'],
    ['int8', 'int8'],
    ['uint16_le', 'uint16 LE'],
    ['int16_le', 'int16 LE'],
    ['uint32_le', 'uint32 LE'],
    ['uint64_le', 'uint64 LE'],
    ['bytes', 'bytes'],
  ]
  let cursor = x
  return entries
    .map(([cssClass, label]) => {
      const result = `  <rect class="${cssClass}" x="${cursor}" y="${y}" width="18" height="12" />
  <text class="legend-label" x="${cursor + 25}" y="${y + 11}">${label}</text>`
      cursor += 42 + label.length * 7
      return result
    })
    .join('\n')
}

function telemetrySvg() {
  const telemetry = manifest.telemetry
  const profiles = telemetry.profiles
  const maxSize = Math.max(...profiles.map((profile) => profile.frame_size))
  const margin = 24
  const labelWidth = 190
  const byteWidth = 104
  const rowHeight = 70
  const cellHeight = 62
  const gridX = margin + labelWidth
  const firstRowY = 82
  const width = gridX + maxSize * byteWidth + margin
  const height = firstRowY + profiles.length * rowHeight + 62
  const parts = [
    documentStart(
      width,
      height,
      'Telemetry profile byte map',
      'Rows are telemetry profiles and columns are application-frame byte offsets. Colors encode field representation.',
    ),
  ]
  parts.push(`  <text class="title" x="${margin}" y="30">Telemetry profile byte map</text>`)
  parts.push(`  <text class="axis" x="${margin}" y="65">profile</text>`)
  for (let offset = 0; offset < maxSize; ++offset) {
    parts.push(
      `  <text class="axis" x="${gridX + (offset + 0.5) * byteWidth}" y="65" text-anchor="middle">${offset}</text>`,
    )
  }

  for (const [index, profile] of profiles.entries()) {
    const y = firstRowY + index * rowHeight
    parts.push(
      `  <text class="row-name" x="${margin}" y="${y + 22}">${profile.id} · ${escapeXml(profile.name)}</text>`,
    )
    parts.push(
      `  <text class="secondary" x="${margin}" y="${y + 41}">${profile.frame_size} bytes</text>`,
    )
    const fields = [
      { name: 'header', offset: 0, encoding: 'uint8', const_raw: telemetry.header },
      ...telemetry.common_fields,
      ...profile.fields,
    ].sort((left, right) => left.offset - right.offset)
    for (const field of fields) {
      const length = fieldLength(field)
      parts.push(
        renderField(
          field,
          gridX + field.offset * byteWidth,
          y,
          length * byteWidth,
          cellHeight,
          field.offset,
          length,
        ),
      )
    }
    if (profile.frame_size < maxSize) {
      parts.push(
        `  <rect class="unused" x="${gridX + profile.frame_size * byteWidth}" y="${y}" width="${(maxSize - profile.frame_size) * byteWidth}" height="${cellHeight}" />`,
      )
    }
  }
  parts.push(renderLegend(margin, height - 32))
  parts.push('</svg>')
  return `${parts.join('\n')}\n`
}

function gatewayStreamSvg() {
  const stream = manifest.gateway_stream
  const messages = stream.messages
  const margin = 24
  const labelWidth = 190
  const byteWidth = 36
  const variablePreviewBytes = 5
  const rowHeight = 84
  const cellHeight = 62
  const gridX = margin + labelWidth
  const firstRowY = 58
  const longest = Math.max(
    ...messages.map((message) => message.size.fixed ?? message.size.base + variablePreviewBytes),
  )
  const width = gridX + longest * byteWidth + margin
  const height = firstRowY + messages.length * rowHeight + 62
  const parts = [
    documentStart(
      width,
      height,
      'Gateway stream message layouts',
      'The five version-one WebSocket messages are shown as proportional byte fields. The telemetry payload is variable length.',
    ),
  ]
  parts.push(`  <text class="title" x="${margin}" y="30">Gateway stream v${stream.version}</text>`)

  for (const [index, message] of messages.entries()) {
    const y = firstRowY + index * rowHeight
    const sizeLabel =
      message.size.fixed !== undefined
        ? `${message.size.fixed} bytes`
        : `${message.size.base} + payload bytes`
    parts.push(
      `  <text class="row-name" x="${margin}" y="${y + 24}">${message.kind} · ${escapeXml(message.name)}</text>`,
    )
    parts.push(`  <text class="secondary" x="${margin}" y="${y + 43}">${sizeLabel}</text>`)
    const fields = [
      { ...stream.common_fields[0], const_raw: stream.version },
      { ...stream.common_fields[1], const_raw: message.kind },
      ...stream.common_fields.slice(2),
      ...message.fields,
    ].sort((left, right) => left.offset - right.offset)
    for (const field of fields) {
      const variable = field.length_from !== undefined
      const length = fieldLength(field, variablePreviewBytes)
      parts.push(
        renderField(
          field,
          gridX + field.offset * byteWidth,
          y,
          length * byteWidth,
          cellHeight,
          field.offset,
          length,
          variable,
        ),
      )
    }
  }
  parts.push(renderLegend(margin, height - 32))
  parts.push('</svg>')
  return `${parts.join('\n')}\n`
}

function writeIfChanged(name, content) {
  const path = resolve(generatedDirectory, name)
  let previous
  try {
    previous = readFileSync(path, 'utf8')
  } catch (error) {
    if (error.code !== 'ENOENT') throw error
  }
  if (previous === content) return false
  writeFileSync(path, content, 'utf8')
  return true
}

mkdirSync(generatedDirectory, { recursive: true })
const changed = [
  writeIfChanged('telemetry-profiles.svg', telemetrySvg()),
  writeIfChanged('gateway-stream.svg', gatewayStreamSvg()),
].filter(Boolean).length
console.log(
  changed === 0 ? 'Protocol diagrams are up to date.' : `Updated ${changed} protocol diagram(s).`,
)
