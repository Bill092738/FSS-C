import { afterEach, describe, expect, it, vi } from 'vitest'

import { api, ApiError, request, spotQueryString } from './api'

function mockFetch(status: number, body: string) {
  const fn = vi.fn(async () => new Response(body, { status }))
  vi.stubGlobal('fetch', fn)
  return fn
}

describe('request', () => {
  afterEach(() => vi.unstubAllGlobals())

  it('parses the error envelope, keeping extra fields', async () => {
    mockFetch(409, '{"error":{"code":"duplicate_claim","message":"vote instead","claim_id":7}}')
    const err = await request('POST', '/x', {}).catch((e) => e)
    expect(err).toBeInstanceOf(ApiError)
    expect(err).toMatchObject({ status: 409, code: 'duplicate_claim', message: 'vote instead', extra: { claim_id: 7 } })
  })

  it('handles non-JSON errors and network failures', async () => {
    mockFetch(502, '<html>Bad gateway</html>')
    await expect(request('GET', '/x')).rejects.toMatchObject({ status: 502, code: 'http_502' })
    vi.stubGlobal('fetch', vi.fn(async () => Promise.reject(new TypeError('failed'))))
    await expect(request('GET', '/x')).rejects.toMatchObject({ status: 0, code: 'network' })
  })

  it('sends JSON with the content type the server requires', async () => {
    const fn = mockFetch(200, '{"claim":{}}')
    await api.vote(3, -1)
    expect(fn).toHaveBeenCalledWith('/api/v1/claims/3/vote', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: '{"v":-1}',
    })
  })

  it('omits position fields when location is unavailable', async () => {
    const fn = mockFetch(201, '{}')
    await api.reportLevel(1, 2)
    await api.reportLevel(1, 0, { lat: 40, lon: -83, accuracy_m: 12 })
    const bodies = fn.mock.calls.map((c) => JSON.parse((c as unknown as [string, RequestInit])[1].body as string))
    expect(bodies).toEqual([{ level: 2 }, { level: 0, lat: 40, lon: -83, accuracy_m: 12 }])
  })
})

describe('spotQueryString', () => {
  it('encodes filters the way GET /spots expects', () => {
    expect(spotQueryString({})).toBe('')
    const qs = spotQueryString({
      campus: 'demo',
      must: ['late_night', 'outlets'],
      not: ['calls_allowed'],
      quiet: 2,
      vibe: 'collab',
      near: { lat: 40.003, lon: -83.012 },
      q: '  白板 ',
    })
    const p = new URLSearchParams(qs.slice(1))
    expect(Object.fromEntries(p)).toEqual({
      campus: 'demo',
      must: 'late_night,outlets',
      not: 'calls_allowed',
      quiet: '2',
      vibe: 'collab',
      near: '40.003000,-83.012000',
      q: '白板',
    })
  })

  it('drops quiet=0 and blank text', () => {
    expect(spotQueryString({ quiet: 0, q: '   ' })).toBe('')
  })
})
