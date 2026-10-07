import type { SpotQuery } from './api'

export interface Filters {
  q: string
  must: string[]
  not: string[]
  /** 0 = any, 3 = silent only (server maps it to a max noise level). */
  quiet: number
  vibe: string
  near: boolean
}

export const EMPTY_FILTERS: Filters = { q: '', must: [], not: [], quiet: 0, vibe: '', near: false }

const list = (s: string | null) => (s ? s.split(',').filter(Boolean) : [])

export function parseFilters(p: URLSearchParams): Filters {
  const quiet = Number(p.get('quiet'))
  return {
    q: p.get('q') ?? '',
    must: list(p.get('must')),
    not: list(p.get('not')),
    quiet: Number.isInteger(quiet) && quiet >= 0 && quiet <= 3 ? quiet : 0,
    vibe: p.get('vibe') ?? '',
    near: p.get('near') === '1',
  }
}

export function serializeFilters(f: Filters): URLSearchParams {
  const p = new URLSearchParams()
  if (f.q) p.set('q', f.q)
  if (f.must.length) p.set('must', f.must.join(','))
  if (f.not.length) p.set('not', f.not.join(','))
  if (f.quiet) p.set('quiet', String(f.quiet))
  if (f.vibe) p.set('vibe', f.vibe)
  if (f.near) p.set('near', '1')
  return p
}

export function activeFilterCount(f: Filters): number {
  return f.must.length + f.not.length + (f.quiet ? 1 : 0) + (f.vibe ? 1 : 0) + (f.near ? 1 : 0)
}

export type FlagState = 'any' | 'must' | 'not'

export function flagState(f: Filters, key: string): FlagState {
  if (f.must.includes(key)) return 'must'
  if (f.not.includes(key)) return 'not'
  return 'any'
}

/** any → must → not → any. `canExclude` is false for ordinals like outlets. */
export function cycleFlag(f: Filters, key: string, canExclude = true): Filters {
  const state = flagState(f, key)
  const must = f.must.filter((k) => k !== key)
  const not = f.not.filter((k) => k !== key)
  if (state === 'any') must.push(key)
  else if (state === 'must' && canExclude) not.push(key)
  return { ...f, must, not }
}

export function toSpotQuery(
  f: Filters,
  campus: string,
  pos?: { lat: number; lon: number },
): SpotQuery {
  return {
    campus,
    q: f.q || undefined,
    must: f.must,
    not: f.not,
    quiet: f.quiet || undefined,
    vibe: f.vibe || undefined,
    near: f.near ? pos : undefined,
    limit: 50,
  }
}
