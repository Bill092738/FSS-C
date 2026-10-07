import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useRef, useState, type ChangeEvent } from 'react'
import { Link } from 'react-router'

import { api, ApiError, PHOTO_MAX_BYTES, PHOTO_TYPES } from '../lib/api'
import { keys, useMe } from '../lib/queries'
import type { Photo, SpotDetail } from '../lib/types'
import { Button, Card, ErrorBox } from './ui'

function PhotoTile({ photo, me, onVoted }: { photo: Photo; me: number | null; onVoted: () => void }) {
  const [mine, setMine] = useState<1 | -1 | 0>(0)
  const vote = useMutation({
    mutationFn: (v: 1 | -1 | 0) => api.votePhoto(photo.id, v),
    onSuccess: (_r, v) => {
      setMine(v)
      onVoted()
    },
  })
  const own = photo.user === me || (vote.error instanceof ApiError && vote.error.code === 'own_photo')
  const canVote = me !== null && !own
  return (
    <figure className="overflow-hidden rounded-lg ring-1 ring-slate-200">
      <a href={photo.url} target="_blank" rel="noreferrer">
        <img src={photo.url} alt="" loading="lazy" className="aspect-square w-full object-cover" />
      </a>
      <figcaption className="flex items-center justify-between gap-1 px-1.5 py-1 text-xs text-slate-600">
        <span title="Weighted votes">
          ▲ {photo.up} · ▼ {photo.down}
        </span>
        {canVote && (
          <span className="flex gap-0.5">
            {([1, -1] as const).map((v) => (
              <button
                key={v}
                type="button"
                aria-label={v > 0 ? 'Helpful photo' : 'Wrong or unhelpful photo'}
                aria-pressed={mine === v}
                disabled={vote.isPending}
                onClick={() => vote.mutate(mine === v ? 0 : v)}
                className={`rounded px-1.5 ${mine === v ? 'bg-brand-100 text-brand-800' : 'hover:bg-slate-100'}`}
              >
                {v > 0 ? '▲' : '▼'}
              </button>
            ))}
          </span>
        )}
        {own && <span>Your photo</span>}
      </figcaption>
    </figure>
  )
}

/** Spot photos with votes and upload (roadmap 7.5, 10.5). */
export function Photos({ spot }: { spot: SpotDetail }) {
  const me = useMe()
  const qc = useQueryClient()
  const input = useRef<HTMLInputElement>(null)
  const [localError, setLocalError] = useState<string | null>(null)
  const refresh = () => void qc.invalidateQueries({ queryKey: keys.spot(spot.id) })
  const upload = useMutation({
    mutationFn: (file: File) => api.uploadPhoto(spot.id, file),
    onSuccess: refresh,
  })

  const pick = (e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    e.target.value = ''
    setLocalError(null)
    upload.reset()
    if (!file) return
    if (!PHOTO_TYPES.includes(file.type)) return setLocalError('Choose a JPEG, PNG or WebP image.')
    if (file.size > PHOTO_MAX_BYTES) return setLocalError('Photos are limited to 5 MB.')
    upload.mutate(file)
  }

  const duplicate = upload.error instanceof ApiError && upload.error.code === 'duplicate_photo'
  return (
    <Card>
      <h2 className="font-semibold">Photos</h2>
      {spot.photos.length ? (
        <div className="mt-3 grid grid-cols-2 gap-2">
          {spot.photos.map((p) => (
            <PhotoTile key={p.id} photo={p} me={me.data?.id ?? null} onVoted={refresh} />
          ))}
        </div>
      ) : (
        <p className="mt-1 text-sm text-slate-500">No photos yet.</p>
      )}
      {me.data === null ? (
        <p className="mt-3 text-sm text-slate-600">
          <Link to="/login" className="font-medium text-brand-700 hover:underline">
            Log in
          </Link>{' '}
          to add a photo.
        </p>
      ) : (
        <>
          <input ref={input} type="file" accept={PHOTO_TYPES.join(',')} className="hidden" onChange={pick} />
          <Button variant="secondary" className="mt-3 w-full" busy={upload.isPending} onClick={() => input.current?.click()}>
            Add a photo
          </Button>
          {upload.isSuccess && (
            <p className="mt-2 text-sm text-brand-800">
              Thanks!{upload.data.karma ? ` +${upload.data.karma} karma.` : ''}
            </p>
          )}
          {localError && <p className="mt-2 text-sm text-red-700">{localError}</p>}
          {duplicate ? (
            <p className="mt-2 text-sm text-slate-600">This photo was already uploaded.</p>
          ) : (
            <ErrorBox error={upload.error} className="mt-2" />
          )}
        </>
      )}
    </Card>
  )
}
