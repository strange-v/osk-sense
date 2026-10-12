import { describe, expect, it } from 'vitest'
import { encodeBackupFile, MAX_BACKUP_BYTES, validBackupPassword } from './backup'

describe('backup input', () => {
  it('counts UTF-8 bytes and rejects NUL and oversized passwords', () => {
    expect(validBackupPassword('short')).toBe(false)
    expect(validBackupPassword('пароль')).toBe(true)
    expect(validBackupPassword('a'.repeat(128))).toBe(true)
    expect(validBackupPassword('я'.repeat(65))).toBe(false)
    expect(validBackupPassword('password\0more')).toBe(false)
  })
  it('rejects oversized and wrong-format files before upload', async () => {
    await expect(
      encodeBackupFile(new File([new Uint8Array(MAX_BACKUP_BYTES + 1)], 'large')),
    ).rejects.toThrow()
    await expect(encodeBackupFile(new File(['other'.repeat(20)], 'bad'))).rejects.toThrow()
  })
  it('preserves the exact encrypted bytes in base64 transport', async () => {
    const bytes = Uint8Array.from({ length: 100 }, (_, i) => i)
    bytes.set(new TextEncoder().encode('OSKB'))
    const encoded = await encodeBackupFile(new File([bytes], 'test.oskbackup'))
    expect(Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0))).toEqual(bytes)
  })
})
