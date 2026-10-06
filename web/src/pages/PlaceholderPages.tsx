import type { ReactNode } from 'react'

import { ComingSoon } from '../components/ComingSoon'
import { Button, Card, Input } from '../components/ui'
import { FEATURES, type FeatureKey } from '../lib/features'

/**
 * A page for a feature whose backend is not built yet. `preview` is an inert
 * sketch of the planned UI so the layout can be reviewed early; it never
 * shows data.
 */
function PlaceholderPage({ feature, preview }: { feature: FeatureKey; preview?: ReactNode }) {
  return (
    <div className="mx-auto max-w-3xl space-y-4 p-4">
      <h1 className="text-xl font-bold">{FEATURES[feature].title}</h1>
      <ComingSoon feature={feature} />
      {preview && (
        <div aria-hidden className="pointer-events-none select-none opacity-50 grayscale">
          <p className="mb-2 text-xs font-semibold tracking-wide text-slate-500 uppercase">Layout preview</p>
          {preview}
        </div>
      )}
    </div>
  )
}

const Bar = ({ w }: { w: string }) => <div className="h-3 rounded bg-slate-200" style={{ width: w }} />

export function StudyWithMePage() {
  return (
    <PlaceholderPage
      feature="studyWithMe"
      preview={
        <div className="space-y-2">
          {['78%', '64%', '70%'].map((w) => (
            <Card key={w} className="flex items-center justify-between">
              <div className="w-2/3 space-y-2">
                <Bar w={w} />
                <Bar w="40%" />
              </div>
              <Button variant="secondary" tabIndex={-1}>
                Join
              </Button>
            </Card>
          ))}
          <Button className="w-full" tabIndex={-1}>
            Open a room at my spot
          </Button>
        </div>
      }
    />
  )
}

export function HeatmapPage() {
  return (
    <PlaceholderPage
      feature="heatmap"
      preview={
        <Card>
          <div className="mb-3 flex gap-2">
            {['All', 'Engineering', 'Arts', 'Business'].map((d) => (
              <span key={d} className="rounded-full bg-slate-100 px-2.5 py-1 text-xs">
                {d}
              </span>
            ))}
          </div>
          <div className="h-64 rounded-lg bg-gradient-to-br from-slate-100 via-slate-200 to-slate-100" />
        </Card>
      }
    />
  )
}

export function MerchantPage() {
  return (
    <PlaceholderPage
      feature="merchant"
      preview={
        <Card className="flex gap-2">
          <Input placeholder="Coupon code" tabIndex={-1} />
          <Button tabIndex={-1}>Redeem</Button>
        </Card>
      }
    />
  )
}

export function AdminPage() {
  return (
    <PlaceholderPage
      feature="admin"
      preview={
        <Card className="space-y-3">
          <div className="grid grid-cols-3 gap-3">
            {[1, 2, 3].map((i) => (
              <div key={i} className="h-16 rounded-lg bg-slate-100" />
            ))}
          </div>
          <div className="h-48 rounded-lg bg-slate-100" />
          <Button variant="secondary" tabIndex={-1}>
            Export CSV
          </Button>
        </Card>
      }
    />
  )
}
