import { describe, expect, it } from 'vitest'

import { cycleFlag, EMPTY_FILTERS, parseFilters, serializeFilters, toSpotQuery } from './filters'

describe('filters', () => {
  it('round-trips through the URL', () => {
    const f = { q: 'basement', must: ['whiteboard'], not: ['calls_allowed'], quiet: 2, vibe: 'collab', near: true }
    expect(parseFilters(serializeFilters(f))).toEqual(f)
    expect(serializeFilters(EMPTY_FILTERS).toString()).toBe('')
  })

  it('ignores invalid quiet levels', () => {
    expect(parseFilters(new URLSearchParams('quiet=9')).quiet).toBe(0)
    expect(parseFilters(new URLSearchParams('quiet=abc')).quiet).toBe(0)
  })

  it('cycles flags any → must → not → any', () => {
    let f = cycleFlag(EMPTY_FILTERS, 'whiteboard')
    expect(f).toMatchObject({ must: ['whiteboard'], not: [] })
    f = cycleFlag(f, 'whiteboard')
    expect(f).toMatchObject({ must: [], not: ['whiteboard'] })
    f = cycleFlag(f, 'whiteboard')
    expect(f).toMatchObject({ must: [], not: [] })
  })

  it('never excludes ordinals such as outlets', () => {
    const f = cycleFlag(cycleFlag(EMPTY_FILTERS, 'outlets', false), 'outlets', false)
    expect(f).toMatchObject({ must: [], not: [] })
  })

  it('only sends near when a position is known', () => {
    const f = { ...EMPTY_FILTERS, near: true }
    expect(toSpotQuery(f, 'demo').near).toBeUndefined()
    expect(toSpotQuery(f, 'demo', { lat: 1, lon: 2 }).near).toEqual({ lat: 1, lon: 2 })
  })
})
