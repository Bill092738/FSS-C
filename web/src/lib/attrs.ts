import type { AttrDef, ClaimValue, CrowdColor, EventKind } from './types'

// Labels for ordinal and enum values. The attribute registry (attr_def) has
// labels for keys only, so value labels live here; unknown keys fall back to
// the raw number.
const ORDINAL_LABELS: Record<string, string[]> = {
  noise: ['Silent', 'Quiet', 'Moderate', 'Lively', 'Loud'],
  outlets: ['None', 'Few', 'Some', 'Plenty'],
  wifi_quality: ['None', 'Poor', 'OK', 'Great'],
  crowd_typical: ['Usually empty', 'Usually moderate', 'Usually packed'],
}
const TEMP_LABELS: Record<string, string> = { '-2': 'Cold', '-1': 'Cool', '0': 'Comfortable', '1': 'Warm', '2': 'Hot' }
const ENUM_LABELS: Record<string, string> = { deep_work: 'Deep work', collab: 'Collaborative', casual: 'Casual' }

export function formatValue(key: string, v: ClaimValue | null | undefined): string {
  if (v === null || v === undefined) return 'Unknown'
  if (typeof v === 'boolean') return v ? 'Yes' : 'No'
  if (typeof v === 'string') return ENUM_LABELS[v] ?? v
  if (key === 'temp') return TEMP_LABELS[String(v)] ?? String(v)
  if (key === 'capacity') return `${v} seats`
  return ORDINAL_LABELS[key]?.[v] ?? String(v)
}

/** Choices for an attribute's value, for forms. Null for free text and wide ranges. */
export function valueOptions(def: AttrDef): { value: ClaimValue; label: string }[] | null {
  if (def.kind === 'flag')
    return [
      { value: true, label: 'Yes' },
      { value: false, label: 'No' },
    ]
  if (def.kind === 'enum' && Array.isArray(def.domain))
    return def.domain.map((v) => ({ value: v, label: formatValue(def.key, v) }))
  if (def.kind === 'ordinal' && def.domain && !Array.isArray(def.domain) && def.domain.max - def.domain.min <= 10) {
    const out = []
    for (let v = def.domain.min; v <= def.domain.max; v++) out.push({ value: v, label: formatValue(def.key, v) })
    return out
  }
  return null
}

export function hasFlag(features: number, bit: number | null): boolean {
  return bit !== null && bit < 31 ? (features & (1 << bit)) !== 0 : false
}

export function sortedAttrs(attrs: AttrDef[]): AttrDef[] {
  return [...attrs].sort((a, b) => a.sort - b.sort)
}

export const GROUP_LABELS: Record<AttrDef['group'], string> = {
  location: 'Location',
  infra: 'Facilities',
  rules: 'Rules',
  vibe: 'Atmosphere',
}

export const CROWD: Record<CrowdColor | 'none', { label: string; dot: string; text: string; bg: string }> = {
  green: { label: 'Not busy', dot: 'bg-crowd-green', text: 'text-green-800', bg: 'bg-green-50' },
  yellow: { label: 'Getting busy', dot: 'bg-crowd-yellow', text: 'text-yellow-800', bg: 'bg-yellow-50' },
  red: { label: 'Packed', dot: 'bg-crowd-red', text: 'text-red-800', bg: 'bg-red-50' },
  none: { label: 'No reports', dot: 'bg-crowd-none', text: 'text-slate-600', bg: 'bg-slate-100' },
}

/** Hex values for the map, matching the --color-crowd-* tokens. */
export const CROWD_HEX: Record<CrowdColor | 'none', string> = {
  green: '#16a34a',
  yellow: '#eab308',
  red: '#dc2626',
  none: '#94a3b8',
}

export const EVENT_LABELS: Record<EventKind, string> = {
  outlet_broken: 'Outlets broken',
  wifi_down: 'Wi-Fi down',
  closed_event: 'Closed',
}

/** Front-end scenario presets: aliases for filter combinations (roadmap 7.1). */
export const PRESETS: { name: string; must: string[]; quiet: number; vibe?: string }[] = [
  { name: 'Finals all-nighter', must: ['late_night', 'outlets'], quiet: 2 },
  { name: 'Deep focus', must: [], quiet: 3, vibe: 'deep_work' },
  { name: 'Group project', must: ['whiteboard'], quiet: 0, vibe: 'collab' },
  { name: 'Lunch & laptop', must: ['food_allowed', 'outlets'], quiet: 0 },
]

export function timeAgo(ms: number, now = Date.now()): string {
  const s = Math.round((now - ms) / 1000)
  if (s < 60) return 'just now'
  if (s < 3600) return `${Math.floor(s / 60)} min ago`
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`
  return `${Math.floor(s / 86400)} d ago`
}
