import { useEffect, useState } from 'react'

import { useLive } from '../lib/liveContext'
import { toastFor, type ToastText } from '../lib/karma'

interface Toast extends ToastText {
  id: number
}

const TOAST_MS = 4000

/** Karma and badge notifications pushed over the WebSocket (roadmap 7.5, 9.2). */
export function Toasts() {
  const live = useLive()
  const [toasts, setToasts] = useState<Toast[]>([])
  useEffect(() => {
    let next = 1
    const timers = new Set<ReturnType<typeof setTimeout>>()
    const off = live.onMessage((m) => {
      const t = toastFor(m)
      if (!t) return
      const id = next++
      setToasts((list) => [...list.slice(-3), { id, ...t }])
      const timer = setTimeout(() => {
        timers.delete(timer)
        setToasts((list) => list.filter((x) => x.id !== id))
      }, TOAST_MS)
      timers.add(timer)
    })
    return () => {
      off()
      for (const t of timers) clearTimeout(t)
    }
  }, [live])
  return (
    <div aria-live="polite" className="pointer-events-none fixed inset-x-0 bottom-20 z-50 flex flex-col items-center gap-2 md:bottom-6">
      {toasts.map((t) => (
        <div
          key={t.id}
          className={`rounded-full px-4 py-2 text-sm font-medium shadow-lg ring-1 ${
            t.tone === 'good' ? 'bg-brand-700 text-white ring-brand-800' : 'bg-white text-red-800 ring-red-200'
          }`}
        >
          {t.text}
        </div>
      ))}
    </div>
  )
}
