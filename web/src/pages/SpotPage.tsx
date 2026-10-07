import { Link, useParams } from 'react-router'

import { Claims } from '../components/Claims'
import { ComingSoon } from '../components/ComingSoon'
import { CrowdBadge } from '../components/CrowdBadge'
import { ReportPanel } from '../components/ReportPanel'
import { ApiError } from '../lib/api'
import { Card, ErrorBox, Loading } from '../components/ui'
import { EVENT_LABELS, formatValue, GROUP_LABELS, sortedAttrs, timeAgo } from '../lib/attrs'
import { useLiveChannel } from '../lib/liveContext'
import { useCampus, useSpot } from '../lib/queries'
import type { AttrDef, SpotDetail } from '../lib/types'
import { NotFoundPage } from './NotFoundPage'

function Attributes({ spot, attrs }: { spot: SpotDetail; attrs: AttrDef[] }) {
  const groups = (Object.keys(GROUP_LABELS) as AttrDef['group'][]).map((g) => ({
    g,
    items: sortedAttrs(attrs).filter((a) => a.group === g),
  }))
  return (
    <Card>
      <h2 className="font-semibold">What it's like</h2>
      <div className="mt-3 grid gap-x-6 gap-y-4 sm:grid-cols-2">
        {groups.map(({ g, items }) => (
          <div key={g}>
            <h3 className="mb-1 text-xs font-semibold tracking-wide text-slate-500 uppercase">{GROUP_LABELS[g]}</h3>
            <dl className="space-y-1">
              {items.map((a) => {
                const m = spot.attrs[a.key]
                return (
                  <div key={a.key} className="flex items-center justify-between gap-2 text-sm">
                    <dt className="text-slate-600">{a.label_en}</dt>
                    <dd className={m ? 'font-medium' : 'text-slate-400'} title={m ? `${Math.round(m.p * 100)}% confidence` : 'No confident claim yet'}>
                      {m ? formatValue(a.key, m.v) : 'Unknown'}
                      {m && (
                        <span className="ml-1.5 inline-block h-1.5 w-8 overflow-hidden rounded-full bg-slate-200 align-middle">
                          <span className="block h-full bg-brand-500" style={{ width: `${Math.round(m.p * 100)}%` }} />
                        </span>
                      )}
                    </dd>
                  </div>
                )
              })}
            </dl>
          </div>
        ))}
      </div>
    </Card>
  )
}

function Hours({ hours }: { hours: unknown }) {
  if (!hours || typeof hours !== 'object') return null
  const rows = Object.entries(hours as Record<string, unknown>)
  if (!rows.length) return null
  return (
    <dl className="mt-2 grid grid-cols-[auto_1fr] gap-x-4 gap-y-0.5 text-sm">
      {rows.map(([day, h]) => (
        <div key={day} className="contents">
          <dt className="text-slate-500 capitalize">{day}</dt>
          <dd>{String(h)}</dd>
        </div>
      ))}
    </dl>
  )
}

export function SpotPage() {
  const id = Number(useParams().id)
  const valid = Number.isInteger(id) && id > 0
  const spot = useSpot(id, valid)
  const campus = useCampus(spot.data?.campus ?? null)
  useLiveChannel(valid ? `spot:${id}` : null)

  if (!valid || (spot.error instanceof ApiError && spot.error.status === 404)) return <NotFoundPage />
  if (spot.isPending) return <Loading />
  if (spot.error) return <ErrorBox error={spot.error} className="m-4" />

  const s = spot.data
  const attrs = campus.data?.attrs ?? []
  const events = s.events

  return (
    <div className="mx-auto max-w-5xl space-y-4 p-4">
      <Link to="/" className="text-sm text-slate-500 hover:text-slate-800">
        ← Back to map
      </Link>

      {s.status === 'hidden' && (
        <div className="rounded-lg bg-amber-50 px-3 py-2 text-sm text-amber-900 ring-1 ring-amber-200">
          This spot was suggested by a student and is waiting for others to confirm it. It does not appear in search yet.
        </div>
      )}

      <header className="flex flex-wrap items-start justify-between gap-3">
        <div>
          <h1 className="text-2xl font-bold">{s.name}</h1>
          <p className="text-slate-600">
            {s.building.name}
            {s.floor && ` · Floor ${s.floor}`}
          </p>
          {s.summary && <p className="mt-2 max-w-prose text-slate-700">{s.summary}</p>}
        </div>
        <div className="text-right">
          <CrowdBadge live={s.live} />
          {s.live?.at && <p className="mt-1 text-xs text-slate-500">updated {timeAgo(s.live.at)}</p>}
          {!s.live && s.attrs.crowd_typical && (
            <p className="mt-1 text-xs text-slate-500">{formatValue('crowd_typical', s.attrs.crowd_typical.v)}</p>
          )}
        </div>
      </header>

      {events.length > 0 && (
        <div className="space-y-1">
          {events.map((e) => (
            <div key={e.kind} className="rounded-lg bg-red-50 px-3 py-2 text-sm text-red-800 ring-1 ring-red-200">
              ⚠ {EVENT_LABELS[e.kind]} reported by {e.reports} {e.reports === 1 ? 'student' : 'students'} (until{' '}
              {new Date(e.until).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })})
            </div>
          ))}
        </div>
      )}

      <div className="grid gap-4 lg:grid-cols-[1fr_340px]">
        <div className="space-y-4">
          <ReportPanel spot={s} />
          <Attributes spot={s} attrs={attrs} />
          {(s.pros.length > 0 || s.cons.length > 0) && (
            <Card>
              <div className="grid gap-4 sm:grid-cols-2">
                <div>
                  <h3 className="text-sm font-semibold text-green-800">Pros</h3>
                  <ul className="mt-1 list-disc pl-5 text-sm">{s.pros.map((p) => <li key={p}>{p}</li>)}</ul>
                </div>
                <div>
                  <h3 className="text-sm font-semibold text-red-800">Cons</h3>
                  <ul className="mt-1 list-disc pl-5 text-sm">{s.cons.map((p) => <li key={p}>{p}</li>)}</ul>
                </div>
              </div>
            </Card>
          )}
          <Claims spot={s} attrs={attrs} />
        </div>
        <aside className="space-y-4">
          <ComingSoon feature="checkins" compact />
          <ComingSoon feature="forecast" compact />
          <ComingSoon feature="photos" compact />
          <Card>
            <h2 className="font-semibold">Building hours</h2>
            <Hours hours={s.building.hours} />
            {!s.building.hours && <p className="mt-1 text-sm text-slate-500">Unknown</p>}
          </Card>
        </aside>
      </div>
    </div>
  )
}
