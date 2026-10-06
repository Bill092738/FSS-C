import { Link, NavLink, Outlet } from 'react-router'

import { Select } from '../components/ui'
import { cx } from '../lib/cx'
import { useLiveStatus } from '../lib/liveContext'
import { useCampuses, useMe, useSelectedCampus } from '../lib/queries'

const NAV = [
  { to: '/', label: 'Map', icon: 'M9 20l-5.447-2.724A1 1 0 0 1 3 16.382V5.618a1 1 0 0 1 1.447-.894L9 7m0 13l6-3m-6 3V7m6 10l4.553 2.276A1 1 0 0 0 21 18.382V7.618a1 1 0 0 0-.553-.894L15 4m0 13V4m0 0L9 7' },
  { to: '/study', label: 'Study With Me', icon: 'M17 20h5v-2a3 3 0 0 0-5.356-1.857M17 20H7m10 0v-2c0-.656-.126-1.283-.356-1.857M7 20H2v-2a3 3 0 0 1 5.356-1.857M7 20v-2c0-.656.126-1.283.356-1.857m0 0a5.002 5.002 0 0 1 9.288 0M15 7a3 3 0 1 1-6 0 3 3 0 0 1 6 0z' },
  { to: '/heatmap', label: 'Heatmap', icon: 'M17.657 18.657A8 8 0 0 1 6.343 7.343S7 9 9 10c0-2 .5-5 2.986-7C14 5 16.09 5.777 17.656 7.343A7.975 7.975 0 0 1 20 13a7.975 7.975 0 0 1-2.343 5.657z' },
  { to: '/me', label: 'Me', icon: 'M16 7a4 4 0 1 1-8 0 4 4 0 0 1 8 0zM12 14a7 7 0 0 0-7 7h14a7 7 0 0 0-7-7z' },
]

function Icon({ d, className }: { d: string; className?: string }) {
  return (
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" className={className} aria-hidden>
      <path d={d} />
    </svg>
  )
}

function LiveIndicator() {
  const status = useLiveStatus()
  const map = {
    open: ['bg-crowd-green', 'Live updates on'],
    connecting: ['bg-crowd-yellow animate-pulse', 'Connecting…'],
    closed: ['bg-crowd-red', 'Live updates offline, retrying'],
  } as const
  const [dot, label] = map[status]
  return (
    <span className="flex items-center gap-1.5 text-xs text-slate-500" title={label} data-live-status={status}>
      <span className={cx('size-2 rounded-full', dot)} />
      <span className="hidden lg:inline">{status === 'open' ? 'Live' : label}</span>
    </span>
  )
}

function CampusPicker() {
  const campuses = useCampuses()
  const [slug, setSlug] = useSelectedCampus()
  const list = campuses.data?.campuses ?? []
  if (list.length < 2) return <span className="truncate text-sm text-slate-600">{list[0]?.name}</span>
  return (
    <Select aria-label="Campus" value={slug ?? ''} onChange={(e) => setSlug(e.target.value)} className="w-auto py-1.5">
      {list.map((c) => (
        <option key={c.slug} value={c.slug}>
          {c.name}
        </option>
      ))}
    </Select>
  )
}

export function AppShell() {
  const me = useMe()
  return (
    <div className="flex h-full flex-col">
      <header className="z-20 flex h-14 shrink-0 items-center gap-3 border-b border-slate-200 bg-white px-3 sm:px-4">
        <Link to="/" className="flex items-center gap-2 font-bold text-brand-800">
          <img src="/favicon.svg" alt="" className="size-7" />
          <span className="hidden sm:inline">FSS</span>
        </Link>
        <div className="min-w-0 flex-1 sm:flex-none">
          <CampusPicker />
        </div>
        <nav className="ml-4 hidden flex-1 items-center gap-1 md:flex">
          {NAV.map((n) => (
            <NavLink
              key={n.to}
              to={n.to}
              end={n.to === '/'}
              className={({ isActive }) =>
                cx('rounded-lg px-3 py-1.5 text-sm font-medium', isActive ? 'bg-brand-50 text-brand-800' : 'text-slate-600 hover:bg-slate-100')
              }
            >
              {n.label}
            </NavLink>
          ))}
        </nav>
        <LiveIndicator />
        {me.data ? (
          <Link to="/me" className="flex items-center gap-2 rounded-lg px-2 py-1 text-sm hover:bg-slate-100">
            <span className="grid size-7 place-items-center rounded-full bg-brand-100 text-xs font-bold text-brand-800">
              {(me.data.display_name || me.data.email).slice(0, 1).toUpperCase()}
            </span>
            <span className="hidden font-medium sm:inline">{me.data.karma} pts</span>
          </Link>
        ) : (
          me.isSuccess && (
            <Link to="/login" className="rounded-lg bg-brand-700 px-3 py-1.5 text-sm font-medium text-white hover:bg-brand-800">
              Log in
            </Link>
          )
        )}
      </header>

      <main className="relative min-h-0 flex-1 overflow-y-auto">
        <Outlet />
      </main>

      <nav className="grid shrink-0 grid-cols-4 border-t border-slate-200 bg-white pb-[env(safe-area-inset-bottom)] md:hidden">
        {NAV.map((n) => (
          <NavLink
            key={n.to}
            to={n.to}
            end={n.to === '/'}
            className={({ isActive }) =>
              cx('flex flex-col items-center gap-0.5 py-2 text-[11px] font-medium', isActive ? 'text-brand-700' : 'text-slate-500')
            }
          >
            <Icon d={n.icon} className="size-5" />
            {n.label}
          </NavLink>
        ))}
      </nav>
    </div>
  )
}
