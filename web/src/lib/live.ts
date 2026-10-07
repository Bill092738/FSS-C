import type { Bbox, ServerMessage } from './types'

export type LiveStatus = 'connecting' | 'open' | 'closed'

type Listener = (msg: ServerMessage) => void

interface View {
  campus: string
  bbox: Bbox
}

const MAX_BACKOFF_MS = 30_000

export function defaultSocketUrl(): string {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${location.host}/ws`
}

/**
 * One WebSocket to `/ws` (roadmap 9.2) shared by the whole app.
 *
 * Channels are reference counted, so several components can watch the same
 * spot. After a reconnect every channel is subscribed again with `since` set
 * to the newest `at` seen, and the server replays what was missed.
 */
export class LiveSocket {
  private ws: WebSocket | null = null
  private readonly channels = new Map<string, number>()
  private readonly listeners = new Set<Listener>()
  private readonly statusListeners = new Set<(s: LiveStatus) => void>()
  private currentView: View | null = null
  private lastAt = 0
  private backoff = 1000
  private retryTimer: ReturnType<typeof setTimeout> | null = null
  private stopped = true
  status: LiveStatus = 'closed'

  private readonly url: string
  private readonly makeSocket: (url: string) => WebSocket

  constructor(url: string = defaultSocketUrl(), makeSocket: (url: string) => WebSocket = (u) => new WebSocket(u)) {
    this.url = url
    this.makeSocket = makeSocket
  }

  start(): void {
    if (!this.stopped) return
    this.stopped = false
    this.open()
  }

  stop(): void {
    this.stopped = true
    if (this.retryTimer) clearTimeout(this.retryTimer)
    this.retryTimer = null
    // An explicit code: the server echoes a bare close with the reserved code
    // 1005, which browsers reject as a broken frame (cstl a24d0be).
    this.ws?.close(1000)
    this.ws = null
    this.setStatus('closed')
  }

  /** Reconnects so the server re-reads the session cookie (after login/logout). */
  restart(): void {
    this.stop()
    this.start()
  }

  onMessage(fn: Listener): () => void {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }

  onStatus(fn: (s: LiveStatus) => void): () => void {
    this.statusListeners.add(fn)
    return () => this.statusListeners.delete(fn)
  }

  /** Subscribes to `spot:{id}` or `bldg:{id}`; returns the unsubscribe function. */
  subscribe(ch: string): () => void {
    const n = this.channels.get(ch) ?? 0
    this.channels.set(ch, n + 1)
    if (n === 0) this.send({ op: 'sub', ch })
    let done = false
    return () => {
      if (done) return
      done = true
      const left = (this.channels.get(ch) ?? 1) - 1
      if (left > 0) {
        this.channels.set(ch, left)
      } else {
        this.channels.delete(ch)
        this.send({ op: 'unsub', ch })
      }
    }
  }

  /** Follows the buildings inside a map viewport (`view` op). */
  view(campus: string, bbox: Bbox): void {
    this.currentView = { campus, bbox }
    this.send({ op: 'view', campus, bbox })
  }

  private open(): void {
    this.setStatus('connecting')
    let ws: WebSocket
    try {
      ws = this.makeSocket(this.url)
    } catch {
      this.scheduleRetry()
      return
    }
    this.ws = ws
    ws.onopen = () => {
      this.backoff = 1000
      this.setStatus('open')
      const since = this.lastAt || undefined
      for (const ch of this.channels.keys()) this.send({ op: 'sub', ch, since })
      if (this.currentView) this.send({ op: 'view', ...this.currentView })
    }
    ws.onmessage = (ev) => {
      let msg: ServerMessage
      try {
        msg = JSON.parse(String(ev.data)) as ServerMessage
      } catch {
        return
      }
      if ((msg.t === 'live' || msg.t === 'event') && msg.at > this.lastAt) this.lastAt = msg.at
      for (const fn of this.listeners) fn(msg)
    }
    ws.onclose = () => {
      if (this.ws !== ws) return
      this.ws = null
      if (this.stopped) return
      this.setStatus('closed')
      this.scheduleRetry()
    }
  }

  private scheduleRetry(): void {
    if (this.stopped || this.retryTimer) return
    const delay = this.backoff
    this.backoff = Math.min(this.backoff * 2, MAX_BACKOFF_MS)
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null
      if (!this.stopped) this.open()
    }, delay)
  }

  private send(obj: Record<string, unknown>): void {
    if (this.ws?.readyState === 1 /* OPEN */) this.ws.send(JSON.stringify(obj))
    // Otherwise the state is replayed in onopen.
  }

  private setStatus(s: LiveStatus): void {
    if (s === this.status) return
    this.status = s
    for (const fn of this.statusListeners) fn(s)
  }
}
