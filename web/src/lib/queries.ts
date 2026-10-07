import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { useCallback, useSyncExternalStore } from 'react'

import { api, ApiError, type SpotQuery } from './api'
import type { User } from './types'

// Pushes keep crowd colors current; this slow poll is the safety net for a
// missed push (a dropped socket, or the server-side subscription loss noted
// in web/README.md).
const LIVE_FALLBACK_MS = 60_000

export const keys = {
  campuses: ['campuses'] as const,
  campus: (slug: string) => ['campus', slug] as const,
  spots: (q: SpotQuery) => ['spots', q] as const,
  spot: (id: number) => ['spot', id] as const,
  me: ['me'] as const,
}

export function useCampuses() {
  return useQuery({ queryKey: keys.campuses, queryFn: () => api.campuses(), staleTime: Infinity })
}

export function useCampus(slug: string | null) {
  return useQuery({
    queryKey: keys.campus(slug ?? ''),
    queryFn: () => api.campus(slug!),
    enabled: !!slug,
    staleTime: 5 * 60_000,
  })
}

export function useSpots(q: SpotQuery, enabled = true) {
  return useQuery({
    queryKey: keys.spots(q),
    queryFn: () => api.spots(q),
    enabled,
    placeholderData: keepPreviousData,
    refetchInterval: LIVE_FALLBACK_MS,
  })
}

export function useSpot(id: number, enabled = true) {
  return useQuery({
    queryKey: keys.spot(id),
    queryFn: () => api.spot(id).then((r) => r.spot),
    enabled,
    refetchInterval: LIVE_FALLBACK_MS,
  })
}

/** The logged-in user, or null when anonymous. */
export function useMe() {
  return useQuery<User | null>({
    queryKey: keys.me,
    queryFn: async () => {
      try {
        return (await api.me()).user
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) return null
        throw e
      }
    },
    staleTime: 60_000,
    retry: false,
  })
}

// The selected campus lives in localStorage so it survives reloads.
const CAMPUS_KEY = 'fss.campus'
const campusListeners = new Set<() => void>()

function readCampus(): string | null {
  try {
    return localStorage.getItem(CAMPUS_KEY)
  } catch {
    return null
  }
}

export function useSelectedCampus(): [string | null, (slug: string) => void] {
  const stored = useSyncExternalStore(
    (fn) => {
      campusListeners.add(fn)
      return () => campusListeners.delete(fn)
    },
    readCampus,
    () => null,
  )
  const campuses = useCampuses()
  const me = useMe()
  const list = campuses.data?.campuses ?? []
  const valid = (s: string | null | undefined) => (s && list.some((c) => c.slug === s) ? s : null)
  const slug = valid(stored) ?? valid(me.data?.campus) ?? list[0]?.slug ?? null
  const set = useCallback((s: string) => {
    try {
      localStorage.setItem(CAMPUS_KEY, s)
    } catch {
      // Private mode: the choice lasts for this page only.
    }
    for (const fn of campusListeners) fn()
  }, [])
  return [slug, set]
}
