// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

// jsQR is stubbed: this exercises the decode pipeline around it (downscaling,
// the full-resolution retry, result mapping, and cleanup), not the QR maths.
const decode = vi.hoisted(() => vi.fn())
vi.mock('jsqr', () => ({ default: decode }))

import { DECODE_PASSES, scanPairingQr } from './qrScan'

const UID = '102132435465768798A9'
const KEY = '00112233445566778899AABBCCDDEEFF'
const PAIRING_URI = `web+opensmartkit:pair?v=1&family=sense&uid=${UID.toLowerCase()}&key=${KEY.toLowerCase()}`

const photo = () => new File([new Uint8Array([1, 2, 3])], 'label.jpg', { type: 'image/jpeg' })

// Records the pixel dimensions every decode pass was handed, so the tests can
// assert what was downscaled and what was retried at full resolution.
const passes: Array<{ width: number; height: number }> = []
let closed = 0

function stubImageBitmap(width: number, height: number) {
  vi.stubGlobal(
    'createImageBitmap',
    vi.fn(async () => ({
      width,
      height,
      close: () => {
        closed += 1
      },
    })),
  )
}

beforeEach(() => {
  passes.length = 0
  closed = 0
  decode.mockReset()
  // jsdom has no 2D canvas; a stub that reports the requested size is all the
  // pipeline needs.
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(function (
    this: HTMLCanvasElement,
  ) {
    const canvas = this
    return {
      drawImage: () => {},
      getImageData: (_x: number, _y: number, width: number, height: number) => {
        passes.push({ width, height })
        return { data: new Uint8ClampedArray(width * height * 4), width, height }
      },
    } as unknown as CanvasRenderingContext2D
  })
})
afterEach(() => {
  vi.unstubAllGlobals()
  vi.restoreAllMocks()
})

describe('scanPairingQr', () => {
  it('yields a frame before the blocking decode so a busy state can paint', async () => {
    stubImageBitmap(800, 600)
    decode.mockReturnValue({ data: PAIRING_URI })
    const frames = vi.spyOn(globalThis, 'requestAnimationFrame')
    await scanPairingQr(photo())
    expect(frames).toHaveBeenCalled()
  })

  it('fills credentials from a canonical pairing code', async () => {
    stubImageBitmap(800, 600)
    decode.mockReturnValue({ data: PAIRING_URI })
    await expect(scanPairingQr(photo())).resolves.toEqual({
      status: 'read',
      credentials: { uid: UID, key: KEY },
    })
  })

  it('reports a QR code that is not a pairing code', async () => {
    stubImageBitmap(800, 600)
    decode.mockReturnValue({ data: 'https://example.com' })
    await expect(scanPairingQr(photo())).resolves.toEqual({ status: 'unsupported' })
  })

  it('reports an image with no QR code in it', async () => {
    stubImageBitmap(800, 600)
    decode.mockReturnValue(null)
    await expect(scanPairingQr(photo())).resolves.toEqual({ status: 'no_code' })
  })

  it('reports a file the browser cannot decode as an image', async () => {
    vi.stubGlobal(
      'createImageBitmap',
      vi.fn(async () => {
        throw new Error('not an image')
      }),
    )
    await expect(scanPairingQr(photo())).resolves.toEqual({ status: 'unreadable' })
    expect(decode).not.toHaveBeenCalled()
  })

  it('decodes a phone-sized photo far below full resolution', async () => {
    stubImageBitmap(3072, 4080)
    decode.mockReturnValue({ data: PAIRING_URI })
    await scanPairingQr(photo())
    // One pass only, and nowhere near the 3072x4080 the camera produced --
    // full resolution is what made real photos fail.
    expect(passes).toEqual([{ width: 527, height: 700 }])
  })

  it('stops at the first pass that reads a code', async () => {
    stubImageBitmap(3072, 4080)
    decode.mockReturnValueOnce(null).mockReturnValueOnce({ data: PAIRING_URI })
    await expect(scanPairingQr(photo())).resolves.toMatchObject({ status: 'read' })
    expect(passes).toEqual([
      { width: 527, height: 700 },
      { width: 301, height: 400 },
    ])
  })

  it('works up the whole ladder before reporting a miss', async () => {
    stubImageBitmap(3072, 4080)
    decode.mockReturnValue(null)
    await expect(scanPairingQr(photo())).resolves.toEqual({ status: 'no_code' })
    expect(passes).toHaveLength(DECODE_PASSES.length)
    // Both regimes are covered: shrink hard for a code that fills the frame,
    // and stay large for one that occupies a small part of it.
    expect(Math.min(...passes.map((p) => p.height))).toBeLessThanOrEqual(250)
    expect(Math.max(...passes.map((p) => p.height))).toBeGreaterThanOrEqual(1700)
  })

  it('never upscales, and runs a repeated size only once', async () => {
    // Smaller than every pass edge, so all five collapse onto one decode.
    stubImageBitmap(240, 180)
    decode.mockReturnValue(null)
    await scanPairingQr(photo())
    // Three, not five: the ladder's three unblurred passes all collapse onto
    // the same pixels, while the two blurred ones stay distinct.
    expect(passes).toEqual([
      { width: 240, height: 180 },
      { width: 240, height: 180 },
      { width: 240, height: 180 },
    ])
    expect(new Set(DECODE_PASSES.map((pass) => pass.blur)).size).toBe(passes.length)
  })

  it('releases the decoded image whatever the outcome', async () => {
    stubImageBitmap(800, 600)
    decode.mockReturnValue(null)
    await scanPairingQr(photo())
    decode.mockReturnValue({ data: PAIRING_URI })
    await scanPairingQr(photo())
    expect(closed).toBe(2)
  })
})
