import { Link } from 'react-router'

import { timeAgo } from '../lib/attrs'
import { KARMA_REASONS } from '../lib/karma'
import { useKarmaHistory } from '../lib/queries'
import type { KarmaEntry } from '../lib/types'
import { Button, Card, ErrorBox, Loading } from './ui'

function refLink(e: KarmaEntry): string | null {
  return e.ref_type === 'spot' ? `/spots/${e.ref_id}` : null
}

/** The karma ledger (roadmap 7.5), newest first. */
export function KarmaHistory() {
  const q = useKarmaHistory()
  const entries = q.data?.pages.flatMap((p) => p.entries) ?? []
  return (
    <Card>
      <h2 className="font-semibold">Karma history</h2>
      {q.isPending ? (
        <Loading />
      ) : entries.length ? (
        <ul className="mt-2 divide-y divide-slate-100">
          {entries.map((e) => {
            const link = refLink(e)
            const label = KARMA_REASONS[e.reason] ?? e.reason
            return (
              <li key={e.id} className="flex items-center justify-between gap-3 py-1.5 text-sm">
                <span>
                  {link ? (
                    <Link to={link} className="hover:underline">
                      {label}
                    </Link>
                  ) : (
                    label
                  )}
                  <span className="ml-2 text-xs text-slate-500">{timeAgo(e.at)}</span>
                </span>
                <span className={e.delta > 0 ? 'font-medium text-green-700' : 'font-medium text-red-700'}>
                  {e.delta > 0 ? '+' : '−'}
                  {Math.abs(e.delta)}
                </span>
              </li>
            )
          })}
        </ul>
      ) : (
        <p className="mt-1 text-sm text-slate-500">
          Report how crowded a spot is, check in or add photos to earn karma.
        </p>
      )}
      {q.hasNextPage && (
        <Button variant="ghost" className="mt-2 w-full" busy={q.isFetchingNextPage} onClick={() => void q.fetchNextPage()}>
          Show more
        </Button>
      )}
      <ErrorBox error={q.error} className="mt-2" />
    </Card>
  )
}
