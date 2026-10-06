import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useState, type FormEvent } from 'react'

import { api, ApiError } from '../lib/api'
import { formatValue, sortedAttrs, valueOptions } from '../lib/attrs'
import { keys, useMe } from '../lib/queries'
import type { AttrDef, Claim, ClaimValue, SpotDetail } from '../lib/types'
import { Button, Card, ErrorBox, Field, Input, Pill, Select } from './ui'
import { cx } from '../lib/cx'

const SOURCE_STYLE: Record<Claim['source'], string> = {
  official: 'bg-blue-50 text-blue-800',
  llm: 'bg-violet-50 text-violet-800',
  user: 'bg-slate-100 text-slate-700',
}
const SOURCE_LABEL: Record<Claim['source'], string> = { official: 'Official', llm: 'Web (AI-extracted)', user: 'Student' }

function ClaimRow({ claim, def, canVote }: { claim: Claim; def?: AttrDef; canVote: boolean }) {
  const qc = useQueryClient()
  // The API does not say how the current user voted; remember it for this session.
  const [myVote, setMyVote] = useState<1 | -1 | 0>(0)
  const vote = useMutation({
    mutationFn: (v: 1 | -1 | 0) => api.vote(claim.id, v),
    onSuccess: (_r, v) => {
      setMyVote(v)
      // A vote can change which claim wins, so reload the whole spot.
      void qc.invalidateQueries({ queryKey: ['spot'] })
    },
  })
  const ownClaim = vote.error instanceof ApiError && vote.error.status === 403
  return (
    <li className="py-3">
      <div className="flex flex-wrap items-center gap-2">
        <span className="text-sm font-medium">
          {def?.label_en ?? claim.attr}: {formatValue(claim.attr, claim.value)}
        </span>
        <Pill className={SOURCE_STYLE[claim.source]}>{SOURCE_LABEL[claim.source]}</Pill>
        <span className="text-xs text-slate-500" title="Confidence (Beta mean of prior and weighted votes)">
          {Math.round(claim.p * 100)}% sure
        </span>
      </div>
      {claim.evidence && (
        <blockquote className="mt-1 border-l-2 border-slate-300 pl-2 text-sm text-slate-600 italic">“{claim.evidence}”</blockquote>
      )}
      {claim.url && /^https?:/.test(claim.url) && (
        <a href={claim.url} target="_blank" rel="noreferrer noopener" className="mt-1 block truncate text-xs text-brand-700 hover:underline">
          {claim.url}
        </a>
      )}
      <div className="mt-1.5 flex items-center gap-1 text-xs text-slate-500">
        {canVote && (
          <>
            <button
              type="button"
              aria-label="Agree"
              aria-pressed={myVote === 1}
              disabled={vote.isPending}
              onClick={() => vote.mutate(myVote === 1 ? 0 : 1)}
              className={cx('rounded px-1.5 py-0.5 hover:bg-slate-100', myVote === 1 && 'bg-green-100 text-green-800')}
            >
              ▲
            </button>
            <button
              type="button"
              aria-label="Disagree"
              aria-pressed={myVote === -1}
              disabled={vote.isPending}
              onClick={() => vote.mutate(myVote === -1 ? 0 : -1)}
              className={cx('rounded px-1.5 py-0.5 hover:bg-slate-100', myVote === -1 && 'bg-red-100 text-red-800')}
            >
              ▼
            </button>
          </>
        )}
        <span>
          {claim.up} up · {claim.down} down
        </span>
      </div>
      {ownClaim ? (
        <p className="mt-1 text-xs text-slate-500">You can't vote on your own claim.</p>
      ) : (
        <ErrorBox error={vote.error} className="mt-1" />
      )}
    </li>
  )
}

function AddClaimForm({ spot, attrs, onDone }: { spot: SpotDetail; attrs: AttrDef[]; onDone: () => void }) {
  const qc = useQueryClient()
  const editable = sortedAttrs(attrs)
  const [attr, setAttr] = useState(editable[0]?.key ?? '')
  const [value, setValue] = useState('')
  const [evidence, setEvidence] = useState('')
  const def = editable.find((a) => a.key === attr)
  const options = def ? valueOptions(def) : null

  const create = useMutation({
    mutationFn: (v: ClaimValue) => api.createClaim(spot.id, { attr, value: v, evidence: evidence.trim() || undefined }),
    onSuccess: (r) => {
      qc.setQueryData(keys.spot(spot.id), r.spot)
      onDone()
    },
  })

  const submit = (e: FormEvent) => {
    e.preventDefault()
    if (!def) return
    let v: ClaimValue
    if (options) v = options[Number(value)]?.value ?? options[0].value
    else if (def.kind === 'ordinal') v = Number(value)
    else v = value.trim()
    create.mutate(v)
  }

  return (
    <form onSubmit={submit} className="mt-3 space-y-3 rounded-lg bg-slate-50 p-3">
      <div className="grid gap-3 sm:grid-cols-2">
        <Field label="Attribute">
          <Select
            value={attr}
            onChange={(e) => {
              setAttr(e.target.value)
              setValue('')
            }}
          >
            {editable.map((a) => (
              <option key={a.key} value={a.key}>
                {a.label_en}
              </option>
            ))}
          </Select>
        </Field>
        <Field label="Value">
          {options ? (
            <Select value={value || '0'} onChange={(e) => setValue(e.target.value)}>
              {options.map((o, i) => (
                <option key={String(o.value)} value={i}>
                  {o.label}
                </option>
              ))}
            </Select>
          ) : (
            <Input
              required
              type={def?.kind === 'ordinal' ? 'number' : 'text'}
              value={value}
              onChange={(e) => setValue(e.target.value)}
              placeholder={def?.key === 'hours' ? 'e.g. Mon–Fri 8:00–22:00' : ''}
            />
          )}
        </Field>
      </div>
      <Field label="What did you see? (optional)">
        <Input value={evidence} onChange={(e) => setEvidence(e.target.value)} maxLength={500} placeholder="e.g. outlets under every table" />
      </Field>
      <ErrorBox error={create.error} />
      <div className="flex gap-2">
        <Button type="submit" busy={create.isPending}>
          Submit
        </Button>
        <Button variant="ghost" onClick={onDone}>
          Cancel
        </Button>
      </div>
    </form>
  )
}

/** All claims with their evidence and votes (roadmap 7.6). */
export function Claims({ spot, attrs }: { spot: SpotDetail; attrs: AttrDef[] }) {
  const me = useMe()
  const [adding, setAdding] = useState(false)
  const byKey = new Map(attrs.map((a) => [a.key, a]))
  const claims = [...spot.claims].sort(
    (a, b) => (byKey.get(a.attr)?.sort ?? 999) - (byKey.get(b.attr)?.sort ?? 999) || b.p - a.p,
  )
  return (
    <Card>
      <div className="flex items-center justify-between">
        <h2 className="font-semibold">Evidence &amp; corrections</h2>
        {me.data && !adding && (
          <Button variant="secondary" className="py-1 text-xs" onClick={() => setAdding(true)}>
            Suggest a correction
          </Button>
        )}
      </div>
      <p className="mt-1 text-xs text-slate-500">Each attribute shows the claim with the highest confidence. Vote to confirm or dispute.</p>
      {adding && <AddClaimForm spot={spot} attrs={attrs} onDone={() => setAdding(false)} />}
      {claims.length === 0 ? (
        <p className="mt-3 text-sm text-slate-500">No claims yet.</p>
      ) : (
        <ul className="mt-2 divide-y divide-slate-100">
          {claims.map((c) => (
            <ClaimRow key={c.id} claim={c} def={byKey.get(c.attr)} canVote={!!me.data} />
          ))}
        </ul>
      )}
    </Card>
  )
}
