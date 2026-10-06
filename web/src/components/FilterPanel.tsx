import { GROUP_LABELS, PRESETS, formatValue, sortedAttrs } from '../lib/attrs'
import { EMPTY_FILTERS, cycleFlag, flagState, type Filters } from '../lib/filters'
import type { AttrDef } from '../lib/types'
import { Button } from './ui'
import { cx } from '../lib/cx'

const QUIET_LEVELS = ['Any', 'Some quiet', 'Quiet', 'Silent']

/** Ordinals the server accepts in `must` (meaning "at least 1"). */
const MUST_ORDINALS = new Set(['outlets'])

function Segmented<T extends string | number>({
  options,
  value,
  onChange,
  label,
}: {
  options: { value: T; label: string }[]
  value: T
  onChange: (v: T) => void
  label: string
}) {
  return (
    <div role="radiogroup" aria-label={label} className="flex rounded-lg bg-slate-100 p-0.5">
      {options.map((o) => (
        <button
          key={String(o.value)}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          onClick={() => onChange(o.value)}
          className={cx(
            'flex-1 rounded-md px-2 py-1.5 text-xs font-medium transition-colors',
            o.value === value ? 'bg-white text-slate-900 shadow-sm' : 'text-slate-600 hover:text-slate-900',
          )}
        >
          {o.label}
        </button>
      ))}
    </div>
  )
}

function FlagChip({ def, filters, onChange }: { def: AttrDef; filters: Filters; onChange: (f: Filters) => void }) {
  const state = flagState(filters, def.key)
  const canExclude = def.kind === 'flag'
  const title = state === 'any' ? 'Any' : state === 'must' ? 'Required' : 'Excluded'
  return (
    <button
      type="button"
      title={`${def.label_en}: ${title}. Click to change.`}
      aria-pressed={state !== 'any'}
      onClick={() => onChange(cycleFlag(filters, def.key, canExclude))}
      className={cx(
        'rounded-full px-2.5 py-1 text-xs font-medium ring-1 transition-colors',
        state === 'any' && 'bg-white text-slate-700 ring-slate-300 hover:bg-slate-50',
        state === 'must' && 'bg-brand-700 text-white ring-brand-700',
        state === 'not' && 'bg-red-50 text-red-700 line-through ring-red-300',
      )}
    >
      {state === 'must' && '✓ '}
      {def.label_en}
    </button>
  )
}

export function FilterPanel({
  attrs,
  filters,
  onChange,
}: {
  attrs: AttrDef[]
  filters: Filters
  onChange: (f: Filters) => void
}) {
  const filterable = sortedAttrs(attrs).filter((a) => a.kind === 'flag' || MUST_ORDINALS.has(a.key))
  const groups = (Object.keys(GROUP_LABELS) as AttrDef['group'][])
    .map((g) => ({ g, items: filterable.filter((a) => a.group === g) }))
    .filter((x) => x.items.length)
  const vibeDef = attrs.find((a) => a.key === 'vibe')
  const vibes = Array.isArray(vibeDef?.domain) ? vibeDef.domain : []

  return (
    <div className="space-y-5">
      <div>
        <h3 className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">Scenarios</h3>
        <div className="flex flex-wrap gap-1.5">
          {PRESETS.map((p) => (
            <button
              key={p.name}
              type="button"
              onClick={() => onChange({ ...EMPTY_FILTERS, q: filters.q, near: filters.near, must: p.must, quiet: p.quiet, vibe: p.vibe ?? '' })}
              className="rounded-full bg-brand-50 px-2.5 py-1 text-xs font-medium text-brand-800 ring-1 ring-brand-100 hover:bg-brand-100"
            >
              {p.name}
            </button>
          ))}
        </div>
      </div>

      <div>
        <h3 className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">Quiet</h3>
        <Segmented
          label="Quiet level"
          value={filters.quiet}
          onChange={(quiet) => onChange({ ...filters, quiet })}
          options={QUIET_LEVELS.map((label, value) => ({ value, label }))}
        />
      </div>

      {vibes.length > 0 && (
        <div>
          <h3 className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">Vibe</h3>
          <Segmented
            label="Vibe"
            value={filters.vibe}
            onChange={(vibe) => onChange({ ...filters, vibe })}
            options={[{ value: '', label: 'Any' }, ...vibes.map((v) => ({ value: v, label: formatValue('vibe', v) }))]}
          />
        </div>
      )}

      {groups.map(({ g, items }) => (
        <div key={g}>
          <h3 className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">{GROUP_LABELS[g]}</h3>
          <div className="flex flex-wrap gap-1.5">
            {items.map((a) => (
              <FlagChip key={a.key} def={a} filters={filters} onChange={onChange} />
            ))}
          </div>
        </div>
      ))}
      <p className="text-xs text-slate-500">Tap a feature once to require it, twice to exclude it.</p>

      <Button variant="ghost" className="w-full" onClick={() => onChange({ ...EMPTY_FILTERS, q: filters.q })}>
        Clear filters
      </Button>
    </div>
  )
}
