import { Link } from 'react-router'

export function NotFoundPage() {
  return (
    <div className="mx-auto max-w-md p-10 text-center">
      <p className="text-5xl font-bold text-slate-300">404</p>
      <h1 className="mt-2 text-lg font-semibold">Nothing here</h1>
      <p className="mt-1 text-sm text-slate-500">This page or spot does not exist.</p>
      <Link to="/" className="mt-4 inline-block text-sm font-medium text-brand-700 hover:underline">
        Back to the map
      </Link>
    </div>
  )
}
