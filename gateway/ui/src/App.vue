<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { useRouter } from 'vue-router'
import { api, gatewayReachable, isAdmin } from './api/client'
import Icon from './components/Icon.vue'
import LanguageSelect from './components/LanguageSelect.vue'
import ThemeToggle from './components/ThemeToggle.vue'

const route = useRoute()
const router = useRouter()
const uiVersion = __UI_VERSION__
const hostname = ref('')
const showChrome = computed(() => route.meta.chrome !== false)

async function loadIdentity() {
  if (!showChrome.value) return
  try {
    hostname.value = (await api.info()).hostname
  } catch {
    // The banner already reports an unreachable gateway; keep the last name.
  }
}

async function signOut() {
  try {
    await api.logout()
  } catch {
    /* Expire the local navigation state anyway. */
  }
  await router.replace('/login')
}

onMounted(loadIdentity)
watch(showChrome, loadIdentity)
</script>

<template>
  <div
    v-if="showChrome"
    class="app-layout"
  >
    <aside class="sidebar">
      <RouterLink
        class="brand"
        to="/status"
        aria-label="OSK Sense Hub"
      >
        <span
          class="brand-mark"
          aria-hidden="true"
          ><i></i><i></i><i></i
        ></span>
        <strong class="brand-name">OSK Sense Hub</strong>
      </RouterLink>

      <nav
        class="primary-nav"
        :aria-label="$t('nav.main')"
      >
        <RouterLink to="/status"
          ><span
            class="nav-icon"
            aria-hidden="true"
            ><Icon name="view-dashboard" /></span
          ><span>{{ $t('nav.overview') }}</span></RouterLink
        >
        <RouterLink to="/nodes"
          ><span
            class="nav-icon"
            aria-hidden="true"
            ><Icon name="access-point" /></span
          ><span>{{ $t('nav.nodes') }}</span></RouterLink
        >
        <RouterLink to="/settings"
          ><span
            class="nav-icon"
            aria-hidden="true"
            ><Icon name="cog" /></span
          ><span>{{ $t('nav.settings') }}</span></RouterLink
        >
        <RouterLink
          v-if="isAdmin"
          to="/admin"
          ><span
            class="nav-icon"
            aria-hidden="true"
            ><Icon name="shield-account" /></span
          ><span>{{ $t('nav.admin') }}</span></RouterLink
        >
      </nav>
    </aside>

    <div class="workspace">
      <header class="topbar">
        <span class="gateway-address">{{ hostname }}</span>
        <div class="topbar-actions">
          <LanguageSelect compact /><ThemeToggle /><button
            class="icon-button outlined"
            type="button"
            :aria-label="$t('shell.signOut')"
            :title="$t('shell.signOut')"
            @click="signOut"
          >
            <Icon name="logout" />
          </button>
        </div>
      </header>
      <div
        v-if="!gatewayReachable"
        class="offline-banner"
        role="status"
      >
        <Icon name="alert" /><span>{{ $t('shell.offline') }}</span>
      </div>
      <main><RouterView /></main>
    </div>
  </div>

  <div
    v-else
    class="standalone-layout"
  >
    <header class="standalone-header">
      <RouterLink
        class="brand"
        to="/"
        aria-label="OSK Sense Hub"
      >
        <span
          class="brand-mark"
          aria-hidden="true"
          ><i></i><i></i><i></i
        ></span>
        <strong class="brand-name">OSK Sense Hub</strong>
      </RouterLink>
      <div class="standalone-actions"><LanguageSelect compact /><ThemeToggle /></div>
    </header>
    <div
      v-if="!gatewayReachable"
      class="offline-banner"
      role="status"
    >
      <Icon name="alert" /><span>{{ $t('shell.offline') }}</span>
    </div>
    <main><RouterView /></main>
    <footer>{{ $t('common.uiVersion', { version: uiVersion }) }}</footer>
  </div>
</template>
