import { useState } from 'react'
import { Link } from 'react-router'

import { HEARTBEAT_MS, useCheckinActions } from '../lib/checkin'
import { useMe } from '../lib/queries'
import type { CheckinResult, SpotDetail } from '../lib/types'
import { Button, Card, ErrorBox } from './ui'

const END_TEXT: Record<string, string> = {
  user: 'Checked out.',
  outside: 'Checked out: your location was outside the building twice in a row.',
  timeout: 'Checked out after the 6-hour limit.',
  stale: 'Checked out: no location update for 30 minutes.',
}

function minutes(ms: number): string {
  const m = Math.floor(ms / 60_000)
  return m >= 60 ? `${Math.floor(m / 60)} h ${m % 60} min` : `${m} min`
}

const clock = (t: number) => new Date(t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })

/** Check-in at a spot (roadmap 7.4). Heartbeats run in the app shell. */
export function CheckinPanel({ spot }: { spot: SpotDetail }) {
  const me = useMe()
  const { start, end } = useCheckinActions()
  const [ended, setEnded] = useState<CheckinResult | null>(null)
  const open = me.data?.checkin ?? null
  const here = open?.spot === spot.id

  const present = spot.present > 0 && (
    <p className="mt-1 text-xs text-slate-500">
      {spot.present} {spot.present === 1 ? 'student is' : 'students are'} checked in here.
    </p>
  )

  let body
  if (spot.status !== 'active') {
    body = <p className="mt-1 text-sm text-slate-500">Check-ins open once other students confirm this spot.</p>
  } else if (me.data === null) {
    body = (
      <p className="mt-1 text-sm text-slate-600">
        <Link to="/login" className="font-medium text-brand-700 hover:underline">
          Log in
        </Link>{' '}
        to check in and earn karma for your study time.
      </p>
    )
  } else if (open && here) {
    body = (
      <>
        <p className="mt-1 text-sm">
          Checked in since {clock(open.start_at)}.{' '}
          {open.verified ? (
            <>Verified study time: {minutes(open.verified_ms)}.</>
          ) : (
            <span className="text-amber-800">Not verified yet: your location was not inside the building.</span>
          )}
        </p>
        <p className="mt-1 text-xs text-slate-500">
          FSS checks your location every {HEARTBEAT_MS / 60_000} minutes while it is open.
        </p>
        <Button
          variant="secondary"
          className="mt-3 w-full"
          busy={end.isPending}
          onClick={() => end.mutate(open.id, { onSuccess: setEnded })}
        >
          Check out
        </Button>
      </>
    )
  } else if (open) {
    body = (
      <>
        <p className="mt-1 text-sm text-slate-600">
          You are checked in at{' '}
          <Link to={`/spots/${open.spot}`} className="font-medium text-brand-700 hover:underline">
            another spot
          </Link>
          .
        </p>
        <Button variant="secondary" className="mt-3 w-full" busy={end.isPending} onClick={() => end.mutate(open.id)}>
          Check out there
        </Button>
      </>
    )
  } else {
    body = (
      <>
        <p className="mt-1 text-xs text-slate-500">
          Your location confirms you are here; every verified hour earns 1 karma.
        </p>
        <Button
          className="mt-3 w-full"
          busy={start.isPending}
          onClick={() => {
            setEnded(null)
            start.mutate(spot.id)
          }}
        >
          Check in here
        </Button>
        {ended && (
          <p className="mt-2 rounded-lg bg-brand-50 px-3 py-2 text-sm text-brand-800">
            {END_TEXT[ended.checkin.end_reason ?? 'user']}
            {ended.karma ? ` +${ended.karma} karma.` : ''}
          </p>
        )}
      </>
    )
  }

  return (
    <Card>
      <h2 className="font-semibold">Check-in</h2>
      {present}
      {body}
      <ErrorBox error={start.error ?? end.error} className="mt-3" />
    </Card>
  )
}
