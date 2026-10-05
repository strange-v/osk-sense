export interface SetupStatus { setup_required: boolean; physical_window_active: boolean; remaining_seconds: number; recovery_required?: boolean; recovery_reason?: string }
export interface BackupPreview { gateway_id: string; created_at_ms: number; node_count: number; hostname: string; mdns_enabled: boolean }
export interface SetupRequest { username: string; password: string; hostname?: string; operational_network_id?: number }
export interface SessionUser { id: number; username: string; role: 'admin' | 'viewer' }
export interface Session { user: SessionUser; csrf_token: string }

// A stored local account. The gateway never returns password material, and the
// list is capped -- see USER_LIMIT.
export interface GatewayUser { id: number; username: string; role: 'admin' | 'viewer'; enabled: boolean }
export interface UserList { users: GatewayUser[] }
// `password` is optional on update: omitted leaves the stored one alone.
export interface UserWrite { username: string; role: 'admin' | 'viewer'; enabled: boolean; password?: string }
// The gateway has exactly one scope (TokenScope in GatewayStorage.h), so the
// UI never asks which to grant and never sends the field. The type stays a
// union so that adding a second one -- a write scope, if a token is ever
// allowed to change anything -- surfaces as a compile error rather than as a
// silent widening.
export type TokenScope = 'telemetry:read'
export interface ApiToken { id: number; name: string; enabled: boolean; created_at_ms: number; scopes: TokenScope[] }
// The 201 body carries the generated secret exactly once and, unlike a list
// entry, has no `enabled` field -- a new token is always enabled
// (WebServer.cpp, handleCreateToken).
export interface CreatedApiToken extends Omit<ApiToken, 'enabled'> { token: string }
export interface ApiTokenList { tokens: ApiToken[] }
// One live WebSocket client. `name` is what the client sent as `X-Client` at
// the handshake, so it identifies itself; an empty name is a client that sent
// nothing, and must be shown as unknown rather than guessed at from the keys.
export interface StreamClient { id: number; name: string }
export interface StreamClientList { clients: StreamClient[] }
export interface GatewaySettings { generation: number; hostname: string; mdns_enabled: boolean; ntp_enabled: boolean; pairing_window_seconds: number; setup_window_seconds: number; ntp_servers: string[] }
export interface PairingStatus { active: boolean; remaining_seconds: number; indication: string }
export interface GatewayInfo { firmware_version: string; api_version: number; stream_version: number; ui: { state: string; version: string; required_firmware: string }; board: string; hostname: string; gateway_id: string; boot_id: string; uptime_seconds: number }
export interface GatewayNode {
  node_id: number
  // Immutable uppercase factory UID. With gateway_id this is the stable Home
  // Assistant identity; the radio node_id is reusable and is not.
  device_uid: string
  display_name: string
  profile_id: number
  firmware: string
  state: 'pending' | 'active' | 'disabled' | string
  last_seen_at_ms?: number
  rssi?: number
  has_telemetry?: boolean
  // Radio power: the ceiling the node reported at pairing, the gateway's
  // policy and wanted level, and what the node's latest report says it uses.
  max_power_level?: number
  power_policy?: PowerPolicy
  fixed_power_level?: number
  tx_power_target?: number
  tx_power_level?: number
  radio_fallback?: boolean
  downlink_rssi?: number
  // Absent when the node could not measure its supply.
  supply_mv?: number
}
export type PowerPolicy = 'auto' | 'fixed'
export interface PowerPolicyWrite { power_policy: PowerPolicy; fixed_power_level?: number }
export interface PatchedNode { node_id: number; display_name?: string; power_policy?: PowerPolicy; fixed_power_level?: number; registry_generation: number }
// The command types of protocol/protocol-manifest.json.
export type CommandType = 'set_count' | 'read_info'
export interface CommandArguments { count?: number }
export interface CountResult { previous_count: number; count: number }
export interface NodeInfoResult { profile_id: number; firmware: string; max_power_level: number }
export interface NodeCommand {
  node_id: number
  command_id: number
  type: CommandType
  arguments: CommandArguments
  queued_at_ms: number
  // `delivered` lives in gateway RAM: after a reboot the command reads as
  // pending again until the node's next session.
  state: 'pending' | 'delivered' | 'completed'
  status?: 'applied' | 'unsupported' | 'invalid_argument'
  completed_at_ms?: number
  result?: CountResult | NodeInfoResult
}
export interface CommandList { commands: NodeCommand[] }
export interface CommandRequest { node_id: number; type: CommandType; arguments: CommandArguments }
export interface RadioNetworkReset { operational_network_id: number; removed_nodes: number; restarting: boolean }
export interface RenamedNode { node_id: number; display_name: string; registry_generation: number }
export interface NodeRegistry { registry_generation: number; nodes: GatewayNode[] }
// The public /health probe. Everything else the gateway knows about itself is
// behind a session in Health, returned by /ui/status.
export interface GatewayProbe { status: string; boot_id: string }
export type UpdateState = 'idle' | 'checking' | 'up_to_date' | 'available' | 'installing' | 'restarting' | 'failed'
export interface UpdateStatus { state: UpdateState; current_version: string; available_version: string; progress: number; error: string; pending_verify: boolean }
export interface Health {
  status: string; firmware: string; api_version: number; board: string; hostname: string; gateway_id: string; boot_id: string; reset_reason: string; uptime_seconds: number; free_heap: number;
  registry: { ready: boolean; records: number; generation: number }; setup: { required: boolean; active: boolean; remaining_seconds: number };
  pairing: PairingStatus;
  storage: {
    ready: boolean; settings_generation: number; auth_generation: number; secrets_generation: number;
    nvs: { stats_available: boolean; used_entries: number; free_entries: number; available_entries: number; total_entries: number; namespace_count: number };
  };
  ethernet: { state: string; has_ip: boolean; ip: string; mac: string }; ota: { enabled: boolean; state: string; progress: number };
  web_ui: { state: string; version: string; required_firmware: string }; telemetry: { nodes_seen: number; updates: number; last_node_id: number; last_received_at_ms: number };
  time: { state: string; unix_ms: number; last_sync_ms: number }; websocket: { clients: number; connections: number; messages_sent: number; messages_dropped: number };
  radio: { state: string; present: boolean; version: number; frequency_band_mhz: string; frequency_hz: number; bit_rate: number; node_id: number; network_id: number; variant: string; configured_power_dbm: number; encryption_enabled: boolean; profile: string; spi_host: string; pins: Record<string, number>; counters: Record<string, number>; last_packet: { at_ms: number; sender_id: number; rssi: number } };
  [key: string]: unknown;
}
