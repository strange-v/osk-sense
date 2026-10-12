import { api } from '../api/client'

export interface RouteAccessTarget {
  path: string
  fullPath: string
  requiresAuth: boolean
  requiresAdmin?: boolean
}

export interface RouteAccessApi {
  setupStatus: typeof api.setupStatus
  session: typeof api.session
}

export type RouteAccessResult =
  | true
  | {
      path: string
      query?: Record<string, string>
    }

export async function resolveRouteAccess(
  target: RouteAccessTarget,
  gatewayApi: RouteAccessApi = api,
): Promise<RouteAccessResult> {
  // The hostname survives a factory reset, so a browser can reopen the last
  // visited /login URL even though the gateway now needs first-time setup.
  if (target.path === '/login') {
    try {
      if ((await gatewayApi.setupStatus()).setup_required) return { path: '/setup' }
    } catch {
      // Keep the login page available while the gateway is temporarily offline.
    }
    return true
  }

  if (!target.requiresAuth) return true

  try {
    const session = await gatewayApi.session()
    // A viewer reaching an admin URL is signed in, just not entitled: send it
    // to the overview rather than to login, which would ask it to fix a
    // problem that signing in again cannot fix. The gateway rejects the
    // requests too; this only keeps the UI honest about it.
    if (target.requiresAdmin && session.user.role !== 'admin') return { path: '/status' }
    return true
  } catch {
    try {
      if ((await gatewayApi.setupStatus()).setup_required) return { path: '/setup' }
    } catch {
      // The login page already has the normal connection error handling.
    }
    return { path: '/login', query: { redirect: target.fullPath } }
  }
}
