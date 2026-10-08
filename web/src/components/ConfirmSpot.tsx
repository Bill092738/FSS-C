import { useMutation, useQueryClient } from '@tanstack/react-query'
import { Link } from 'react-router'

import { api, ApiError } from '../lib/api'
import { keys, useMe } from '../lib/queries'
import type { SpotDetail } from '../lib/types'
import { Button, ErrorBox } from './ui'

const NEEDED = 2 // spot_confirm_min in server/config/rules.json

/** Banner of a submitted spot that waits for confirmations (roadmap 7.5). */
export function ConfirmSpot({ spot }: { spot: SpotDetail }) {
  const me = useMe()
  const qc = useQueryClient()
  const confirm = useMutation({
    mutationFn: () => api.confirmSpot(spot.id),
    onSuccess: () => {
      void qc.invalidateQueries({ queryKey: keys.spot(spot.id) })
      void qc.invalidateQueries({ queryKey: ['spots'] })
    },
  })
  const own = confirm.error instanceof ApiError && confirm.error.code === 'own_spot'
  return (
    <div className="rounded-lg bg-amber-50 px-3 py-2 text-sm text-amber-900 ring-1 ring-amber-200">
      <p>
        This spot was suggested by a student and is waiting for others to confirm it ({spot.confirmations} of {NEEDED}).
        It does not appear in search yet.
      </p>
      {me.data === null ? (
        <p className="mt-2">
          <Link to="/login" className="font-medium text-amber-950 underline">
            Log in
          </Link>{' '}
          to confirm it.
        </p>
      ) : confirm.isSuccess ? (
        <p className="mt-2 font-medium">Thanks for confirming!</p>
      ) : (
        <div className="mt-2 flex flex-wrap items-center gap-2">
          <Button variant="secondary" busy={confirm.isPending} onClick={() => confirm.mutate()}>
            I have been there, it exists
          </Button>
          {own && <span>Other students need to confirm the spots you suggest.</span>}
        </div>
      )}
      {!own && <ErrorBox error={confirm.error} className="mt-2" />}
    </div>
  )
}
