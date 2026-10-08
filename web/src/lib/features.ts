// Front-end features whose backend is not built yet (see PROGRESS.md).
//
// Every placeholder in the UI reads its text from here. When a milestone ships
// its endpoint, set `ready: true`, wire the call into lib/api.ts and replace
// the <ComingSoon> where the feature is used.

export type Milestone = 'M4' | 'M5' | 'M6' | 'backlog'

export interface PendingFeature {
  title: string
  milestone: Milestone
  /** Endpoints (relative to /api/v1) or channels the feature waits for. */
  endpoints: string[]
  description: string
  ready: boolean
}

export const MILESTONE_NAMES: Record<Milestone, string> = {
  M4: 'Community',
  M5: 'Forecast & Phase 2',
  M6: 'Monetization & admin',
  backlog: 'Not in the roadmap yet',
}

export const FEATURES = {
  forecast: {
    title: 'Crowd forecast',
    milestone: 'M5',
    endpoints: ['GET /spots/:id/forecast?date='],
    description: "Today's expected crowding curve with the peak hours marked.",
    ready: false,
  },
  studyWithMe: {
    title: 'Study With Me',
    milestone: 'M5',
    endpoints: ['GET /swm', 'POST /swm', 'POST /swm/:id/join', 'POST /swm/:id/leave', 'ws swm:{id}'],
    description: 'Open a study room at your spot and match with classmates by course and language.',
    ready: false,
  },
  heatmap: {
    title: 'Campus heatmap',
    milestone: 'M5',
    endpoints: ['GET /campuses/:slug/heatmap'],
    description: 'Where people study, by hour and major group (k-anonymous, cells with fewer than 5 people hidden).',
    ready: false,
  },
  coupons: {
    title: 'Coupons',
    milestone: 'M6',
    endpoints: ['GET /coupons', 'POST /offers/:id/redeem', 'ws user:{id} {"t":"coupon"}'],
    description: 'Offers from nearby merchants, unlocked by study time or karma.',
    ready: false,
  },
  merchant: {
    title: 'Merchant redeem',
    milestone: 'M6',
    endpoints: ['POST /merchant/coupons/:code/redeem'],
    description: 'Merchants enter a coupon code to redeem it.',
    ready: false,
  },
  admin: {
    title: 'Space insights',
    milestone: 'M6',
    endpoints: ['GET /admin/insights', 'GET /admin/insights.csv'],
    description: 'Utilization by building and hour, peak/off-peak ratio and facility issues for campus admins.',
    ready: false,
  },
  buildings: {
    title: 'Building list',
    milestone: 'backlog',
    endpoints: ['GET /campuses/:slug/buildings'],
    description:
      'No endpoint lists buildings yet, so the new-spot form offers the buildings that already have active spots.',
    ready: false,
  },
} satisfies Record<string, PendingFeature>

export type FeatureKey = keyof typeof FEATURES
