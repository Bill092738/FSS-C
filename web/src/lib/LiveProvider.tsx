import { useQueryClient } from '@tanstack/react-query'
import { useEffect, useState, type ReactNode } from 'react'

import { LiveSocket } from './live'
import { applyToCache, LiveContext } from './liveContext'

export function LiveProvider({ children, socket }: { children: ReactNode; socket?: LiveSocket }) {
  const qc = useQueryClient()
  const [live] = useState(() => socket ?? new LiveSocket())
  useEffect(() => {
    const off = live.onMessage((m) => applyToCache(qc, m))
    live.start()
    return () => {
      off()
      live.stop()
    }
  }, [live, qc])
  return <LiveContext.Provider value={live}>{children}</LiveContext.Provider>
}
