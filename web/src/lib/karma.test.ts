import { describe, expect, it } from 'vitest'

import { toastFor } from './karma'

describe('toastFor', () => {
  it('describes karma changes with their reason', () => {
    expect(toastFor({ t: 'karma', delta: 2, total: 9, reason: 'report' })).toEqual({
      text: '+2 karma · Crowd report',
      tone: 'good',
    })
    expect(toastFor({ t: 'karma', delta: -3, total: 6, reason: 'photo_hidden' })).toEqual({
      text: '−3 karma · Photo hidden by votes',
      tone: 'bad',
    })
    expect(toastFor({ t: 'karma', delta: 1, total: 1 })?.text).toBe('+1 karma · Karma')
  })

  it('announces badges and ignores other messages', () => {
    expect(toastFor({ t: 'badge', key: 'first_report', name: 'First Report' })?.text).toBe('🏅 New badge: First Report')
    expect(toastFor({ t: 'ok', op: 'sub' })).toBeNull()
  })
})
