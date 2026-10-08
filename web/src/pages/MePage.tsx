import { useState, type FormEvent } from 'react'
import { Link, Navigate, useNavigate } from 'react-router'

import { ComingSoon } from '../components/ComingSoon'
import { KarmaHistory } from '../components/KarmaHistory'
import { Button, Card, ErrorBox, Field, Input, Loading, Select } from '../components/ui'
import { useAuthActions } from '../lib/auth'
import { useCampuses, useMe } from '../lib/queries'
import type { User } from '../lib/types'

const splitList = (s: string) =>
  s
    .split(',')
    .map((x) => x.trim())
    .filter(Boolean)

function ProfileForm({ user }: { user: User }) {
  const { updateProfile } = useAuthActions()
  const campuses = useCampuses()
  const [name, setName] = useState(user.display_name ?? '')
  const [major, setMajor] = useState(user.major ?? '')
  const [campus, setCampus] = useState(user.campus ?? '')
  const [langs, setLangs] = useState(user.langs.join(', '))
  const [courses, setCourses] = useState(user.courses.join(', '))

  const submit = (e: FormEvent) => {
    e.preventDefault()
    updateProfile.mutate({
      display_name: name.trim(),
      major: major.trim(),
      langs: splitList(langs),
      courses: splitList(courses),
      ...(campus ? { campus } : {}),
    })
  }

  return (
    <Card>
      <h2 className="font-semibold">Profile</h2>
      <p className="mt-1 text-xs text-slate-500">Courses and languages will be used to match Study With Me rooms.</p>
      <form onSubmit={submit} className="mt-4 grid gap-4 sm:grid-cols-2">
        <Field label="Display name">
          <Input maxLength={60} value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Major">
          <Input maxLength={60} value={major} onChange={(e) => setMajor(e.target.value)} />
        </Field>
        <Field label="Campus">
          <Select value={campus} onChange={(e) => setCampus(e.target.value)}>
            {!user.campus && <option value="">—</option>}
            {campuses.data?.campuses.map((c) => (
              <option key={c.slug} value={c.slug}>
                {c.name}
              </option>
            ))}
          </Select>
        </Field>
        <Field label="Languages" hint="Comma separated, e.g. en, zh">
          <Input value={langs} onChange={(e) => setLangs(e.target.value)} />
        </Field>
        <div className="sm:col-span-2">
          <Field label="Courses" hint="Comma separated, e.g. CSE 2221, MATH 1151">
            <Input value={courses} onChange={(e) => setCourses(e.target.value)} />
          </Field>
        </div>
        <div className="flex items-center gap-3 sm:col-span-2">
          <Button type="submit" busy={updateProfile.isPending}>
            Save
          </Button>
          {updateProfile.isSuccess && <span className="text-sm text-green-700">Saved.</span>}
        </div>
        <ErrorBox error={updateProfile.error} className="sm:col-span-2" />
      </form>
    </Card>
  )
}

export function MePage() {
  const me = useMe()
  const { logout } = useAuthActions()
  const navigate = useNavigate()

  if (me.isPending) return <Loading />
  if (me.error) return <ErrorBox error={me.error} className="m-4" />
  if (!me.data) return <Navigate to="/login?next=/me" replace />

  const u = me.data
  return (
    <div className="mx-auto max-w-3xl space-y-4 p-4">
      <header className="flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-center gap-3">
          <span className="grid size-12 place-items-center rounded-full bg-brand-100 text-lg font-bold text-brand-800">
            {(u.display_name || u.email).slice(0, 1).toUpperCase()}
          </span>
          <div>
            <h1 className="text-xl font-bold">{u.display_name || u.email}</h1>
            <p className="text-sm text-slate-500">
              {u.email}
              {u.role !== 'student' && ` · ${u.role.replace('_', ' ')}`}
            </p>
          </div>
        </div>
        <Button variant="secondary" busy={logout.isPending} onClick={() => logout.mutate(undefined, { onSuccess: () => navigate('/') })}>
          Log out
        </Button>
      </header>

      <div className="grid grid-cols-2 gap-3">
        <Card>
          <p className="text-xs font-medium text-slate-500 uppercase">Karma</p>
          <p className="mt-1 text-3xl font-bold text-brand-800">{u.karma}</p>
        </Card>
        <Card>
          <p className="text-xs font-medium text-slate-500 uppercase" title="Weights your reports and votes (0.1 to 3.0)">
            Reputation
          </p>
          <p className="mt-1 text-3xl font-bold">{u.reputation.toFixed(2)}</p>
        </Card>
      </div>

      <Card>
        <h2 className="font-semibold">Badges</h2>
        {u.badges.length ? (
          <ul className="mt-2 flex flex-wrap gap-2">
            {u.badges.map((b) => (
              <li key={b.key} className="rounded-full bg-amber-50 px-3 py-1 text-sm text-amber-900 ring-1 ring-amber-200">
                🏅 {b.name}
              </li>
            ))}
          </ul>
        ) : (
          <p className="mt-1 text-sm text-slate-500">No badges yet.</p>
        )}
      </Card>

      <ProfileForm user={u} />

      {u.checkin && (
        <Card className="flex flex-wrap items-center justify-between gap-2">
          <p className="text-sm">
            Checked in since {new Date(u.checkin.start_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
            {!u.checkin.verified && ' (not verified yet)'}.
          </p>
          <Link to={`/spots/${u.checkin.spot}`} className="text-sm font-medium text-brand-700 hover:underline">
            Go to the spot →
          </Link>
        </Card>
      )}

      <div className="grid gap-4 sm:grid-cols-2">
        <KarmaHistory />
        <ComingSoon feature="coupons" compact />
      </div>

      {(u.role === 'merchant' || u.role === 'campus_admin') && (
        <Card>
          <h2 className="font-semibold">Tools</h2>
          <div className="mt-2 flex gap-2">
            {u.role === 'merchant' && (
              <Link to="/merchant" className="text-sm font-medium text-brand-700 hover:underline">
                Redeem coupons →
              </Link>
            )}
            {u.role === 'campus_admin' && (
              <Link to="/admin" className="text-sm font-medium text-brand-700 hover:underline">
                Space insights →
              </Link>
            )}
          </div>
        </Card>
      )}
    </div>
  )
}
