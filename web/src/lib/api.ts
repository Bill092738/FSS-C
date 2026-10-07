import type {
  Campus,
  CampusSummary,
  CheckinResult,
  ClaimValue,
  ClaimVoteResult,
  ConfirmResult,
  EventKind,
  KarmaPage,
  PhotoResult,
  ReportResult,
  SpotDetail,
  SpotList,
  User,
} from './types'

const BASE = '/api/v1'

/** The backend's error envelope: `{"error":{"code":"...","message":"..."}}`. */
export class ApiError extends Error {
  readonly status: number
  readonly code: string
  readonly extra: Record<string, unknown>

  constructor(status: number, code: string, message: string, extra: Record<string, unknown> = {}) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.extra = extra
  }
}

/** A request body sent as is (photo uploads) instead of JSON. */
export interface RawBody {
  raw: Blob
  type: string
}

export async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const init: RequestInit = { method, credentials: 'same-origin', headers: {} }
  if (body instanceof Object && 'raw' in body && body.raw instanceof Blob) {
    const { raw, type } = body as RawBody
    init.headers = { 'Content-Type': type }
    init.body = raw
  } else if (body !== undefined) {
    // Every write endpoint requires this content type (roadmap 10.5).
    init.headers = { 'Content-Type': 'application/json' }
    init.body = JSON.stringify(body)
  }
  let res: Response
  try {
    res = await fetch(BASE + path, init)
  } catch {
    throw new ApiError(0, 'network', 'Cannot reach the FSS server. Is it running?')
  }
  const text = await res.text()
  let data: unknown = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      // A proxy error page or similar; handled below.
    }
  }
  if (!res.ok) {
    const err = (data as { error?: { code?: string; message?: string } } | null)?.error
    if (err?.code) {
      const { code, message, ...extra } = err
      throw new ApiError(res.status, code, message ?? code, extra)
    }
    throw new ApiError(res.status, 'http_' + res.status, res.statusText || 'Request failed')
  }
  return data as T
}

export interface SpotQuery {
  campus?: string
  bbox?: string
  must?: string[]
  not?: string[]
  quiet?: number
  vibe?: string
  near?: { lat: number; lon: number }
  q?: string
  limit?: number
  cursor?: string
}

export function spotQueryString(q: SpotQuery): string {
  const p = new URLSearchParams()
  if (q.campus) p.set('campus', q.campus)
  if (q.bbox) p.set('bbox', q.bbox)
  if (q.must?.length) p.set('must', q.must.join(','))
  if (q.not?.length) p.set('not', q.not.join(','))
  if (q.quiet !== undefined && q.quiet > 0) p.set('quiet', String(q.quiet))
  if (q.vibe) p.set('vibe', q.vibe)
  if (q.near) p.set('near', `${q.near.lat.toFixed(6)},${q.near.lon.toFixed(6)}`)
  if (q.q?.trim()) p.set('q', q.q.trim())
  if (q.limit) p.set('limit', String(q.limit))
  if (q.cursor) p.set('cursor', q.cursor)
  const s = p.toString()
  return s ? '?' + s : ''
}

export interface Position {
  lat: number
  lon: number
  accuracy_m: number
}

export interface ProfilePatch {
  display_name?: string
  major?: string
  langs?: string[]
  courses?: string[]
  campus?: string
}

export interface NewSpot {
  building_id: number
  name: string
  floor?: string
  claims?: { attr: string; value: ClaimValue; evidence?: string }[]
}

/** Image types POST /spots/:id/photos accepts (checked by magic bytes). */
export const PHOTO_TYPES = ['image/jpeg', 'image/png', 'image/webp']
export const PHOTO_MAX_BYTES = 5 << 20

/** Endpoints the backend implements today (M0, M2, M3, M4). */
export const api = {
  health: () => request<{ ok: boolean; schema: number; now: number }>('GET', '/health'),
  campuses: () => request<{ campuses: CampusSummary[] }>('GET', '/campuses'),
  campus: (slug: string) => request<Campus>('GET', `/campuses/${encodeURIComponent(slug)}`),

  register: (b: { email: string; password: string; display_name?: string; campus?: string }) =>
    request<{ user: User }>('POST', '/auth/register', b),
  login: (b: { email: string; password: string }) => request<{ user: User }>('POST', '/auth/login', b),
  logout: () => request<unknown>('POST', '/auth/logout', {}),
  me: () => request<{ user: User }>('GET', '/me'),
  updateMe: (b: ProfilePatch) => request<{ user: User }>('PATCH', '/me', b),

  spots: (q: SpotQuery) => request<SpotList>('GET', '/spots' + spotQueryString(q)),
  spot: (id: number) => request<{ spot: SpotDetail }>('GET', `/spots/${id}`),
  createSpot: (b: NewSpot) => request<{ spot: SpotDetail }>('POST', '/spots', b),
  createClaim: (spotId: number, b: { attr: string; value: ClaimValue; evidence?: string }) =>
    request<{ spot: SpotDetail }>('POST', `/spots/${spotId}/claims`, b),
  vote: (claimId: number, v: 1 | -1 | 0) => request<ClaimVoteResult>('POST', `/claims/${claimId}/vote`, { v }),
  reportLevel: (spotId: number, level: 0 | 1 | 2, pos?: Position) =>
    request<ReportResult>('POST', `/spots/${spotId}/reports`, { level, ...pos }),
  reportEvent: (spotId: number, event: EventKind, pos?: Position) =>
    request<ReportResult>('POST', `/spots/${spotId}/reports`, { event, ...pos }),

  checkin: (spotId: number, pos?: Position) => request<CheckinResult>('POST', '/checkins', { spot_id: spotId, ...pos }),
  heartbeat: (id: number, pos?: Position) => request<CheckinResult>('POST', `/checkins/${id}/heartbeat`, { ...pos }),
  endCheckin: (id: number) => request<CheckinResult>('POST', `/checkins/${id}/end`, {}),
  confirmSpot: (spotId: number) => request<ConfirmResult>('POST', `/spots/${spotId}/confirm`, {}),
  uploadPhoto: (spotId: number, file: Blob) =>
    request<PhotoResult>('POST', `/spots/${spotId}/photos`, { raw: file, type: file.type } satisfies RawBody),
  votePhoto: (photoId: number, v: 1 | -1 | 0) => request<PhotoResult>('POST', `/photos/${photoId}/vote`, { v }),
  karma: (cursor?: string) =>
    request<KarmaPage>('GET', '/me/karma' + (cursor ? `?cursor=${encodeURIComponent(cursor)}` : '')),
}
