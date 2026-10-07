import { useState, type FormEvent } from 'react'
import { Link, Navigate, useNavigate, useSearchParams } from 'react-router'

import { Button, Card, ErrorBox, Field, Input, Select } from '../components/ui'
import { useAuthActions } from '../lib/auth'
import { useCampuses, useMe, useSelectedCampus } from '../lib/queries'

/** Only same-site paths are followed after login. */
function safeNext(p: string | null): string {
  return p && p.startsWith('/') && !p.startsWith('//') ? p : '/'
}

export function AuthPage({ mode }: { mode: 'login' | 'register' }) {
  const [params] = useSearchParams()
  const next = safeNext(params.get('next'))
  const navigate = useNavigate()
  const me = useMe()
  const { login, register } = useAuthActions()
  const campuses = useCampuses()
  const [selected] = useSelectedCampus()
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [name, setName] = useState('')
  const [campus, setCampus] = useState('')

  if (me.data) return <Navigate to={next} replace />

  const action = mode === 'login' ? login : register
  const submit = (e: FormEvent) => {
    e.preventDefault()
    const done = { onSuccess: () => navigate(next, { replace: true }) }
    if (mode === 'login') login.mutate({ email, password }, done)
    else
      register.mutate(
        { email, password, display_name: name.trim() || undefined, campus: campus || selected || undefined },
        done,
      )
  }

  return (
    <div className="mx-auto max-w-sm p-4 pt-10">
      <Card className="p-6">
        <h1 className="text-xl font-bold">{mode === 'login' ? 'Welcome back' : 'Create your account'}</h1>
        <p className="mt-1 text-sm text-slate-500">
          {mode === 'login' ? 'Log in to report crowds and vote on spots.' : 'Report crowds, fix spot details and earn karma.'}
        </p>
        <form onSubmit={submit} className="mt-6 space-y-4">
          <Field label="Email">
            <Input type="email" autoComplete="email" required value={email} onChange={(e) => setEmail(e.target.value)} />
          </Field>
          <Field label="Password" hint={mode === 'register' ? 'At least 8 characters.' : undefined}>
            <Input
              type="password"
              autoComplete={mode === 'login' ? 'current-password' : 'new-password'}
              required
              minLength={mode === 'register' ? 8 : undefined}
              value={password}
              onChange={(e) => setPassword(e.target.value)}
            />
          </Field>
          {mode === 'register' && (
            <>
              <Field label="Display name (optional)">
                <Input maxLength={60} value={name} onChange={(e) => setName(e.target.value)} />
              </Field>
              <Field label="Campus">
                <Select value={campus || selected || ''} onChange={(e) => setCampus(e.target.value)}>
                  {campuses.data?.campuses.map((c) => (
                    <option key={c.slug} value={c.slug}>
                      {c.name}
                    </option>
                  ))}
                </Select>
              </Field>
            </>
          )}
          <ErrorBox error={action.error} />
          <Button type="submit" className="w-full" busy={action.isPending}>
            {mode === 'login' ? 'Log in' : 'Sign up'}
          </Button>
        </form>
        <p className="mt-4 text-center text-sm text-slate-600">
          {mode === 'login' ? (
            <>
              New here?{' '}
              <Link to={`/register${next !== '/' ? `?next=${encodeURIComponent(next)}` : ''}`} className="font-medium text-brand-700 hover:underline">
                Create an account
              </Link>
            </>
          ) : (
            <>
              Already have an account?{' '}
              <Link to="/login" className="font-medium text-brand-700 hover:underline">
                Log in
              </Link>
            </>
          )}
        </p>
      </Card>
    </div>
  )
}
