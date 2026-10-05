export const MAX_BACKUP_BYTES = 34_816 + 44 + 16
export function validBackupPassword(value: string): boolean {
  const length = new TextEncoder().encode(value).length
  return length >= 12 && length <= 128 && !value.includes('\0')
}
export async function encodeBackupFile(file: File): Promise<string> {
  if (file.size <= 60 || file.size > MAX_BACKUP_BYTES) throw new Error('invalid_backup')
  const bytes = new Uint8Array(await file.arrayBuffer())
  if (String.fromCharCode(...bytes.subarray(0, 4)) !== 'OSKB') throw new Error('invalid_backup')
  return btoa(String.fromCharCode(...bytes))
}
export function downloadBackup(blob: Blob) {
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = `osk-sense-${new Date().toISOString().slice(0, 10)}.oskbackup`
  document.body.append(link)
  link.click()
  link.remove()
  // Keep the URL alive until the document closes, avoiding a download/revoke race.
  window.addEventListener('pagehide', () => URL.revokeObjectURL(url), { once: true })
}
