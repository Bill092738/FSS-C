import { QueryClient } from '@tanstack/react-query'
import { describe, expect, it } from 'vitest'

import { applyToCache } from './liveContext'
import { keys } from './queries'
import type { SpotDetail, SpotList, User } from './types'

const push = { t: 'live', spot: 1, color: 'red', est: 1.67, conf: 0.63, basis: 'reports', at: 5 } as const

describe('applyToCache', () => {
  it('updates the spot detail and every cached list containing the spot', () => {
    const qc = new QueryClient()
    qc.setQueryData(keys.spot(1), { id: 1, live: null } as unknown as SpotDetail)
    const q1 = keys.spots({ campus: 'demo' })
    const q2 = keys.spots({ campus: 'demo', quiet: 3 })
    const q3 = keys.spots({ campus: 'demo', must: ['whiteboard'] })
    qc.setQueryData(q1, { spots: [{ id: 1, live: null }, { id: 2, live: null }], next: null } as unknown as SpotList)
    qc.setQueryData(q2, { spots: [{ id: 1, live: null }], next: null } as unknown as SpotList)
    const other = { spots: [{ id: 3, live: null }], next: null } as unknown as SpotList
    qc.setQueryData(q3, other)

    applyToCache(qc, push)

    const live = { est: 1.67, conf: 0.63, basis: 'reports', color: 'red', at: 5 }
    expect(qc.getQueryData<SpotDetail>(keys.spot(1))?.live).toEqual(live)
    expect(qc.getQueryData<SpotList>(q1)?.spots.map((s) => s.live)).toEqual([live, null])
    expect(qc.getQueryData<SpotList>(q2)?.spots[0].live).toEqual(live)
    expect(qc.getQueryData(q3)).toBe(other) // untouched
  })

  it('updates karma for the logged-in user and refreshes the history', () => {
    const qc = new QueryClient()
    qc.setQueryData(keys.me, { id: 1, karma: 3 } as User)
    qc.setQueryData(keys.karma, { pages: [], pageParams: [] })
    applyToCache(qc, { t: 'karma', delta: 2, total: 5 })
    expect(qc.getQueryData<User>(keys.me)?.karma).toBe(5)
    expect(qc.getQueryState(keys.karma)?.isInvalidated).toBe(true)
  })

  it('refetches the profile when a badge arrives', () => {
    const qc = new QueryClient()
    qc.setQueryData(keys.me, { id: 1, badges: [] } as unknown as User)
    applyToCache(qc, { t: 'badge', key: 'first_report', name: 'First Report' })
    expect(qc.getQueryState(keys.me)?.isInvalidated).toBe(true)
  })
})
