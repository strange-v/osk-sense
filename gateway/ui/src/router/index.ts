import { createRouter, createWebHistory } from 'vue-router'
import { onSessionLost } from '../api/client'
import { resolveRouteAccess } from './access'
import HomeView from '../views/HomeView.vue'
import SetupView from '../views/SetupView.vue'
import LoginView from '../views/LoginView.vue'
import StatusView from '../views/StatusView.vue'
import NodesView from '../views/NodesView.vue'
import SettingsView from '../views/SettingsView.vue'

// Views are imported eagerly: the production build ships a single application
// bundle (see vite.config.ts), so a lazy route deferred nothing and only cost
// a chunk boundary. Two exceptions earn their chunk -- the pairing QR scanner
// (jsQR), and administration below: users, API tokens, their dialogs and the
// Home Assistant connection page, ~6 KB gzip that only an admin ever needs.
//
// Both admin routes import the same module so they share one chunk. Importing
// each view directly gave them a file each plus a third for what they share;
// see src/views/admin.ts.
export const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', component: HomeView, meta: { chrome: false } },
    { path: '/setup', component: SetupView, meta: { chrome: false } },
    { path: '/login', component: LoginView, meta: { chrome: false } },
    { path: '/status', component: StatusView, meta: { requiresAuth: true } },
    { path: '/nodes', component: NodesView, meta: { requiresAuth: true } },
    { path: '/settings', component: SettingsView, meta: { requiresAuth: true } },
    {
      path: '/admin',
      component: () => import('../views/admin').then((views) => views.AdminView),
      meta: { requiresAuth: true, requiresAdmin: true },
    },
    // Admin-only because its one action is creating a key.
    {
      path: '/home-assistant',
      component: () => import('../views/admin').then((views) => views.HomeAssistantView),
      meta: { requiresAuth: true, requiresAdmin: true },
    },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
})

onSessionLost(() => {
  const current = router.currentRoute.value
  if (current.meta.requiresAuth !== true) return
  router.replace({ path: '/login', query: { redirect: current.fullPath } })
})

router.beforeEach(async (to) => {
  return resolveRouteAccess({
    path: to.path,
    fullPath: to.fullPath,
    requiresAuth: to.meta.requiresAuth === true,
    requiresAdmin: to.meta.requiresAdmin === true,
  })
})
