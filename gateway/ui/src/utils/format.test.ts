// @vitest-environment jsdom
import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'
import SignalBars from '../components/SignalBars.vue'
import {
  HOSTNAME_MAX_BYTES,
  RSSI_MAX_DBM,
  RSSI_MIN_DBM,
  byteLength,
  countdown,
  isMeasuredRssi,
  isValidHostname,
  signal,
  truncateToBytes,
  volts,
} from './format'

// The RFM69 driver returns -RegRssiValue/2 from an 8-bit register, so every
// genuine reading is a negative whole dBm no lower than -128.
describe('isMeasuredRssi', () => {
  it('accepts the whole range the module can report', () => {
    expect(isMeasuredRssi(RSSI_MIN_DBM)).toBe(true)
    expect(isMeasuredRssi(RSSI_MAX_DBM)).toBe(true)
    expect(isMeasuredRssi(-63)).toBe(true)
  })
  it('rejects 0, which the driver uses for "nothing received yet"', () => {
    expect(isMeasuredRssi(0)).toBe(false)
  })
  it('rejects values the module cannot produce', () => {
    expect(isMeasuredRssi(5)).toBe(false)
    expect(isMeasuredRssi(-129)).toBe(false)
    expect(isMeasuredRssi(Number.NaN)).toBe(false)
    expect(isMeasuredRssi(undefined)).toBe(false)
  })
})

describe('signal', () => {
  it('renders a measurement in dBm', () => expect(signal(-74)).toBe('-74 dBm'))
  it('renders a dash rather than "0 dBm" for a missing measurement', () => {
    expect(signal(undefined)).toBe('—')
    expect(signal(0)).toBe('—')
  })
})

describe('volts', () => {
  it('shows millivolts as volts with two decimals', () => {
    expect(volts(2987)).toBe('2.99 V')
    expect(volts(3000)).toBe('3.00 V')
  })

  it('shows a dash when the node did not measure its supply', () => {
    expect(volts(undefined)).toBe('—')
  })
})

describe('SignalBars', () => {
  const lit = (rssi?: number) => mount(SignalBars, { props: { rssi } }).findAll('i.on').length

  it('maps the RFM69 range onto four bars', () => {
    expect(lit(-40)).toBe(4)
    expect(lit(-65)).toBe(4)
    expect(lit(-66)).toBe(3)
    expect(lit(-82)).toBe(3)
    expect(lit(-83)).toBe(2)
    expect(lit(-93)).toBe(2)
    expect(lit(-94)).toBe(1)
  })

  // Measured positions on this network, with the swing RSSI shows between
  // packets: none of them may sit on a band edge.
  it('keeps the known-good positions clear of the band edges', () => {
    for (const rssi of [-35, -37, -40]) expect(lit(rssi)).toBe(4) // beside the gateway
    for (const rssi of [-71, -77, -80]) expect(lit(rssi)).toBe(3) // balcony, one wall
    for (const rssi of [-97, -100, -103]) expect(lit(rssi)).toBe(1) // outer corridor
  })

  it('never drops a real measurement to zero bars', () => {
    // A packet arrived, so there is a link: the weakest reading is one bar,
    // and "no link" is the separate no-telemetry state.
    expect(lit(-110)).toBe(1)
    expect(lit(RSSI_MIN_DBM)).toBe(1)
  })

  it('shows no bars for a value that is not a measurement', () => {
    // 0 is the trap: numerically the top of the scale, actually "no packet".
    expect(lit(0)).toBe(0)
    expect(lit(undefined)).toBe(0)
    expect(lit(12)).toBe(0)
  })

  it('labels only a real reading', () => {
    expect(mount(SignalBars, { props: { rssi: -74 } }).attributes('aria-label')).toBe('-74 dBm')
    expect(mount(SignalBars, { props: { rssi: 0 } }).attributes('aria-label')).toBeUndefined()
  })
})

// Node names are limited in UTF-8 bytes, and the field trims to that limit as
// it is typed, so a cut must never leave half a character behind.
describe('truncateToBytes', () => {
  it('leaves a string that fits alone', () => {
    expect(truncateToBytes('Hall', 48)).toBe('Hall')
    expect(truncateToBytes('', 0)).toBe('')
  })

  it('counts Cyrillic as two bytes a letter', () => {
    expect(truncateToBytes('я'.repeat(30), 48)).toBe('я'.repeat(24))
  })

  it('cuts between characters, never inside one', () => {
    expect(truncateToBytes('aя', 2)).toBe('a')
    expect(truncateToBytes('a€', 3)).toBe('a')
    expect(truncateToBytes('a😀', 4)).toBe('a')
    expect(truncateToBytes('a😀', 5)).toBe('a😀')
  })

  it('agrees with byteLength on what fits', () => {
    for (const text of ['Кухня на першому поверсі 😀', 'a€b😀c', '\ud800x']) {
      for (let max = 0; max <= byteLength(text); max += 1) {
        const cut = truncateToBytes(text, max)
        expect(byteLength(cut)).toBeLessThanOrEqual(max)
        expect(text.startsWith(cut)).toBe(true)
      }
    }
  })
})

// Mirrors validHostname() in GatewayStorage.cpp. The name reaches DNS and DHCP
// unchanged, so anything the firmware would refuse must be refused here too --
// otherwise the form accepts a name the save then rejects.
describe('isValidHostname', () => {
  it('accepts a DNS label', () => {
    expect(isValidHostname('osk-hub-floor1')).toBe(true)
    expect(isValidHostname('a')).toBe(true)
    expect(isValidHostname('1')).toBe(true)
    expect(isValidHostname('a'.repeat(HOSTNAME_MAX_BYTES))).toBe(true)
  })

  it('accepts empty, which keeps the osk-hub default', () => {
    expect(isValidHostname('')).toBe(true)
  })

  it('refuses what DNS cannot carry', () => {
    expect(isValidHostname('OSK-Hub')).toBe(false)
    expect(isValidHostname('-hub')).toBe(false)
    expect(isValidHostname('hub-')).toBe(false)
    expect(isValidHostname('osk hub')).toBe(false)
    expect(isValidHostname('hub_1')).toBe(false)
    expect(isValidHostname('hub.local')).toBe(false)
    expect(isValidHostname('шлюз')).toBe(false)
    expect(isValidHostname('a'.repeat(HOSTNAME_MAX_BYTES + 1))).toBe(false)
  })
})

describe('countdown', () => {
  it('shows minutes and zero-padded seconds', () => {
    expect(countdown(600)).toBe('10:00')
    expect(countdown(65)).toBe('1:05')
    expect(countdown(9)).toBe('0:09')
  })
  it('never goes below zero', () => {
    expect(countdown(-3)).toBe('0:00')
  })
})
