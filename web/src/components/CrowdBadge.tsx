import { CROWD } from '../lib/attrs'
import type { CrowdColor, LiveState } from '../lib/types'
import { Pill } from './ui'
import { cx } from '../lib/cx'

export function CrowdDot({ color, className }: { color: CrowdColor | 'none'; className?: string }) {
  const c = CROWD[color]
  return <span className={cx('inline-block size-2.5 shrink-0 rounded-full', c.dot, className)} aria-hidden />
}

/** Crowd color plus whether it comes from live reports or the forecast (roadmap 7.3). */
export function CrowdBadge({ live, showBasis = true }: { live: LiveState | null; showBasis?: boolean }) {
  const c = CROWD[live?.color ?? 'none']
  return (
    <Pill className={cx(c.bg, c.text)}>
      <CrowdDot color={live?.color ?? 'none'} />
      {c.label}
      {showBasis && live && (
        <span className="font-normal opacity-75">· {live.basis === 'reports' ? 'live' : 'forecast'}</span>
      )}
    </Pill>
  )
}
