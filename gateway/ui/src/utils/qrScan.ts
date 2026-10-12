// Local-only pairing QR decoding. The picked or freshly shot image is turned
// into pixels in this tab and handed to jsQR; nothing is uploaded, and the
// gateway never sees the photo.
//
// Loaded through a dynamic import from the pairing dialog, so jsQR's ~15 KB
// gzip only reaches the browser when someone actually scans.
import jsQR from 'jsqr'
import { parsePairingUri, type PairingUri } from './hexCredentials'

// 'unreadable' -- the file was not an image this browser can decode.
// 'no_code'    -- decoded fine, but no QR code was found in it.
// 'unsupported'-- a QR code was found, but it is not a v1 sense pairing code.
export type QrScanStatus = 'read' | 'no_code' | 'unsupported' | 'unreadable'

export type QrScanResult =
  | { status: 'read'; credentials: PairingUri }
  | { status: Exclude<QrScanStatus, 'read'> }

// Decode passes, tried in order until one reads a code. Each is a longest-edge
// size in pixels and a blur radius applied while drawing.
//
// Counter-intuitively, more pixels make jsQR *worse*, and the order here is
// deliberate. Measured against real Pixel 8 photos (3072x4080) of a pairing
// code on a monitor: full resolution took 22 seconds and found nothing, while
// the same photo read in 22 ms at 400 px. jsQR wants a handful of pixels per
// module; feed it dozens and the screen's subpixel grid, paper grain, or JPEG
// noise dominate its local binarizer and the modules never come out clean.
// The blur is a low-pass filter for exactly that texture -- it is what makes
// photographs of a screen readable at all (one sample went from failing at
// every size to passing at every size >= 300 px once blurred).
//
// The large passes at the end are not redundant: they cover the opposite
// regime, a code occupying a small part of the frame, where shrinking to
// 700 px leaves too few pixels per module to resolve.
export const DECODE_PASSES: ReadonlyArray<{ edge: number; blur: number }> = [
  { edge: 700, blur: 1 },
  { edge: 400, blur: 0 },
  { edge: 250, blur: 0 },
  { edge: 1100, blur: 1.5 },
  { edge: 1700, blur: 0 },
]

interface DecodableImage {
  readonly width: number
  readonly height: number
  close(): void
}

// createImageBitmap is the cheap path (decodes off the main thread and needs
// no document); older WebKit needs the <img> route.
async function loadImage(
  file: File,
): Promise<{ source: CanvasImageSource; image: DecodableImage }> {
  if (typeof createImageBitmap === 'function') {
    const bitmap = await createImageBitmap(file)
    return { source: bitmap, image: bitmap }
  }
  const url = URL.createObjectURL(file)
  try {
    const element = await new Promise<HTMLImageElement>((resolve, reject) => {
      const img = new Image()
      img.onload = () => resolve(img)
      img.onerror = () => reject(new Error('image decode failed'))
      img.src = url
    })
    return {
      source: element,
      image: {
        width: element.naturalWidth,
        height: element.naturalHeight,
        // Drop the decoded pixels and the blob URL together.
        close: () => {
          element.src = ''
          URL.revokeObjectURL(url)
        },
      },
    }
  } catch (error) {
    URL.revokeObjectURL(url)
    throw error
  }
}

// The size one pass draws at: never upscales, so a small image is decoded as
// it is rather than blown up into blur.
function passSize(width: number, height: number, edge: number) {
  const scale = Math.min(1, edge / Math.max(width, height))
  return {
    width: Math.max(1, Math.round(width * scale)),
    height: Math.max(1, Math.round(height * scale)),
  }
}

// Draws the image into a throwaway canvas at the given size and asks jsQR for
// a code. The canvas is collapsed to 0x0 before returning so the backing
// surface is released immediately rather than at the next garbage collection.
function decodeAt(
  source: CanvasImageSource,
  width: number,
  height: number,
  blur: number,
): string | null {
  const canvas = document.createElement('canvas')
  canvas.width = width
  canvas.height = height
  try {
    const context = canvas.getContext('2d', { willReadFrequently: true })
    if (!context) return null
    if (blur) context.filter = `blur(${blur}px)`
    context.drawImage(source, 0, 0, width, height)
    const pixels = context.getImageData(0, 0, width, height)
    return jsQR(pixels.data, pixels.width, pixels.height)?.data ?? null
  } finally {
    canvas.width = 0
    canvas.height = 0
  }
}

// jsQR runs synchronously and can hold the main thread for a second or more on
// a full-resolution photo. Give the browser one frame to paint first, so the
// caller's "scanning" state is on screen before everything freezes -- the
// awaits above only yield to microtasks, which is not enough for a paint.
function nextPaint(): Promise<void> {
  return new Promise((resolve) => {
    if (typeof requestAnimationFrame !== 'function') return resolve()
    requestAnimationFrame(() => setTimeout(resolve, 0))
  })
}

export async function scanPairingQr(file: File): Promise<QrScanResult> {
  let loaded
  try {
    loaded = await loadImage(file)
  } catch {
    return { status: 'unreadable' }
  }

  const { source, image } = loaded
  try {
    if (!image.width || !image.height) return { status: 'unreadable' }
    await nextPaint()

    let text: string | null = null
    // An image smaller than a pass's edge is decoded at its own size, so two
    // passes can land on the same pixels; run each distinct one once.
    const seen = new Set<string>()
    for (const { edge, blur } of DECODE_PASSES) {
      const { width, height } = passSize(image.width, image.height, edge)
      const key = `${width}x${height}@${blur}`
      if (seen.has(key)) continue
      seen.add(key)
      text = decodeAt(source, width, height, blur)
      if (text !== null) break
    }
    if (text === null) return { status: 'no_code' }

    const credentials = parsePairingUri(text)
    return credentials ? { status: 'read', credentials } : { status: 'unsupported' }
  } finally {
    image.close()
  }
}
