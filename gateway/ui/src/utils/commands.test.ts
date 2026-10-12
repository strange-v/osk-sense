import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import {
  COUNT_MAX,
  commandArguments,
  commandValue,
  parseCommandValue,
  supportedCommands,
} from './commands'

interface Manifest {
  commands: { types: Array<{ type: number; name: string }> }
  telemetry: { profiles: Array<{ id: number; commands: number[] }> }
}

const manifest = JSON.parse(
  readFileSync(new URL('../../../../protocol/protocol-manifest.json', import.meta.url), 'utf8'),
) as Manifest

describe('command catalogue', () => {
  it('offers exactly the commands the protocol manifest lists per profile', () => {
    const names = new Map(manifest.commands.types.map((type) => [type.type, type.name]))
    for (const profile of manifest.telemetry.profiles) {
      expect(supportedCommands(profile.id), `profile ${profile.id}`).toEqual(
        profile.commands.map((type) => names.get(type)),
      )
    }
    expect(supportedCommands(0)).toEqual([])
    expect(supportedCommands(99)).toEqual([])
  })
})

describe('parseCommandValue', () => {
  it('accepts whole numbers within the count range', () => {
    expect(parseCommandValue('set_count', '0')).toBe(0)
    expect(parseCommandValue('set_count', ' 31 ')).toBe(31)
    expect(parseCommandValue('set_count', '4294967295')).toBe(COUNT_MAX)
  })

  it('rejects anything else', () => {
    for (const text of ['', '-1', '1e3', '12.5', '0x10', 'abc', '4294967296']) {
      expect(parseCommandValue('set_count', text), text).toBeNull()
    }
  })
})

describe('command arguments', () => {
  it('names the argument after the command type', () => {
    expect(commandArguments('set_count', 1234)).toEqual({ count: 1234 })
    expect(commandArguments('read_info', null)).toEqual({})
    expect(commandValue({ arguments: { count: 0 } })).toBe(0)
  })
})
