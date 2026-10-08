import type { QueryClient } from '@tanstack/react-query'
import { createContext, useContext, useEffect, useSyncExternalStore } from 'react'

import type { LiveSocket, LiveStatus } from './live'
import { keys } from './queries'
import type { LiveState, ServerMessage, SpotDetail, SpotList, User } from './types'

export const LiveContext = createContext<LiveSocket | null>(null)

/** Applies pushed messages to cached query data, so every view updates in place. */
export function applyToCache(qc: QueryClient, msg: ServerMessage): void {
  if (msg.t === 'live') {
    const live: LiveState = { est: msg.est, conf: msg.conf, basis: msg.basis, color: msg.color, at: msg.at }
    qc.setQueryData<SpotDetail>(keys.spot(msg.spot), (old) => (old ? { ...old, live } : old))
    qc.setQueriesData<SpotList>({ queryKey: ['spots'] }, (old) =>
      old && old.spots.some((s) => s.id === msg.spot)
        ? { ...old, spots: old.spots.map((s) => (s.id === msg.spot ? { ...s, live } : s)) }
        : old,
    )
  } else if (msg.t === 'event') {
    // Event visibility depends on server rules; refetch rather than guess.
    void qc.invalidateQueries({ queryKey: keys.spot(msg.spot) })
  } else if (msg.t === 'karma') {
    qc.setQueryData<User | null>(keys.me, (old) => (old ? { ...old, karma: msg.total } : old))
    void qc.invalidateQueries({ queryKey: keys.karma })
  } else if (msg.t === 'badge') {
    // Badges and reputation live on the profile; refetch it.
    void qc.invalidateQueries({ queryKey: keys.me })
  }
}

export function useLive(): LiveSocket {
  const live = useContext(LiveContext)
  if (!live) throw new Error('useLive outside <LiveProvider>')
  return live
}

export function useLiveStatus(): LiveStatus {
  const live = useLive()
  return useSyncExternalStore(
    (fn) => live.onStatus(fn),
    () => live.status,
  )
}

/** Keeps a channel such as `spot:42` subscribed while the component is mounted. */
export function useLiveChannel(ch: string | null): void {
  const live = useLive()
  useEffect(() => (ch ? live.subscribe(ch) : undefined), [live, ch])
}
