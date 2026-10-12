import { describe, expect, it, vi } from 'vitest'
import { resolveRouteAccess, type RouteAccessApi } from './access'

function gatewayApi(setupRequired: boolean, role?: 'admin' | 'viewer'): RouteAccessApi {
  return {
    setupStatus: vi.fn().mockResolvedValue({
      setup_required: setupRequired,
      physical_window_active: false,
      remaining_seconds: 0,
    }),
    session: role
      ? vi
          .fn()
          .mockResolvedValue({ user: { id: 1, username: 'someone', role }, csrf_token: 'csrf' })
      : vi.fn().mockRejectedValue(new Error('authentication_required')),
  } as RouteAccessApi
}

describe('gateway route access', () => {
  it('redirects a stale login URL to setup after a factory reset', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/login', fullPath: '/login', requiresAuth: false },
        gatewayApi(true),
      ),
    ).resolves.toEqual({ path: '/setup' })
  })

  it('keeps login available after setup is complete', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/login', fullPath: '/login', requiresAuth: false },
        gatewayApi(false),
      ),
    ).resolves.toBe(true)
  })

  it('redirects an unauthenticated protected URL to setup when required', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/nodes', fullPath: '/nodes', requiresAuth: true },
        gatewayApi(true),
      ),
    ).resolves.toEqual({ path: '/setup' })
  })

  it('preserves the requested URL when normal login is required', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/nodes', fullPath: '/nodes?selected=2', requiresAuth: true },
        gatewayApi(false),
      ),
    ).resolves.toEqual({ path: '/login', query: { redirect: '/nodes?selected=2' } })
  })

  it('lets an admin into an admin-only URL', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/admin', fullPath: '/admin', requiresAuth: true, requiresAdmin: true },
        gatewayApi(false, 'admin'),
      ),
    ).resolves.toBe(true)
  })

  it('sends a signed-in viewer to the overview instead of login', async () => {
    // Signing in again would not grant the role, so /login is the wrong answer.
    await expect(
      resolveRouteAccess(
        { path: '/admin', fullPath: '/admin', requiresAuth: true, requiresAdmin: true },
        gatewayApi(false, 'viewer'),
      ),
    ).resolves.toEqual({ path: '/status' })
  })

  it('still sends an unauthenticated visitor on an admin URL to login', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/admin', fullPath: '/admin', requiresAuth: true, requiresAdmin: true },
        gatewayApi(false),
      ),
    ).resolves.toEqual({ path: '/login', query: { redirect: '/admin' } })
  })

  // The connection page is admin-only for the same reason as /admin: its one
  // action is creating a key.
  it('guards the Home Assistant connection page like the admin page', async () => {
    const target = {
      path: '/home-assistant',
      fullPath: '/home-assistant',
      requiresAuth: true,
      requiresAdmin: true,
    }
    await expect(resolveRouteAccess(target, gatewayApi(false, 'admin'))).resolves.toBe(true)
    await expect(resolveRouteAccess(target, gatewayApi(false, 'viewer'))).resolves.toEqual({
      path: '/status',
    })
    await expect(resolveRouteAccess(target, gatewayApi(false))).resolves.toEqual({
      path: '/login',
      query: { redirect: '/home-assistant' },
    })
  })

  it('leaves a viewer alone on a normal protected URL', async () => {
    await expect(
      resolveRouteAccess(
        { path: '/nodes', fullPath: '/nodes', requiresAuth: true },
        gatewayApi(false, 'viewer'),
      ),
    ).resolves.toBe(true)
  })
})
