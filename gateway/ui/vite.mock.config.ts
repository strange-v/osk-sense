// Throwaway dev config used only to preview styling without a real gateway.
// Serves canned /api, /ui and /health responses. Not part of the build.
import vue from '@vitejs/plugin-vue'
import { defineConfig, type Plugin } from 'vite'
import { firmwareSeries, firmwareVersion } from './firmwareVersion'
import { createCipheriv, createDecipheriv, pbkdf2Sync, randomBytes } from 'node:crypto'

const now = Date.now()

const health = {
  status: 'ok', firmware: '2.1.0', api_version: 1, board: 'esp32-poe', hostname: 'osk-hub-a085e3',
  gateway_id: 'a1b2c3d4e5f60718293a4b5c6d7e8f90', boot_id: '00112233445566778899aabbccddeeff',
  reset_reason: 'power_on', uptime_seconds: 191_400, free_heap: 184_320,
  registry: { ready: true, records: 6, generation: 12 },
  setup: { required: false, active: false, remaining_seconds: 0 },
  pairing: { active: false, remaining_seconds: 0, indication: 'idle' },
  storage: {
    ready: true, settings_generation: 4, auth_generation: 2, secrets_generation: 1,
    nvs: { stats_available: true, used_entries: 455, free_entries: 553, available_entries: 427, total_entries: 1008, namespace_count: 5 },
  },
  ethernet: { state: 'connected', has_ip: true, ip: '192.168.1.42', mac: '50:ff:20:2e:fd:fe' },
  ota: { enabled: true, state: 'idle', progress: 0 },
  web_ui: { state: 'ok', version: firmwareVersion, required_firmware: firmwareSeries },
  telemetry: { nodes_seen: 5, updates: 14_207, last_node_id: 3, last_received_at_ms: now - 40_000 },
  time: { state: 'synchronized', unix_ms: now, last_sync_ms: now - 1_800_000 },
  websocket: { clients: 2, connections: 9, messages_sent: 2_140, messages_dropped: 0 },
  radio: {
    state: 'ready', present: true, version: 36, frequency_band_mhz: '868', frequency_hz: 868_000_000,
    bit_rate: 55_555, node_id: 1, network_id: 172, variant: 'RFM69HCW', configured_power_dbm: 14,
    encryption_enabled: true, profile: 'balanced', spi_host: 'VSPI', pins: {}, counters: { failed_tags: 2, replay_frames: 7, activation_challenges: 1 },
    last_packet: { at_ms: now - 40_000, sender_id: 3, rssi: -74 },
  },
}

// Radio fields as the gateway reports them: the ceiling and policy from the
// registry, the wanted level from its controller, and what the node's latest
// report says it uses. Тепличка has fallen back; Газ is still moving to the
// fixed level just set; Кухня has the protocol's highest ceiling, so its
// levels come as a list rather than side by side.
type MockNode = {
  node_id: number; device_uid: string; display_name: string; profile_id: number; firmware: string; state: string
  last_seen_at_ms?: number; rssi?: number; has_telemetry: boolean
  max_power_level: number; power_policy: string; fixed_power_level?: number; tx_power_target?: number
  tx_power_level?: number; radio_fallback?: boolean; downlink_rssi?: number; supply_mv?: number
}
const radio = (level: number, over: Partial<MockNode> = {}) => ({
  max_power_level: 2, power_policy: 'auto', tx_power_target: level, tx_power_level: level,
  radio_fallback: false, downlink_rssi: -66, supply_mv: 2987, ...over,
})
const nodes: MockNode[] = [
  { node_id: 2, device_uid: 'A1B2C3D4E5F60718293A', display_name: 'Кухня', profile_id: 1, firmware: '2.1.0', state: 'active', last_seen_at_ms: now - 120_000, rssi: -68, has_telemetry: true, ...radio(1, { max_power_level: 31, supply_mv: 4312 }) },
  { node_id: 3, device_uid: '0F1E2D3C4B5A69788796', display_name: 'Гараж', profile_id: 1, firmware: '2.1.0', state: 'active', last_seen_at_ms: now - 40_000, rssi: -74, has_telemetry: true, ...radio(2) },
  { node_id: 4, device_uid: '112233445566778899AA', display_name: '', profile_id: 2, firmware: '2.0.4', state: 'pending', has_telemetry: false, max_power_level: 2, power_policy: 'auto' },
  { node_id: 5, device_uid: 'BBCCDDEEFF0011223344', display_name: 'Тепличка', profile_id: 1, firmware: '2.1.0', state: 'active', last_seen_at_ms: now - 300_000, rssi: -89, has_telemetry: true, ...radio(2, { radio_fallback: true, downlink_rssi: -93, supply_mv: 2641 }) },
  { node_id: 6, device_uid: 'C0FFEE00112233445566', display_name: 'Газ', profile_id: 6, firmware: '0.1.0', state: 'active', last_seen_at_ms: now - 90_000, rssi: -71, has_telemetry: true, ...radio(2, { power_policy: 'fixed', fixed_power_level: 1, tx_power_target: 1 }) },
]

// The command book with the gateway's one-per-node rule. A queued command is
// delivered after 3 s and answered after 6 s, standing in for a node whose
// button was pressed; `set_count` reports the count it replaced and
// `read_info` the node's registry identity.
type MockCommand = { node_id: number; command_id: number; type: string; arguments: Record<string, number>; queued_at_ms: number; state: string; status?: string; completed_at_ms?: number; result?: Record<string, number | string> }
const commands: MockCommand[] = []
let nextCommandId = 4660
const commandTimers = new Map<number, ReturnType<typeof setTimeout>[]>()

function queueMockCommand(body: Record<string, unknown>) {
  const node = nodes.find((entry) => entry.node_id === body.node_id && entry.state === 'active')
  if (!node) return { status: 404, body: { error: 'node_not_found' } }
  const supported = body.type === 'read_info' || (body.type === 'set_count' && node.profile_id === 6)
  if (!supported) return { status: 422, body: { error: 'unsupported_command' } }
  const args = (body.arguments ?? {}) as Record<string, number>
  const valid = body.type === 'read_info'
    ? Object.keys(args).length === 0
    : Number.isInteger(args.count) && args.count >= 0 && args.count <= 0xffff_ffff
  if (!valid) return { status: 422, body: { error: 'invalid_command_arguments' } }
  const existing = commands.findIndex((entry) => entry.node_id === node.node_id)
  if (existing >= 0 && commands[existing].state !== 'completed') return { status: 409, body: { error: 'command_pending' } }
  if (existing >= 0) commands.splice(existing, 1)
  const command: MockCommand = { node_id: node.node_id, command_id: nextCommandId++, type: String(body.type), arguments: args, queued_at_ms: Date.now(), state: 'pending' }
  commands.push(command)
  commandTimers.set(node.node_id, [
    setTimeout(() => { command.state = 'delivered' }, 3_000),
    setTimeout(() => {
      Object.assign(command, { state: 'completed', status: 'applied', completed_at_ms: Date.now() })
      if (command.type === 'set_count') command.result = { previous_count: 1_207, count: args.count }
      if (command.type === 'read_info') {
        command.result = { profile_id: node.profile_id, firmware: node.firmware, max_power_level: node.max_power_level ?? 0 }
      }
    }, 6_000),
  ])
  return { status: 201, body: command }
}

const update = { state: 'idle', current_version: firmwareVersion, available_version: '', progress: 0, error: '', pending_verify: false }

const routes: Record<string, unknown> = {
  '/ui/status': health,
  '/api/info': {
    firmware_version: '2.1.0', api_version: 1, stream_version: 1,
    gateway_id: health.gateway_id, boot_id: health.boot_id, uptime_seconds: health.uptime_seconds,
    ui: { state: 'ok', version: firmwareVersion, required_firmware: firmwareSeries },
    board: 'esp32-poe', hostname: 'osk-hub-a085e3',
  },
  '/api/nodes': { registry_generation: 12, nodes },
  // One client that identified itself at the handshake, and one that did not,
  // so the card is exercised in both halves. The count in `health.websocket`
  // is what says how many there are; these only supply the names.
  '/ui/clients': { clients: [{ id: 3, name: 'home-assistant/0.3.0' }, { id: 4, name: '' }] },
  '/ui/setup': { setup_required: false, physical_window_active: false, remaining_seconds: 0 },
  '/ui/session': { user: { id: 1, username: 'admin', role: 'admin' }, csrf_token: 'mock-csrf' },
  '/ui/settings': { generation: 4, hostname: 'osk-hub-a085e3', mdns_enabled: true, ntp_enabled: true, pairing_window_seconds: 120, setup_window_seconds: 300, ntp_servers: ['pool.ntp.org'] },
  '/ui/pairing': { active: false, remaining_seconds: 0, indication: 'idle' },
}

let registryGeneration = 12

// Local accounts, with the same capacity and last-admin rule the gateway
// enforces, so the UI guardrails can actually be exercised here.
const users = [
  { id: 1, username: 'admin', role: 'admin', enabled: true },
  { id: 2, username: 'olena', role: 'viewer', enabled: true },
]
let nextUserId = 3

// API tokens, with the gateway's capacity and one-shot secret. These used to
// sit in the static `routes` map, which answered every method with the same
// list -- POST appeared to succeed while returning no `token` at all.
const tokens = [
  { id: 1, name: 'Home Assistant', enabled: true, created_at_ms: now - 86_400_000, scopes: ['telemetry:read'] },
]
let nextTokenId = 2
const TOKEN_SCOPES = ['telemetry:read']

function readBody(req: { on: (event: string, handler: (chunk?: unknown) => void) => void }, done: (body: Record<string, unknown>) => void) {
  let raw = ''
  req.on('data', (chunk) => { raw += chunk })
  req.on('end', () => done(JSON.parse(raw || '{}')))
}

function json(res: { statusCode: number; setHeader: (k: string, v: string) => void; end: (body?: string) => void }, status: number, body?: unknown) {
  res.statusCode = status
  if (body === undefined) { res.end(); return }
  res.setHeader('Content-Type', 'application/json')
  res.end(JSON.stringify(body))
}

const enabledAdmins = () => users.filter((user) => user.role === 'admin' && user.enabled).length
let pairingTimer: ReturnType<typeof setTimeout> | undefined

// Stands in for a node joining: the real gateway closes the window as soon as
// the JOIN_REQUEST lands. Pass ?fail=1 on the open request to rehearse the
// timeout path instead.
function openPairingWindow(deviceUid: string, succeed: boolean) {
  clearTimeout(pairingTimer)
  health.pairing = { active: true, remaining_seconds: 120, indication: 'pairing' }
  pairingTimer = setTimeout(() => {
    if (succeed) {
      nodes.push({ node_id: 7 + nodes.length, device_uid: deviceUid, display_name: '', profile_id: 1, firmware: '2.1.0', state: 'active', last_seen_at_ms: Date.now(), rssi: -63, has_telemetry: true, ...radio(2) })
      registryGeneration += 1
    }
    health.pairing = { active: false, remaining_seconds: 0, indication: 'idle' }
  }, 5_000)
}

let signedIn = true

function mockApi(): Plugin {
  return {
    name: 'mock-gateway-api',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        const path = (req.url ?? '').split('?')[0]
        if (!path.startsWith('/api') && !path.startsWith('/ui') && path !== '/health') return next()
        if (path === '/ui/mock/recovery' && req.method === 'POST') {
          readBody(req, body => {
            routes['/ui/setup'] = { setup_required: body.mode !== 'configured', physical_window_active: body.mode === 'clean', remaining_seconds: body.mode === 'clean' ? 600 : 0, recovery_required: body.mode === 'incomplete' }
            json(res, 200, routes['/ui/setup'])
          })
          return
        }
        // Stands in for a gateway restart: sessions live in RAM, so every
        // request that needs one answers 401 until someone signs in again.
        if (path === '/ui/mock/restart' && req.method === 'POST') {
          signedIn = false
          health.boot_id = randomBytes(16).toString('hex')
          return json(res, 200, { restarted: true })
        }
        if (path === '/ui/session' && req.method === 'POST') signedIn = true
        const publicPath = path === '/health' || path === '/ui/setup' || (path === '/ui/session' && req.method === 'POST')
        if (!signedIn && !publicPath) return json(res, 401, { error: 'authentication_required' })
        if (path === '/ui/backup/export' && req.method === 'POST') {
          readBody(req, body => {
            const payload = Buffer.from(JSON.stringify({ version: 3, created_at_ms: Date.now(), settings: { hostname: 'osk-restored', mdns: true, ntp: true, pairing_seconds: 120, setup_seconds: 600, servers: ['pool.ntp.org'] }, network_id: 123, device_secret: '02'.repeat(32), nodes: nodes.map(n => ({ uid: n.device_uid.toLowerCase(), id: n.node_id, profile: n.profile_id, firmware: [1, 0, 0], state: n.state === 'active' ? 2 : 1, nonce: 1, name: n.display_name, max_power: n.max_power_level, power_policy: 0 })) }))
            const header = Buffer.alloc(44); header.write('OSKB'); header[4] = header[5] = header[6] = 1; header.writeUInt32LE(100_000, 8); header.writeUInt32LE(payload.length, 12); randomBytes(28).copy(header, 16)
            const cipher = createCipheriv('aes-256-gcm', pbkdf2Sync(String(body.password), header.subarray(16, 32), 100_000, 32, 'sha256'), header.subarray(32, 44))
            cipher.setAAD(header)
            const encrypted = Buffer.concat([cipher.update(payload), cipher.final()])
            res.setHeader('Content-Type', 'application/octet-stream'); res.end(Buffer.concat([header, encrypted, cipher.getAuthTag()]))
          })
          return
        }
        if ((path === '/ui/backup/preview' || path === '/ui/backup/restore') && req.method === 'POST') {
          readBody(req, body => {
            const setup = routes['/ui/setup'] as { physical_window_active: boolean; recovery_required?: boolean }
            if (setup.recovery_required) return json(res, 409, { error: 'recovery_required' })
            if (path.endsWith('/restore') && !setup.physical_window_active) return json(res, 403, { error: 'physical_setup_required' })
            try {
              const file = Buffer.from(String(body.file), 'base64')
              if (file.length <= 60 || file.length > 17468 || file.subarray(0, 8).toString('hex') !== '4f534b4201010100' || file.readUInt32LE(8) !== 100_000 || file.readUInt32LE(12) !== file.length - 60) throw new Error('invalid')
              const decipher = createDecipheriv('aes-256-gcm', pbkdf2Sync(String(body.password), file.subarray(16, 32), 100_000, 32, 'sha256'), file.subarray(32, 44))
              decipher.setAAD(file.subarray(0, 44)); decipher.setAuthTag(file.subarray(-16))
              const payload = JSON.parse(Buffer.concat([decipher.update(file.subarray(44, -16)), decipher.final()]).toString())
              if (path.endsWith('/preview')) return json(res, 200, { gateway_id: health.gateway_id, created_at_ms: payload.created_at_ms, node_count: payload.nodes.length, hostname: payload.settings.hostname, mdns_enabled: payload.settings.mdns })
              routes['/ui/setup'] = { setup_required: false, physical_window_active: false, remaining_seconds: 0 }
              json(res, 200, { status: 'restarting' })
            } catch { json(res, 422, { error: 'invalid_backup' }) }
          })
          return
        }
        if (path === '/ui/pairing/open' && req.method === 'POST') {
          let raw = ''
          req.on('data', (chunk) => { raw += chunk })
          req.on('end', () => {
            const uid = String(JSON.parse(raw || '{}').device_uid ?? '').toUpperCase()
            if (nodes.some((node) => node.device_uid.toUpperCase() === uid)) return json(res, 409, { error: 'node_already_exists' })
            openPairingWindow(uid, !(req.url ?? '').includes('fail=1'))
            res.setHeader('Content-Type', 'application/json')
            res.end(JSON.stringify(health.pairing))
          })
          return
        }
        if (path === '/ui/pairing/close' && req.method === 'POST') {
          clearTimeout(pairingTimer)
          health.pairing = { active: false, remaining_seconds: 0, indication: 'idle' }
          res.setHeader('Content-Type', 'application/json')
          res.end(JSON.stringify(health.pairing))
          return
        }
        if (path === '/ui/users') {
          if (req.method === 'GET') return json(res, 200, { users })
          if (req.method === 'POST') {
            readBody(req, (body) => {
              if (users.length >= 4) return json(res, 409, { error: 'user_capacity_reached' })
              if (users.some((user) => user.username === body.username)) return json(res, 409, { error: 'username_already_exists' })
              const created = { id: nextUserId++, username: String(body.username), role: body.role === 'admin' ? 'admin' : 'viewer', enabled: body.enabled !== false }
              users.push(created)
              json(res, 201, created)
            })
            return
          }
          if (req.method === 'PUT') {
            readBody(req, (body) => {
              const user = users.find((entry) => entry.id === body.id)
              if (!user) return json(res, 404, { error: 'user_not_found' })
              const losingLastAdmin = user.role === 'admin' && user.enabled && enabledAdmins() === 1
                && (body.role !== 'admin' || body.enabled === false)
              if (losingLastAdmin) return json(res, 409, { error: 'last_admin_required' })
              if (users.some((entry) => entry.username === body.username && entry.id !== user.id)) {
                return json(res, 409, { error: 'username_already_exists' })
              }
              Object.assign(user, { username: String(body.username), role: body.role === 'admin' ? 'admin' : 'viewer', enabled: body.enabled !== false })
              json(res, 200, user)
            })
            return
          }
          if (req.method === 'DELETE') {
            readBody(req, (body) => {
              const index = users.findIndex((entry) => entry.id === body.id)
              if (index < 0) return json(res, 404, { error: 'user_not_found' })
              const user = users[index]
              if (user.role === 'admin' && user.enabled && enabledAdmins() === 1) {
                return json(res, 409, { error: 'last_admin_required' })
              }
              users.splice(index, 1)
              json(res, 204)
            })
            return
          }
        }
        if (path === '/ui/tokens') {
          if (req.method === 'GET') return json(res, 200, { tokens })
          if (req.method === 'POST') {
            readBody(req, (body) => {
              const name = typeof body.name === 'string' ? body.name : ''
              // `scopes` is optional and the gateway grants its only scope when
              // it is absent; a value that is present is still validated.
              const scopes = body.scopes === undefined ? [...TOKEN_SCOPES] : body.scopes as string[]
              const validScopes = Array.isArray(scopes) && scopes.length > 0 &&
                scopes.every((scope) => TOKEN_SCOPES.includes(scope))
              if (Buffer.byteLength(name, 'utf8') === 0 || Buffer.byteLength(name, 'utf8') > 32 || !validScopes) {
                return json(res, 422, { error: 'invalid_token_values' })
              }
              if (tokens.length >= 8) return json(res, 409, { error: 'token_capacity_reached' })
              const created = { id: nextTokenId++, name, enabled: true, created_at_ms: Date.now(), scopes }
              tokens.push(created)
              // The secret is returned once and never stored in a readable
              // form, exactly as the gateway does it.
              const secret = Array.from({ length: 43 }, () => 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'[Math.floor(Math.random() * 64)]).join('')
              json(res, 201, { id: created.id, name: created.name, created_at_ms: created.created_at_ms, scopes: created.scopes, token: secret })
            })
            return
          }
          if (req.method === 'DELETE') {
            readBody(req, (body) => {
              const index = tokens.findIndex((token) => token.id === body.id)
              if (index < 0) return json(res, 404, { error: 'token_not_found' })
              tokens.splice(index, 1)
              json(res, 204)
            })
            return
          }
        }
        if (path === '/ui/commands') {
          if (req.method === 'GET') return json(res, 200, { commands })
          if (req.method === 'POST') {
            readBody(req, (body) => {
              const result = queueMockCommand(body)
              json(res, result.status, result.body)
            })
            return
          }
          if (req.method === 'DELETE') {
            readBody(req, (body) => {
              const index = commands.findIndex((entry) => entry.node_id === body.node_id && entry.state !== 'completed')
              if (index < 0) return json(res, 404, { error: 'command_not_found' })
              for (const timer of commandTimers.get(commands[index].node_id) ?? []) clearTimeout(timer)
              commands.splice(index, 1)
              json(res, 204)
            })
            return
          }
        }
        if (path === '/api/nodes' && req.method === 'GET') {
          res.setHeader('Content-Type', 'application/json')
          res.end(JSON.stringify({ registry_generation: registryGeneration, nodes }))
          return
        }
        // Firmware update: a check finds 9.9.9 after a second, an install
        // counts to 100 % and then stands in for the restart.
        // The public probe carries only the boot id, read live so a stand-in
        // restart shows up; everything else is behind a session at /ui/status.
        if (path === '/health') return json(res, 200, { status: 'ok', boot_id: health.boot_id })
        if (path === '/ui/update' && req.method === 'GET') return json(res, 200, update)
        if (path === '/ui/update/check' && req.method === 'POST') {
          Object.assign(update, { state: 'checking', error: '' })
          setTimeout(() => Object.assign(update, { state: 'available', available_version: '9.9.9' }), 1_000)
          return json(res, 202, update)
        }
        if (path === '/ui/update/install' && req.method === 'POST') {
          if (update.state !== 'available') return json(res, 409, { error: 'update_unavailable' })
          Object.assign(update, { state: 'installing', progress: 0 })
          const timer = setInterval(() => {
            update.progress += 10
            if (update.progress < 100) return
            clearInterval(timer)
            update.state = 'restarting'
            setTimeout(() => {
              Object.assign(update, { state: 'idle', progress: 0, current_version: update.available_version, available_version: '', pending_verify: true })
              health.boot_id = 'aabbccddeeff00112233445566778899'
            }, 2_000)
          }, 400)
          return json(res, 202, update)
        }
        if (path === '/ui/radio/reset' && req.method === 'POST') {
          let raw = ''
          req.on('data', (chunk) => { raw += chunk })
          req.on('end', () => {
            const requested = JSON.parse(raw || '{}').operational_network_id
            const removed = nodes.length
            nodes.length = 0
            registryGeneration += 1
            health.radio.network_id = requested || 137
            // Stand in for the reboot so the dialog's wait actually resolves.
            setTimeout(() => { health.boot_id = 'ffeeddccbbaa99887766554433221100' }, 4_000)
            res.statusCode = 202
            res.setHeader('Content-Type', 'application/json')
            res.end(JSON.stringify({ operational_network_id: health.radio.network_id, removed_nodes: removed, restarting: true }))
          })
          return
        }
        if (path === '/ui/nodes' && req.method === 'PATCH') {
          let raw = ''
          req.on('data', (chunk) => { raw += chunk })
          req.on('end', () => {
            const body = JSON.parse(raw || '{}')
            const node = nodes.find((entry) => entry.node_id === body.node_id)
            if (!node) return json(res, 404, { error: 'node_not_found' })
            if (typeof body.display_name === 'string') node.display_name = body.display_name
            if (body.power_policy === 'auto') {
              Object.assign(node, { power_policy: 'auto', fixed_power_level: undefined, tx_power_target: node.tx_power_level })
            } else if (body.power_policy === 'fixed') {
              const level = body.fixed_power_level
              if (!Number.isInteger(level) || level < 0 || level > node.max_power_level) return json(res, 422, { error: 'invalid_power_policy' })
              Object.assign(node, { power_policy: 'fixed', fixed_power_level: level, tx_power_target: level })
              // The node picks the level up with its next report.
              setTimeout(() => { node.tx_power_level = level; node.radio_fallback = false }, 5_000)
            }
            registryGeneration += 1
            json(res, 200, { ...body, registry_generation: registryGeneration })
          })
          return
        }
        if (path === '/ui/nodes' && req.method === 'DELETE') {
          let raw = ''
          req.on('data', (chunk) => { raw += chunk })
          req.on('end', () => {
            const { node_id: id } = JSON.parse(raw || '{}')
            const index = nodes.findIndex((entry) => entry.node_id === id)
            if (index >= 0) nodes.splice(index, 1)
            res.statusCode = 204
            res.end()
          })
          return
        }
        const body = routes[path] ?? {}
        res.setHeader('Content-Type', 'application/json')
        res.end(JSON.stringify(body))
      })
    },
  }
}

export default defineConfig({
  plugins: [vue(), mockApi()],
  define: { __UI_VERSION__: JSON.stringify(firmwareVersion) },
})
