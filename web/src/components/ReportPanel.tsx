import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { Link } from 'react-router'

import { api } from '../lib/api'
import { CROWD, EVENT_LABELS } from '../lib/attrs'
import { getPosition } from '../lib/geo'
import { keys, useMe } from '../lib/queries'
import type { EventKind, ReportResult, SpotDetail } from '../lib/types'
import { Button, Card, ErrorBox } from './ui'
import { cx } from '../lib/cx'

const LEVELS = [
  { level: 0, color: 'green' },
  { level: 1, color: 'yellow' },
  { level: 2, color: 'red' },
] as const

function fenceText(fence: number): string {
  if (fence >= 1) return 'You are inside the building, so your report counts fully.'
  if (fence >= 0.6) return 'You seem to be near the building; your report counts a bit less.'
  return 'Your location could not be confirmed, so your report counts less.'
}

/** Three-color crowding report and event report (roadmap 7.2, 7.3, 10.2). */
export function ReportPanel({ spot }: { spot: SpotDetail }) {
  const me = useMe()
  const qc = useQueryClient()
  const [last, setLast] = useState<ReportResult | null>(null)

  const report = useMutation({
    mutationFn: async (what: { level: 0 | 1 | 2 } | { event: EventKind }) => {
      const pos = await getPosition()
      return 'level' in what ? api.reportLevel(spot.id, what.level, pos) : api.reportEvent(spot.id, what.event, pos)
    },
    onSuccess: (r) => {
      setLast(r)
      if (r.live) qc.setQueryData<SpotDetail>(keys.spot(spot.id), (old) => (old ? { ...old, live: r.live! } : old))
      if (r.event) void qc.invalidateQueries({ queryKey: keys.spot(spot.id) })
      void qc.invalidateQueries({ queryKey: ['spots'] })
    },
  })

  if (spot.status !== 'active')
    return (
      <Card>
        <h2 className="font-semibold">How crowded is it?</h2>
        <p className="mt-1 text-sm text-slate-500">Reports open once other students confirm this spot exists.</p>
      </Card>
    )

  return (
    <Card>
      <h2 className="font-semibold">How crowded is it right now?</h2>
      {me.data === null ? (
        <p className="mt-2 text-sm text-slate-600">
          <Link to="/login" className="font-medium text-brand-700 hover:underline">
            Log in
          </Link>{' '}
          to report the crowd level.
        </p>
      ) : (
        <>
          <div className="mt-3 grid grid-cols-3 gap-2">
            {LEVELS.map(({ level, color }) => (
              <button
                key={level}
                type="button"
                disabled={report.isPending}
                onClick={() => report.mutate({ level })}
                className={cx(
                  'flex flex-col items-center gap-1 rounded-xl py-3 text-sm font-medium ring-1 transition-transform active:scale-95 disabled:opacity-50',
                  CROWD[color].bg,
                  CROWD[color].text,
                  'ring-slate-200 hover:ring-slate-400',
                )}
              >
                <span className={cx('size-5 rounded-full', CROWD[color].dot)} />
                {CROWD[color].label}
              </button>
            ))}
          </div>
          <div className="mt-3 flex flex-wrap items-center gap-2 text-sm">
            <span className="text-slate-500">Problem?</span>
            {(Object.keys(EVENT_LABELS) as EventKind[]).map((k) => (
              <Button key={k} variant="secondary" className="px-2 py-1 text-xs" disabled={report.isPending} onClick={() => report.mutate({ event: k })}>
                {EVENT_LABELS[k]}
              </Button>
            ))}
          </div>
          {report.isPending && <p className="mt-3 text-xs text-slate-500">Getting your location and sending…</p>}
          <ErrorBox error={report.error} className="mt-3" />
          {last && !report.isPending && !report.error && (
            <p className="mt-3 rounded-lg bg-brand-50 px-3 py-2 text-sm text-brand-800">
              Thanks!{' '}
              {last.event &&
                (last.event.visible ? 'Others can now see this problem. ' : 'The problem shows once someone else confirms it. ')}
              {fenceText(last.report.fence)}
              {last.karma > 0 && ` +${last.karma} karma.`}
            </p>
          )}
        </>
      )}
    </Card>
  )
}
