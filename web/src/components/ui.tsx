import type { ButtonHTMLAttributes, InputHTMLAttributes, ReactNode, SelectHTMLAttributes } from 'react'

import { ApiError } from '../lib/api'
import { cx } from '../lib/cx'

type Variant = 'primary' | 'secondary' | 'ghost' | 'danger'

const VARIANTS: Record<Variant, string> = {
  primary: 'bg-brand-700 text-white hover:bg-brand-800 disabled:bg-slate-300',
  secondary: 'bg-white text-slate-800 ring-1 ring-slate-300 hover:bg-slate-50 disabled:text-slate-400',
  ghost: 'text-slate-700 hover:bg-slate-100 disabled:text-slate-400',
  danger: 'bg-red-600 text-white hover:bg-red-700 disabled:bg-slate-300',
}

export function Button({
  variant = 'primary',
  className,
  busy,
  children,
  ...rest
}: ButtonHTMLAttributes<HTMLButtonElement> & { variant?: Variant; busy?: boolean }) {
  return (
    <button
      type="button"
      {...rest}
      disabled={rest.disabled || busy}
      className={cx(
        'inline-flex shrink-0 items-center justify-center gap-2 rounded-lg px-3 py-2 text-sm font-medium whitespace-nowrap transition-colors',
        'focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-brand-600 disabled:cursor-not-allowed',
        VARIANTS[variant],
        className,
      )}
    >
      {busy && <Spinner className="size-4" />}
      {children}
    </button>
  )
}

export function Card({ className, children }: { className?: string; children: ReactNode }) {
  return <section className={cx('rounded-xl bg-white p-4 shadow-sm ring-1 ring-slate-200', className)}>{children}</section>
}

export function Pill({ className, children }: { className?: string; children: ReactNode }) {
  return (
    <span className={cx('inline-flex items-center gap-1 rounded-full px-2 py-0.5 text-xs font-medium', className)}>
      {children}
    </span>
  )
}

export function Spinner({ className }: { className?: string }) {
  return (
    <svg className={cx('animate-spin', className ?? 'size-5')} viewBox="0 0 24 24" fill="none" aria-hidden>
      <circle cx="12" cy="12" r="10" stroke="currentColor" strokeOpacity=".25" strokeWidth="4" />
      <path d="M22 12a10 10 0 0 0-10-10" stroke="currentColor" strokeWidth="4" strokeLinecap="round" />
    </svg>
  )
}

export function Loading({ label = 'Loading…' }: { label?: string }) {
  return (
    <div className="flex items-center gap-2 p-6 text-sm text-slate-500">
      <Spinner /> {label}
    </div>
  )
}

const FRIENDLY: Record<string, string> = {
  unauthorized: 'Please log in first.',
  rate_limited: 'Too many requests. Try again later.',
  network: 'Cannot reach the FSS server. Is it running?',
}

function errorText(err: unknown): string {
  if (err instanceof ApiError) return FRIENDLY[err.code] ?? err.message
  if (err instanceof Error) return err.message
  return 'Something went wrong.'
}

export function ErrorBox({ error, className }: { error: unknown; className?: string }) {
  if (!error) return null
  return (
    <div role="alert" className={cx('rounded-lg bg-red-50 px-3 py-2 text-sm text-red-800 ring-1 ring-red-200', className)}>
      {errorText(error)}
      {error instanceof ApiError && error.code !== 'network' && (
        <span className="ml-1 font-mono text-xs text-red-500">({error.code})</span>
      )}
    </div>
  )
}

export function Field({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return (
    <label className="block space-y-1">
      <span className="text-sm font-medium text-slate-700">{label}</span>
      {children}
      {hint && <span className="block text-xs text-slate-500">{hint}</span>}
    </label>
  )
}

const INPUT =
  'block w-full rounded-lg border-0 bg-white px-3 py-2 text-sm ring-1 ring-slate-300 placeholder:text-slate-400 focus:ring-2 focus:ring-brand-600 focus:outline-none'

export function Input(props: InputHTMLAttributes<HTMLInputElement>) {
  return <input {...props} className={cx(INPUT, props.className)} />
}

export function Select(props: SelectHTMLAttributes<HTMLSelectElement>) {
  return <select {...props} className={cx(INPUT, 'pr-8', props.className)} />
}
