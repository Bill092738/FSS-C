import type { KarmaReason, ServerMessage } from './types'

export const KARMA_REASONS: Record<KarmaReason, string> = {
  report: 'Crowd report',
  event_confirmed: 'Problem report confirmed',
  photo: 'Photo',
  photo_hidden: 'Photo hidden by votes',
  claim_accepted: 'Attribute fix accepted',
  spot_discovered: 'New spot confirmed',
  checkin: 'Verified study time',
}

export interface ToastText {
  text: string
  tone: 'good' | 'bad'
}

/** Text for a personal push on `user:{id}` (roadmap 9.2), or null for other messages. */
export function toastFor(m: ServerMessage): ToastText | null {
  if (m.t === 'karma' && m.delta !== 0) {
    const what = (m.reason && KARMA_REASONS[m.reason]) || 'Karma'
    return { text: `${m.delta > 0 ? '+' : '−'}${Math.abs(m.delta)} karma · ${what}`, tone: m.delta > 0 ? 'good' : 'bad' }
  }
  if (m.t === 'badge') return { text: `🏅 New badge: ${m.name}`, tone: 'good' }
  return null
}
