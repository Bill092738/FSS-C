import type { Position } from './api'

/**
 * The device position for a report (roadmap 10.2). Resolves to undefined when
 * the user denies it or it is unavailable: the server still accepts the report,
 * with the lowest geofence weight.
 */
export function getPosition(timeoutMs = 8000): Promise<Position | undefined> {
  if (!('geolocation' in navigator)) return Promise.resolve(undefined)
  return new Promise((resolve) => {
    navigator.geolocation.getCurrentPosition(
      (p) => resolve({ lat: p.coords.latitude, lon: p.coords.longitude, accuracy_m: Math.round(p.coords.accuracy) }),
      () => resolve(undefined),
      { enableHighAccuracy: true, timeout: timeoutMs, maximumAge: 30_000 },
    )
  })
}
