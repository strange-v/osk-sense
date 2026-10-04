import assert from 'node:assert/strict'
import { createCipheriv } from 'node:crypto'
import { readFileSync } from 'node:fs'

const bytes = (hex) => Buffer.from(hex, 'hex')
const encrypt = (key, block) => {
  const cipher = createCipheriv('aes-128-ecb', key, null)
  cipher.setAutoPadding(false)
  return Buffer.concat([cipher.update(block), cipher.final()])
}
const xor = (a, b) => Buffer.from(a.map((value, i) => value ^ b[i]))
const double = (block) => {
  const result = Buffer.alloc(16)
  for (let i = 0; i < 16; i++) result[i] = ((block[i] << 1) | ((block[i + 1] ?? 0) >> 7)) & 255
  if (block[0] & 128) result[15] ^= 0x87
  return result
}
const cmac = (key, input) => {
  const k1 = double(encrypt(key, Buffer.alloc(16)))
  const k2 = double(k1)
  let state = Buffer.alloc(16)
  let position = 0
  while (input.length - position > 16) {
    state = encrypt(key, xor(state, input.subarray(position, position + 16)))
    position += 16
  }
  const last = Buffer.alloc(16)
  input.copy(last, 0, position)
  const remaining = input.length - position
  if (remaining < 16) last[remaining] = 128
  return encrypt(key, xor(state, xor(last, remaining === 16 ? k1 : k2)))
}
// Check the independent CMAC implementation before using it as an oracle.
const standardKey = bytes('2b7e151628aed2a6abf7158809cf4f3c')
assert.equal(cmac(standardKey, Buffer.alloc(0)).toString('hex'), 'bb1d6929e95937287fa37d129b756746')
assert.equal(cmac(standardKey, bytes('6bc1bee22e409f96e93d7e117393172a')).toString('hex'), '070a16b46b4d4144f79bdd9dd04a287c')

const factory = bytes('000102030405060708090a0b0c0d0e0f')
const salt = bytes('1011121314151617')
const enc = cmac(factory, Buffer.concat([Buffer.from([1]), salt]))
const mac = cmac(factory, Buffer.concat([Buffer.from([2]), salt]))
const counter = bytes('78563412')
const transport = Buffer.from([100, 7, 0x40])
const plain = bytes('02bae40c2e09d7119427')
const nonce = (header, direction = 0, index = 0) => Buffer.concat([
  Buffer.from([direction, 7, header]), counter, Buffer.alloc(8), Buffer.from([index]),
])
const ciphertext = (header, direction = 0) => xor(plain, encrypt(enc, nonce(header, direction)).subarray(0, plain.length))
const tag = (header, transport, payload) => cmac(mac, Buffer.concat([Buffer.from([header]), transport, counter, payload]))
const telemetry = ciphertext(0x60)
const activation = Buffer.concat([salt, ciphertext(0x6a)])
const downlink = Buffer.from([7, 100, 0x80])
const ack = bytes('07' + '17' + '78573412')
const replyTransport = Buffer.from([7, 100, 0])
const command = ciphertext(0x64, 1)
const confirm = bytes('630102030405060708090a7876541200010203')
const confirmTag = cmac(mac, Buffer.concat([confirm.subarray(0, 1), Buffer.from([100,7,0]), confirm.subarray(1), salt]))
const result = {
  factory_key: factory.toString('hex'), salt: salt.toString('hex'),
  encryption_key: enc.toString('hex'), authentication_key: mac.toString('hex'),
  counter: 0x12345678, node_id: 7, gateway_id: 100, plaintext: plain.toString('hex'),
  telemetry: Buffer.concat([Buffer.from([0x60]), counter, telemetry, tag(0x60, transport, telemetry).subarray(0, 8)]).toString('hex'),
  activation: Buffer.concat([Buffer.from([0x6a]), counter, activation, tag(0x6a, transport, activation).subarray(0, 8)]).toString('hex'),
  ack_floor: Buffer.concat([ack, tag(0x7f, downlink, ack).subarray(0, 6)]).toString('hex'),
  command: Buffer.concat([Buffer.from([0x64]), counter, command, tag(0x64, replyTransport, command).subarray(0, 6)]).toString('hex'),
  join_confirm: Buffer.concat([confirm, confirmTag.subarray(0, 8)]).toString('hex'),
}
const vectors = JSON.parse(readFileSync(new URL('../protocol-vectors.json', import.meta.url), 'utf8'))
assert.deepEqual(result, vectors.crypto_v3)
console.log('Validated V3 KDF, telemetry, activation, ACK, command and Join confirm against independent AES/CMAC.')
