import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from 'react'
import { Link, useNavigate, useSearchParams } from 'react-router'

import { CrowdBadge, CrowdDot } from '../components/CrowdBadge'
import { FilterPanel } from '../components/FilterPanel'
import { Button, ErrorBox, Input, Loading } from '../components/ui'
import { cx } from '../lib/cx'
import { formatValue, hasFlag } from '../lib/attrs'
import { activeFilterCount, parseFilters, serializeFilters, toSpotQuery, type Filters } from '../lib/filters'
import { getPosition } from '../lib/geo'
import { useLive } from '../lib/liveContext'
import { useCampus, useSelectedCampus, useSpots } from '../lib/queries'
import type { AttrDef, Bbox, SpotListItem } from '../lib/types'

// MapLibre is most of the bundle; load it after the list and shell render.
const SpotMap = lazy(() => import('../components/SpotMap').then((m) => ({ default: m.SpotMap })))

function SpotRow({ spot, attrs, active }: { spot: SpotListItem; attrs: AttrDef[]; active: boolean }) {
  const flags = attrs.filter((a) => a.kind === 'flag' && a.key !== 'indoor' && hasFlag(spot.features, a.bit))
  return (
    <Link
      to={`/spots/${spot.id}`}
      className={cx('block rounded-xl p-3 ring-1 transition-colors', active ? 'bg-brand-50 ring-brand-500' : 'bg-white ring-slate-200 hover:ring-slate-300')}
    >
      <div className="flex items-start justify-between gap-2">
        <div className="min-w-0">
          <h3 className="truncate font-semibold">{spot.name}</h3>
          <p className="truncate text-xs text-slate-500">
            {spot.building.name}
            {spot.floor && ` · Floor ${spot.floor}`}
            {spot.dist_m !== null && ` · ${spot.dist_m < 1000 ? `${spot.dist_m} m` : `${(spot.dist_m / 1000).toFixed(1)} km`}`}
          </p>
        </div>
        <CrowdBadge live={spot.live} showBasis={false} />
      </div>
      <div className="mt-2 flex flex-wrap gap-1 text-[11px] text-slate-600">
        {spot.noise !== null && <span className="rounded bg-slate-100 px-1.5 py-0.5">{formatValue('noise', spot.noise)}</span>}
        {spot.outlets !== null && spot.outlets > 0 && (
          <span className="rounded bg-slate-100 px-1.5 py-0.5">Outlets: {formatValue('outlets', spot.outlets)}</span>
        )}
        {spot.vibe && <span className="rounded bg-slate-100 px-1.5 py-0.5">{formatValue('vibe', spot.vibe)}</span>}
        {flags.slice(0, 4).map((a) => (
          <span key={a.key} className="rounded bg-slate-100 px-1.5 py-0.5">
            {a.label_en}
          </span>
        ))}
      </div>
    </Link>
  )
}

function SelectedCard({ spot, onClose }: { spot: SpotListItem; onClose: () => void }) {
  return (
    <div className="absolute inset-x-3 bottom-3 z-10 rounded-xl bg-white p-4 shadow-lg ring-1 ring-slate-200 md:right-auto md:w-96">
      <div className="flex items-start justify-between gap-2">
        <div className="min-w-0">
          <h3 className="truncate font-semibold">{spot.name}</h3>
          <p className="text-xs text-slate-500">{spot.building.name}</p>
        </div>
        <button type="button" onClick={onClose} className="text-slate-400 hover:text-slate-700" aria-label="Close">
          ✕
        </button>
      </div>
      <div className="mt-2 flex items-center justify-between">
        <CrowdBadge live={spot.live} />
        <Link to={`/spots/${spot.id}`} className="text-sm font-medium text-brand-700 hover:underline">
          Details →
        </Link>
      </div>
    </div>
  )
}

export function MapPage() {
  const [params, setParams] = useSearchParams()
  const filters = useMemo(() => parseFilters(params), [params])
  const setFilters = (f: Filters) => setParams(serializeFilters(f), { replace: true })
  // For effects: applies a change to the latest filters, not a stale copy.
  const updateFilters = useCallback(
    (fn: (f: Filters) => Filters) => setParams((prev) => serializeFilters(fn(parseFilters(prev))), { replace: true }),
    [setParams],
  )
  const [campusSlug] = useSelectedCampus()
  const campus = useCampus(campusSlug)
  const live = useLive()
  const navigate = useNavigate()

  const [text, setText] = useState(filters.q)
  const [pos, setPos] = useState<{ lat: number; lon: number }>()
  const [posError, setPosError] = useState(false)
  const [showFilters, setShowFilters] = useState(false)
  const [mobileList, setMobileList] = useState(false)
  const [selected, setSelected] = useState<number | null>(null)

  // Search text is committed to the URL after a short pause.
  useEffect(() => {
    const t = setTimeout(() => updateFilters((f) => (f.q === text ? f : { ...f, q: text })), 300)
    return () => clearTimeout(t)
  }, [text, updateFilters])

  useEffect(() => {
    if (!filters.near || pos) return
    let cancelled = false
    getPosition().then((p) => {
      if (cancelled) return
      if (p) setPos(p)
      else {
        setPosError(true)
        updateFilters((f) => ({ ...f, near: false }))
      }
    })
    return () => {
      cancelled = true
    }
  }, [filters.near, pos, updateFilters])

  const waitingForPos = filters.near && !pos
  const spots = useSpots(toSpotQuery(filters, campusSlug ?? '', pos), !!campusSlug && !waitingForPos)
  const list = spots.data?.spots ?? []
  const selectedSpot = list.find((s) => s.id === selected)
  const attrs = campus.data?.attrs ?? []
  const nFilters = activeFilterCount(filters)

  const onViewChange = (bbox: Bbox) => {
    if (campusSlug) live.view(campusSlug, bbox)
  }

  const results = (
    <div className="space-y-2">
      <div className="flex items-center justify-between text-xs text-slate-500">
        <span>{spots.isSuccess ? `${list.length}${spots.data.next ? '+' : ''} spots` : ' '}</span>
        <span className="flex items-center gap-3">
          <span className="flex items-center gap-1"><CrowdDot color="green" />free</span>
          <span className="flex items-center gap-1"><CrowdDot color="yellow" />busy</span>
          <span className="flex items-center gap-1"><CrowdDot color="red" />packed</span>
          <span className="flex items-center gap-1"><CrowdDot color="none" />no reports</span>
        </span>
      </div>
      <ErrorBox error={spots.error} />
      {spots.isPending && !waitingForPos && <Loading />}
      {waitingForPos && <Loading label="Finding your location…" />}
      {spots.isSuccess && list.length === 0 && (
        <p className="rounded-xl bg-white p-6 text-center text-sm text-slate-500 ring-1 ring-slate-200">
          No spots match. Try fewer filters.
        </p>
      )}
      {list.map((s) => (
        <SpotRow key={s.id} spot={s} attrs={attrs} active={s.id === selected} />
      ))}
      <Link to="/spots/new" className="block rounded-xl border-2 border-dashed border-slate-300 p-3 text-center text-sm font-medium text-slate-600 hover:border-brand-500 hover:text-brand-700">
        + Suggest a new spot
      </Link>
    </div>
  )

  const searchBar = (
    <div className="flex gap-2">
      <Input
        type="search"
        placeholder="Search spots…"
        value={text}
        onChange={(e) => setText(e.target.value)}
        aria-label="Search spots"
      />
      <Button
        variant={filters.near ? 'primary' : 'secondary'}
        onClick={() => {
          setPosError(false)
          setFilters({ ...filters, near: !filters.near })
        }}
        title="Sort by distance from me"
        aria-pressed={filters.near}
      >
        Near me
      </Button>
      <Button
        variant={showFilters ? 'primary' : 'secondary'}
        onClick={() => {
          // On mobile the panel lives in the list pane.
          if (!showFilters) setMobileList(true)
          setShowFilters(!showFilters)
        }}
        aria-expanded={showFilters}
      >
        Filters{nFilters > 0 && <span className="rounded-full bg-white/25 px-1.5 text-xs">{nFilters}</span>}
      </Button>
    </div>
  )

  return (
    <div className="flex h-full">
      {/* Sidebar (desktop) / list view (mobile) */}
      <aside
        className={cx(
          'flex-col border-r border-slate-200 bg-slate-50 md:flex md:w-[400px] md:shrink-0',
          mobileList ? 'flex w-full' : 'hidden',
        )}
      >
        <div className="space-y-3 border-b border-slate-200 bg-white p-3">
          {searchBar}
          {posError && <p className="text-xs text-red-700">Location unavailable, so results are not sorted by distance.</p>}
        </div>
        <div className="min-h-0 flex-1 overflow-y-auto p-3">
          {showFilters ? (
            <div className="rounded-xl bg-white p-4 ring-1 ring-slate-200">
              {campus.isPending ? <Loading /> : <FilterPanel attrs={attrs} filters={filters} onChange={setFilters} />}
              <Button className="mt-3 w-full" onClick={() => setShowFilters(false)}>
                Show {spots.isSuccess ? list.length : ''} results
              </Button>
            </div>
          ) : (
            results
          )}
        </div>
      </aside>

      {/* Map */}
      <div className={cx('relative min-w-0 flex-1', mobileList && 'hidden md:block')}>
        <div className="absolute inset-x-3 top-3 z-10 md:hidden">
          <div className="rounded-xl bg-white p-2 shadow-md ring-1 ring-slate-200">{searchBar}</div>
        </div>
        <Suspense fallback={<div className="h-full w-full animate-pulse bg-slate-200" />}>
          <SpotMap
            spots={list}
            bbox={campus.data?.bbox ?? null}
            selected={selected}
            onSelect={(id) => (id !== null && id === selected ? navigate(`/spots/${id}`) : setSelected(id))}
            onViewChange={onViewChange}
          />
        </Suspense>
        {selectedSpot && <SelectedCard spot={selectedSpot} onClose={() => setSelected(null)} />}
      </div>

      <button
        type="button"
        onClick={() => {
          setMobileList((v) => !v)
          setShowFilters(false)
        }}
        className="fixed bottom-20 left-1/2 z-20 -translate-x-1/2 rounded-full bg-slate-900 px-4 py-2 text-sm font-medium text-white shadow-lg md:hidden"
      >
        {mobileList ? 'Show map' : `List${spots.isSuccess ? ` (${list.length})` : ''}`}
      </button>
    </div>
  )
}
