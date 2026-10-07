import { createBrowserRouter } from 'react-router'

import { AuthPage } from '../pages/AuthPage'
import { MapPage } from '../pages/MapPage'
import { MePage } from '../pages/MePage'
import { NewSpotPage } from '../pages/NewSpotPage'
import { NotFoundPage } from '../pages/NotFoundPage'
import { AdminPage, HeatmapPage, MerchantPage, StudyWithMePage } from '../pages/PlaceholderPages'
import { SpotPage } from '../pages/SpotPage'
import { AppShell } from './AppShell'

// Pages from roadmap 10.2. The C server answers unknown extension-less paths
// with index.html (SPA fallback in server/src/main.c), so deep links work.
export const router = createBrowserRouter([
  {
    element: <AppShell />,
    children: [
      { path: '/', element: <MapPage /> },
      { path: '/spots/new', element: <NewSpotPage /> },
      { path: '/spots/:id', element: <SpotPage /> },
      { path: '/login', element: <AuthPage mode="login" /> },
      { path: '/register', element: <AuthPage mode="register" /> },
      { path: '/me', element: <MePage /> },
      { path: '/study', element: <StudyWithMePage /> },
      { path: '/heatmap', element: <HeatmapPage /> },
      { path: '/merchant', element: <MerchantPage /> },
      { path: '/admin', element: <AdminPage /> },
      { path: '*', element: <NotFoundPage /> },
    ],
  },
])
