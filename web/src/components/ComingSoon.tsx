import { FEATURES, MILESTONE_NAMES, type FeatureKey } from '../lib/features'
import { cx } from '../lib/cx'

/** Placeholder for a feature whose backend is not built yet (lib/features.ts). */
export function ComingSoon({ feature, compact, className }: { feature: FeatureKey; compact?: boolean; className?: string }) {
  const f = FEATURES[feature]
  return (
    <div
      className={cx(
        'rounded-xl border-2 border-dashed border-slate-300 bg-slate-50/60 text-slate-600',
        compact ? 'p-3' : 'p-6',
        className,
      )}
      data-placeholder={feature}
    >
      <div className="flex flex-wrap items-center gap-2">
        <h3 className={cx('font-semibold text-slate-800', compact ? 'text-sm' : 'text-base')}>{f.title}</h3>
        <span className="rounded-full bg-amber-100 px-2 py-0.5 text-xs font-medium text-amber-800">
          {f.milestone === 'backlog' ? 'Backlog' : `Coming in ${f.milestone}`} · {MILESTONE_NAMES[f.milestone]}
        </span>
      </div>
      <p className={cx('mt-1', compact ? 'text-xs' : 'text-sm')}>{f.description}</p>
      {!compact && (
        <div className="mt-3 flex flex-wrap items-center gap-1 text-xs text-slate-500">
          Waiting for:
          {f.endpoints.map((e) => (
            <code key={e} className="rounded bg-slate-200/70 px-1 py-0.5 font-mono text-[11px] text-slate-700">
              {e}
            </code>
          ))}
        </div>
      )}
    </div>
  )
}
