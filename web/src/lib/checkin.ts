import { useMutation, useQueryClient, type QueryClient } from '@tanstack/react-query'
import { useEffect, useState } from 'react'

import { api, ApiError } from './api'
import { getPosition } from './geo'
import { keys, useMe } from './queries'
import type { Checkin, CheckinResult, OpenCheckin, SpotDetail, User } from './types'

/** Roadmap 7.4: the client sends a heartbeat with its position every 10 minutes. */
export const HEARTBEAT_MS = 10 * 60_000
/** Wait after a failed heartbeat (offline, server restart) before trying again. */
export const HEARTBEAT_RETRY_MS = 60_000

/** Milliseconds until the next heartbeat is due (0 when overdue). */
export function nextBeatDelay(c: Pick<OpenCheckin, 'start_at' | 'last_beat_at'>, now: number): number {
  const last = c.last_beat_at ?? c.start_at
  return Math.max(0, last + HEARTBEAT_MS - now)
}

function asOpen(c: Checkin): OpenCheckin | null {
  if (c.end_at !== null) return null
  const { id, spot, start_at, last_beat_at, verified, verified_ms } = c
  return { id, spot, start_at, last_beat_at, verified, verified_ms }
}

/** Mirrors a check-in response into the cached profile and spot. */
export function applyCheckin(qc: QueryClient, r: CheckinResult): void {
  qc.setQueryData<User | null>(keys.me, (old) => (old ? { ...old, checkin: asOpen(r.checkin) } : old))
  if (r.live) qc.setQueryData<SpotDetail>(keys.spot(r.checkin.spot), (old) => (old ? { ...old, live: r.live! } : old))
  void qc.invalidateQueries({ queryKey: keys.spot(r.checkin.spot) })
  if (r.karma) void qc.invalidateQueries({ queryKey: keys.me })
}

export function useCheckinActions() {
  const qc = useQueryClient()
  const apply = (r: CheckinResult) => applyCheckin(qc, r)
  // A session that ended elsewhere (another tab, the timeout job): re-read it.
  const resync = (e: unknown) => {
    if (e instanceof ApiError && ['checkin_open', 'checkin_ended', 'not_found'].includes(e.code))
      void qc.invalidateQueries({ queryKey: keys.me })
  }
  return {
    start: useMutation({
      mutationFn: async (spotId: number) => api.checkin(spotId, await getPosition()),
      onSuccess: apply,
      onError: resync,
    }),
    end: useMutation({
      mutationFn: (id: number) => api.endCheckin(id),
      onSuccess: apply,
      onError: resync,
    }),
  }
}

/**
 * Sends heartbeats for the open check-in while the app is open, on any page.
 * The server ends the session after two heartbeats from outside the building
 * or 30 minutes without one, so a closed tab ends it on its own.
 */
export function useCheckinHeartbeat(): void {
  const me = useMe()
  const qc = useQueryClient()
  const [failures, setFailures] = useState(0)
  const open = me.data?.checkin ?? null
  const id = open?.id
  const lastBeat = open ? (open.last_beat_at ?? open.start_at) : 0

  useEffect(() => {
    if (!id) return
    let cancelled = false
    const timer = setTimeout(async () => {
      try {
        const r = await api.heartbeat(id, await getPosition())
        if (cancelled) return
        setFailures(0)
        applyCheckin(qc, r)
      } catch {
        if (cancelled) return
        // Ended elsewhere, offline, ...: the profile tells what is left, and
        // the next attempt waits a minute.
        void qc.invalidateQueries({ queryKey: keys.me })
        setFailures((n) => n + 1)
      }
    }, Math.max(nextBeatDelay({ start_at: lastBeat, last_beat_at: lastBeat }, Date.now()), failures ? HEARTBEAT_RETRY_MS : 0))
    return () => {
      cancelled = true
      clearTimeout(timer)
    }
  }, [id, lastBeat, failures, qc])
}
