import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { LiveSocket } from './live'

class FakeSocket {
  readyState = 0
  sent: Record<string, unknown>[] = []
  onopen: (() => void) | null = null
  onmessage: ((ev: { data: string }) => void) | null = null
  onclose: (() => void) | null = null
  send(data: string) {
    this.sent.push(JSON.parse(data))
  }
  close() {
    this.readyState = 3
    this.onclose?.()
  }
  // test helpers
  open() {
    this.readyState = 1
    this.onopen?.()
  }
  receive(msg: unknown) {
    this.onmessage?.({ data: JSON.stringify(msg) })
  }
  drop() {
    this.readyState = 3
    this.onclose?.()
  }
}

function setup() {
  const sockets: FakeSocket[] = []
  const live = new LiveSocket('ws://test/ws', () => {
    const s = new FakeSocket()
    sockets.push(s)
    return s as unknown as WebSocket
  })
  return { live, sockets }
}

describe('LiveSocket', () => {
  beforeEach(() => vi.useFakeTimers())
  afterEach(() => vi.useRealTimers())

  it('reference-counts channels', () => {
    const { live, sockets } = setup()
    live.start()
    sockets[0].open()
    const a = live.subscribe('spot:1')
    const b = live.subscribe('spot:1')
    expect(sockets[0].sent).toEqual([{ op: 'sub', ch: 'spot:1' }])
    a()
    a() // idempotent
    expect(sockets[0].sent).toHaveLength(1)
    b()
    expect(sockets[0].sent).toEqual([
      { op: 'sub', ch: 'spot:1' },
      { op: 'unsub', ch: 'spot:1' },
    ])
  })

  it('subscribes channels requested before the socket opened', () => {
    const { live, sockets } = setup()
    live.subscribe('bldg:2')
    live.view('demo', [-83.02, 39.99, -83.0, 40.01])
    live.start()
    sockets[0].open()
    expect(sockets[0].sent).toEqual([
      { op: 'sub', ch: 'bldg:2' },
      { op: 'view', campus: 'demo', bbox: [-83.02, 39.99, -83.0, 40.01] },
    ])
  })

  it('reconnects with backoff and replays from the last seen message', () => {
    const { live, sockets } = setup()
    const got: unknown[] = []
    live.onMessage((m) => got.push(m))
    const statuses: string[] = []
    live.onStatus((s) => statuses.push(s))
    live.start()
    sockets[0].open()
    live.subscribe('spot:1')
    sockets[0].receive({ t: 'live', spot: 1, color: 'red', est: 1.7, conf: 0.6, basis: 'reports', at: 1000 })
    sockets[0].receive({ t: 'live', spot: 1, color: 'yellow', est: 1.1, conf: 0.6, basis: 'reports', at: 900 })
    expect(got).toHaveLength(2)

    sockets[0].drop()
    expect(live.status).toBe('closed')
    vi.advanceTimersByTime(999)
    expect(sockets).toHaveLength(1)
    vi.advanceTimersByTime(1)
    expect(sockets).toHaveLength(2)
    sockets[1].open()
    expect(sockets[1].sent).toEqual([{ op: 'sub', ch: 'spot:1', since: 1000 }])
    expect(statuses).toEqual(['connecting', 'open', 'closed', 'connecting', 'open'])
  })

  it('does not reconnect after stop', () => {
    const { live, sockets } = setup()
    live.start()
    sockets[0].open()
    live.stop()
    vi.advanceTimersByTime(60_000)
    expect(sockets).toHaveLength(1)
    expect(live.status).toBe('closed')
  })

  it('ignores malformed frames', () => {
    const { live, sockets } = setup()
    const got: unknown[] = []
    live.onMessage((m) => got.push(m))
    live.start()
    sockets[0].open()
    sockets[0].onmessage?.({ data: 'not json' })
    expect(got).toEqual([])
  })
})
