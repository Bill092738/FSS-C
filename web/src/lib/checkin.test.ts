import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { HEARTBEAT_MS, HEARTBEAT_RETRY_MS, nextBeatDelay } from './checkin'

describe('nextBeatDelay', () => {
  it('waits for the heartbeat interval after the last position', () => {
    expect(nextBeatDelay({ start_at: 1000, last_beat_at: null }, 1000)).toBe(HEARTBEAT_MS)
    expect(nextBeatDelay({ start_at: 1000, last_beat_at: 5000 }, 6000)).toBe(HEARTBEAT_MS - 1000)
  })

  it('is due at once when overdue (e.g. after the tab slept)', () => {
    expect(nextBeatDelay({ start_at: 0, last_beat_at: 0 }, HEARTBEAT_MS * 3)).toBe(0)
  })
})

describe('useCheckinHeartbeat', () => {
  const NOW = 1_790_000_000_000

  async function setup(heartbeat: (...a: unknown[]) => Promise<unknown>) {
    const { QueryClient, QueryClientProvider } = await import('@tanstack/react-query')
    const { createElement } = await import('react')
    const { renderHook } = await import('@testing-library/react')
    const { api } = await import('./api')
    const geo = await import('./geo')
    const { keys } = await import('./queries')
    const { useCheckinHeartbeat } = await import('./checkin')
    vi.spyOn(geo, 'getPosition').mockResolvedValue({ lat: 40, lon: -83, accuracy_m: 10 })
    vi.spyOn(api, 'me').mockImplementation(async () => ({ user: qc.getQueryData(keys.me) as never }))
    const spy = vi.spyOn(api, 'heartbeat').mockImplementation(heartbeat as never)
    const qc = new QueryClient({ defaultOptions: { queries: { staleTime: Infinity } } })
    qc.setQueryData(keys.me, {
      id: 1,
      checkin: { id: 9, spot: 1, start_at: NOW, last_beat_at: NOW, verified: true, verified_ms: 0 },
    })
    renderHook(() => useCheckinHeartbeat(), {
      wrapper: ({ children }) => createElement(QueryClientProvider, { client: qc }, children),
    })
    return { spy, qc, keys }
  }

  beforeEach(() => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'Date'] })
    vi.setSystemTime(NOW)
  })
  afterEach(() => {
    vi.useRealTimers()
    vi.restoreAllMocks()
  })

  it('beats every 10 minutes with the position and follows the server state', async () => {
    let beats = 0
    const { spy, qc, keys } = await setup(async () => {
      beats++
      const at = NOW + beats * HEARTBEAT_MS
      return { checkin: { id: 9, spot: 1, start_at: NOW, end_at: null, last_beat_at: at, verified: true, verified_ms: beats * HEARTBEAT_MS } }
    })
    await vi.advanceTimersByTimeAsync(HEARTBEAT_MS - 1)
    expect(spy).not.toHaveBeenCalled()
    await vi.advanceTimersByTimeAsync(1)
    expect(spy).toHaveBeenCalledWith(9, { lat: 40, lon: -83, accuracy_m: 10 })
    await vi.advanceTimersByTimeAsync(HEARTBEAT_MS)
    expect(spy).toHaveBeenCalledTimes(2)
    expect((qc.getQueryData(keys.me) as { checkin: { verified_ms: number } }).checkin.verified_ms).toBe(2 * HEARTBEAT_MS)
  })

  it('retries a failed heartbeat after a minute', async () => {
    let calls = 0
    const { spy } = await setup(async () => {
      calls++
      if (calls === 1) throw new Error('offline')
      return { checkin: { id: 9, spot: 1, start_at: NOW, end_at: null, last_beat_at: Date.now(), verified: true, verified_ms: 0 } }
    })
    const { act } = await import('@testing-library/react')
    await vi.advanceTimersByTimeAsync(HEARTBEAT_MS)
    expect(spy).toHaveBeenCalledTimes(1)
    // React's scheduler runs on the faked timers here; let it re-render now
    await act(async () => {})
    await vi.advanceTimersByTimeAsync(HEARTBEAT_RETRY_MS - 1)
    expect(spy).toHaveBeenCalledTimes(1)
    await vi.advanceTimersByTimeAsync(1)
    expect(spy).toHaveBeenCalledTimes(2)
  })
})
