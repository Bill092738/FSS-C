import { useMutation } from '@tanstack/react-query'
import { useMemo, useState, type FormEvent } from 'react'
import { Navigate, useNavigate } from 'react-router'

import { ComingSoon } from '../components/ComingSoon'
import { Button, Card, ErrorBox, Field, Input, Loading, Select } from '../components/ui'
import { api } from '../lib/api'
import { sortedAttrs, valueOptions } from '../lib/attrs'
import { useCampus, useMe, useSelectedCampus, useSpots } from '../lib/queries'
import type { ClaimValue } from '../lib/types'

// Attributes offered as quick initial claims on a new spot.
const QUICK = ['outlets', 'noise', 'whiteboard', 'food_allowed', 'late_night', 'vibe']

export function NewSpotPage() {
  const me = useMe()
  const navigate = useNavigate()
  const [slug] = useSelectedCampus()
  const campus = useCampus(slug)
  // No endpoint lists buildings yet (lib/features.ts: buildings); use the
  // buildings of the campus's active spots.
  const spots = useSpots({ campus: slug ?? '', limit: 50 }, !!slug)
  const buildings = useMemo(() => {
    const m = new Map<number, string>()
    for (const s of spots.data?.spots ?? []) m.set(s.building.id, s.building.name)
    return [...m].sort((a, b) => a[1].localeCompare(b[1]))
  }, [spots.data])

  const [building, setBuilding] = useState('')
  const [name, setName] = useState('')
  const [floor, setFloor] = useState('')
  const [values, setValues] = useState<Record<string, string>>({})

  const quick = sortedAttrs(campus.data?.attrs ?? []).filter((a) => QUICK.includes(a.key))

  const create = useMutation({
    mutationFn: api.createSpot,
    onSuccess: (r) => navigate(`/spots/${r.spot.id}`),
  })

  if (me.isPending) return <Loading />
  if (!me.data) return <Navigate to="/login?next=/spots/new" replace />

  const submit = (e: FormEvent) => {
    e.preventDefault()
    const claims: { attr: string; value: ClaimValue }[] = []
    for (const a of quick) {
      const idx = values[a.key]
      const opts = valueOptions(a)
      if (idx !== undefined && idx !== '' && opts) claims.push({ attr: a.key, value: opts[Number(idx)].value })
    }
    create.mutate({
      building_id: Number(building || buildings[0]?.[0]),
      name: name.trim(),
      floor: floor.trim() || undefined,
      claims: claims.length ? claims : undefined,
    })
  }

  return (
    <div className="mx-auto max-w-2xl space-y-4 p-4">
      <div>
        <h1 className="text-xl font-bold">Suggest a new spot</h1>
        <p className="text-sm text-slate-500">
          New spots stay hidden until other students confirm them. You can suggest up to 5 a day.
        </p>
      </div>
      <Card>
        <form onSubmit={submit} className="space-y-4">
          <Field label="Building">
            <Select required value={building || String(buildings[0]?.[0] ?? '')} onChange={(e) => setBuilding(e.target.value)}>
              {buildings.map(([id, n]) => (
                <option key={id} value={id}>
                  {n}
                </option>
              ))}
            </Select>
          </Field>
          <div className="grid gap-4 sm:grid-cols-[1fr_120px]">
            <Field label="Name">
              <Input required maxLength={120} value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Stairwell nook by room 310" />
            </Field>
            <Field label="Floor">
              <Input maxLength={10} value={floor} onChange={(e) => setFloor(e.target.value)} placeholder="3" />
            </Field>
          </div>
          {quick.length > 0 && (
            <fieldset>
              <legend className="text-sm font-medium text-slate-700">What's it like? (optional)</legend>
              <div className="mt-2 grid gap-3 sm:grid-cols-3">
                {quick.map((a) => (
                  <Field key={a.key} label={a.label_en}>
                    <Select value={values[a.key] ?? ''} onChange={(e) => setValues({ ...values, [a.key]: e.target.value })}>
                      <option value="">Don't know</option>
                      {valueOptions(a)?.map((o, i) => (
                        <option key={String(o.value)} value={i}>
                          {o.label}
                        </option>
                      ))}
                    </Select>
                  </Field>
                ))}
              </div>
            </fieldset>
          )}
          <ErrorBox error={create.error} />
          <Button type="submit" busy={create.isPending} disabled={!buildings.length}>
            Submit spot
          </Button>
        </form>
      </Card>
      <ComingSoon feature="buildings" compact />
    </div>
  )
}
