import { afterEach, describe, expect, it, vi } from 'vitest'
import { ApiError, api, errorCode, gatewayReachable, onSessionLost, sessionUser } from './client'
afterEach(() => vi.unstubAllGlobals())
describe('gateway API client', () => {
  it('reads setup status', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(
        new Response(
          JSON.stringify({
            setup_required: true,
            physical_window_active: false,
            remaining_seconds: 0,
          }),
          { status: 200, headers: { 'Content-Type': 'application/json' } },
        ),
      ),
    )
    await expect(api.setupStatus()).resolves.toMatchObject({ setup_required: true })
    expect(fetch).toHaveBeenCalledWith(
      '/ui/setup',
      expect.objectContaining({ signal: expect.any(AbortSignal) }),
    )
  })
  it('preserves firmware error codes', async () => {
    vi.stubGlobal(
      'fetch',
      vi
        .fn()
        .mockResolvedValue(
          new Response(JSON.stringify({ error: 'physical_setup_required' }), { status: 403 }),
        ),
    )
    await expect(api.setup({ username: 'admin', password: 'password' })).rejects.toEqual(
      new ApiError(403, 'physical_setup_required'),
    )
  })
  it('maps network errors', () => expect(errorCode(new TypeError('fetch failed'))).toBe('network'))
  it('marks the gateway unreachable only on transport failures', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('fetch failed')))
    await expect(api.status()).rejects.toThrow(TypeError)
    expect(gatewayReachable.value).toBe(false)

    vi.stubGlobal(
      'fetch',
      vi
        .fn()
        .mockResolvedValue(
          new Response(JSON.stringify({ error: 'authentication_required' }), { status: 401 }),
        ),
    )
    await expect(api.status()).rejects.toBeInstanceOf(ApiError)
    expect(gatewayReachable.value).toBe(true)
  })
  describe('a lost session', () => {
    const json = (body: unknown, status = 200) =>
      new Response(JSON.stringify(body), {
        status,
        headers: { 'Content-Type': 'application/json' },
      })
    const session = { user: { id: 1, username: 'admin', role: 'admin' }, csrf_token: 'csrf-value' }
    const lost = vi.fn()
    afterEach(() => {
      lost.mockReset()
      onSessionLost(() => {})
    })

    it('is reported once when a signed-in request comes back 401', async () => {
      onSessionLost(lost)
      vi.stubGlobal(
        'fetch',
        vi
          .fn()
          .mockResolvedValueOnce(json(session))
          .mockResolvedValue(json({ error: 'authentication_required' }, 401)),
      )
      await api.session()
      await Promise.allSettled([api.poll.nodes(), api.poll.status()])
      expect(lost).toHaveBeenCalledTimes(1)
      expect(sessionUser.value).toBeNull()
    })

    it('is not reported while signed out or for a rejected password', async () => {
      onSessionLost(lost)
      vi.stubGlobal(
        'fetch',
        vi.fn().mockResolvedValue(json({ error: 'authentication_required' }, 401)),
      )
      await expect(api.session()).rejects.toBeInstanceOf(ApiError)

      vi.stubGlobal(
        'fetch',
        vi
          .fn()
          .mockResolvedValueOnce(json(session))
          .mockResolvedValue(json({ error: 'invalid_credentials' }, 401)),
      )
      await api.session()
      await expect(api.login('admin', 'wrong')).rejects.toEqual(
        new ApiError(401, 'invalid_credentials'),
      )
      expect(lost).not.toHaveBeenCalled()
      expect(sessionUser.value).not.toBeNull()
    })
  })
  it('uses the session CSRF token for logout', async () => {
    const fetchMock = vi
      .fn()
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            user: { id: 1, username: 'admin', role: 'admin' },
            csrf_token: 'csrf-value',
          }),
          { status: 200, headers: { 'Content-Type': 'application/json' } },
        ),
      )
      .mockResolvedValueOnce(new Response(null, { status: 204 }))
    vi.stubGlobal('fetch', fetchMock)
    await api.login('admin', 'password')
    await api.logout()
    expect(fetchMock).toHaveBeenLastCalledWith(
      '/ui/session',
      expect.objectContaining({
        method: 'DELETE',
        credentials: 'same-origin',
        headers: expect.objectContaining({ 'X-CSRF-Token': 'csrf-value' }),
      }),
    )
  })
  it('sends CSRF and only a name when creating a Home Assistant token', async () => {
    const fetchMock = vi
      .fn()
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            user: { id: 1, username: 'admin', role: 'admin' },
            csrf_token: 'csrf-value',
          }),
          { status: 200, headers: { 'Content-Type': 'application/json' } },
        ),
      )
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            id: 1,
            name: 'Home Assistant',
            enabled: true,
            created_at_ms: 1,
            scopes: ['telemetry:read'],
            token: 'secret',
          }),
          { status: 201, headers: { 'Content-Type': 'application/json' } },
        ),
      )
    vi.stubGlobal('fetch', fetchMock)
    await api.login('admin', 'password')
    await api.createToken('Home Assistant')
    expect(fetchMock).toHaveBeenLastCalledWith(
      '/ui/tokens',
      expect.objectContaining({
        method: 'POST',
        headers: expect.objectContaining({ 'X-CSRF-Token': 'csrf-value' }),
        body: JSON.stringify({ name: 'Home Assistant' }),
      }),
    )
  })
})
