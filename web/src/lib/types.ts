// Shapes returned by the C backend (server/src/api_*.c). Field names follow the
// JSON exactly; keep this file in sync when an endpoint changes.

export type Bbox = [minLon: number, minLat: number, maxLon: number, maxLat: number]

export interface CampusSummary {
  slug: string
  name: string
  tz: string
  bbox: Bbox
}

export type AttrKind = 'flag' | 'ordinal' | 'enum' | 'text'
export type AttrGroup = 'location' | 'infra' | 'rules' | 'vibe'

export interface AttrDef {
  key: string
  kind: AttrKind
  group: AttrGroup
  /** Bit in `spot.features`; flags only. */
  bit: number | null
  /** `{min,max}` for ordinals, the allowed values for enums, null otherwise. */
  domain: { min: number; max: number } | string[] | null
  label_en: string
  label_zh: string
  sort: number
}

export interface Campus extends CampusSummary {
  calendar: unknown
  buildings: number
  spots: number
  attrs: AttrDef[]
}

export type CrowdColor = 'green' | 'yellow' | 'red'
export type LiveBasis = 'reports' | 'forecast'

export interface LiveState {
  est: number
  conf: number
  basis: LiveBasis
  color: CrowdColor
  /** Epoch ms of the last update; detail and report responses only. */
  at?: number
}

export type Vibe = 'deep_work' | 'collab' | 'casual'

interface SpotBase {
  id: number
  name: string
  floor: string | null
  lat: number
  lon: number
  features: number
  noise: number | null
  outlets: number | null
  temp: number | null
  capacity: number | null
  vibe: Vibe | null
  quality: number
  live: LiveState | null
}

export interface SpotListItem extends SpotBase {
  building: { id: number; name: string }
  dist_m: number | null
  score: number
}

export interface SpotList {
  spots: SpotListItem[]
  next: string | null
}

export type ClaimValue = boolean | number | string

export interface Claim {
  id: number
  attr: string
  value: ClaimValue
  source: 'llm' | 'official' | 'user'
  evidence: string | null
  url: string | null
  p: number
  up: number
  down: number
  at: number
}

export type EventKind = 'outlet_broken' | 'wifi_down' | 'closed_event'

export interface SpotEvent {
  kind: EventKind
  reports: number
  until: number
}

export interface SpotDetail extends SpotBase {
  status: 'active' | 'hidden' | 'merged'
  campus: string
  building: { id: number; name: string; lat: number; lon: number; hours: unknown }
  /** Materialized attributes: value and confidence of the winning claim. */
  attrs: Record<string, { v: ClaimValue; p: number }>
  summary: string | null
  pros: string[]
  cons: string[]
  events: SpotEvent[]
  claims: Claim[]
  updated_at: number
}

export interface Badge {
  key: string
  name: string
  at: number
}

export interface User {
  id: number
  email: string
  display_name: string | null
  campus: string | null
  major: string | null
  langs: string[]
  courses: string[]
  role: 'student' | 'merchant' | 'campus_admin'
  reputation: number
  karma: number
  badges: Badge[]
}

export interface ReportResult {
  report: {
    id: number
    spot: number
    level?: 0 | 1 | 2
    event?: EventKind
    in_fence: boolean
    fence: number
    weight: number
    at: number
  }
  live?: LiveState
  event?: { kind: EventKind; visible: boolean; reports: number; until: number }
}

export interface ClaimVoteResult {
  claim: Pick<Claim, 'id' | 'attr' | 'value' | 'p' | 'up' | 'down'>
}

/** Messages pushed on the `/ws` socket (roadmap 9.2). */
export type ServerMessage =
  | { t: 'live'; spot: number; color: CrowdColor; est: number; conf: number; basis: LiveBasis; at: number }
  | { t: 'event'; spot: number; kind: EventKind; until: number; reports: number; at: number }
  | { t: 'karma'; delta: number; total: number; at?: number }
  | { t: 'ok'; op: string; ch?: string; buildings?: number[] }
  | { t: 'error'; op?: string; code: string; message: string }
