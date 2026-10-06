import { useMutation, useQueryClient } from '@tanstack/react-query'

import { api, type ProfilePatch } from './api'
import { useLive } from './liveContext'
import { keys } from './queries'

/**
 * Login, registration and logout. Each one also reconnects the WebSocket,
 * because the server reads the session cookie only at upgrade time (that is
 * when it attaches the private `user:{id}` channel).
 */
export function useAuthActions() {
  const qc = useQueryClient()
  const live = useLive()
  const signedIn = (user: Awaited<ReturnType<typeof api.me>>['user'] | null) => {
    qc.setQueryData(keys.me, user)
    live.restart()
  }
  return {
    login: useMutation({
      mutationFn: api.login,
      onSuccess: (r) => signedIn(r.user),
    }),
    register: useMutation({
      mutationFn: api.register,
      onSuccess: (r) => signedIn(r.user),
    }),
    logout: useMutation({
      mutationFn: api.logout,
      onSuccess: () => signedIn(null),
    }),
    updateProfile: useMutation({
      mutationFn: (b: ProfilePatch) => api.updateMe(b),
      onSuccess: (r) => qc.setQueryData(keys.me, r.user),
    }),
  }
}
