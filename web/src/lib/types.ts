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

export interface Photo {
  id: number
  /** Uploader; users cannot vote on their own photos. */
  user: number
  url: string
  up: number
  down: number
  at: number
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
  /** Open verified check-ins (roadmap 7.4). */
  present: number
  /** Confirmations of a submitted spot; it turns active at 2. */
  confirmations: number
  /** Visible photos, best voted first (at most 20). */
  photos: Photo[]
  updated_at: number
}

export type CheckinEndReason = 'user' | 'outside' | 'timeout' | 'stale'

export interface Checkin {
  id: number
  spot: number
  start_at: number
  end_at: number | null
  end_reason: CheckinEndReason | null
  last_beat_at: number | null
  in_fence: boolean
  verified: boolean
  /** Time credited between two in-fence positions. */
  verified_ms: number
  outside_beats: number
}

/** The open check-in as embedded in GET /me. */
export type OpenCheckin = Pick<Checkin, 'id' | 'spot' | 'start_at' | 'last_beat_at' | 'verified' | 'verified_ms'>

export interface CheckinResult {
  checkin: Checkin
  live?: LiveState
  /** Points credited when the session ended. */
  karma?: number
}

export interface PhotoResult {
  photo: Photo & { spot: number; p: number; status: 'visible' | 'hidden' }
  karma?: number
}

export interface ConfirmResult {
  spot_id: number
  status: 'active' | 'hidden'
  confirmations: number
  needed: number
}

export type KarmaReason =
  | 'report'
  | 'event_confirmed'
  | 'photo'
  | 'photo_hidden'
  | 'claim_accepted'
  | 'spot_discovered'
  | 'checkin'

export interface KarmaEntry {
  id: number
  delta: number
  reason: KarmaReason
  ref_type: string
  ref_id: number
  at: number
}

export interface KarmaPage {
  karma: number
  entries: KarmaEntry[]
  next: string | null
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
  checkin: OpenCheckin | null
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
  /** Karma credited for this report (roadmap 7.5). */
  karma: number
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
  | { t: 'karma'; delta: number; total: number; reason?: KarmaReason; at?: number }
  | { t: 'badge'; key: string; name: string; at?: number }
  | { t: 'ok'; op: string; ch?: string; buildings?: number[] }
  | { t: 'error'; op?: string; code: string; message: string }
