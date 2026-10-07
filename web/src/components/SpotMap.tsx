import { Map as MapLibre, NavigationControl, setWorkerUrl, type GeoJSONSource, type StyleSpecification } from 'maplibre-gl'
import 'maplibre-gl/dist/maplibre-gl.css'
import workerUrl from 'maplibre-gl/dist/maplibre-gl-worker.mjs?url'
import type { FeatureCollection } from 'geojson'
import { useEffect, useRef, useState } from 'react'

import { CROWD_HEX } from '../lib/attrs'
import type { Bbox, SpotListItem } from '../lib/types'

// MapLibre derives its worker URL from import.meta.url, which points into
// Vite's dependency cache in dev and is not emitted by the build. The worker
// file is self-contained, so hand Vite's asset URL to MapLibre instead.
setWorkerUrl(workerUrl)

// OSM raster tiles need no key (roadmap 1). Set VITE_MAP_STYLE to a style URL
// to use vector tiles instead. Mind the OSM tile usage policy for real traffic.
const OSM_STYLE: StyleSpecification = {
  version: 8,
  sources: {
    osm: {
      type: 'raster',
      tiles: ['https://tile.openstreetmap.org/{z}/{x}/{y}.png'],
      tileSize: 256,
      maxzoom: 19,
      attribution: '© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
    },
  },
  layers: [{ id: 'osm', type: 'raster', source: 'osm' }],
}

function webglAvailable(): boolean {
  try {
    const c = document.createElement('canvas')
    return !!(c.getContext('webgl2') ?? c.getContext('webgl'))
  } catch {
    return false
  }
}

function toGeoJSON(spots: SpotListItem[], selected: number | null): FeatureCollection {
  return {
    type: 'FeatureCollection',
    features: spots.map((s) => ({
      type: 'Feature',
      id: s.id,
      geometry: { type: 'Point', coordinates: [s.lon, s.lat] },
      properties: { id: s.id, name: s.name, color: s.live?.color ?? 'none', selected: s.id === selected },
    })),
  }
}

interface Props {
  spots: SpotListItem[]
  bbox: Bbox | null
  selected: number | null
  onSelect: (id: number | null) => void
  onViewChange?: (bbox: Bbox) => void
}

export function SpotMap({ spots, bbox, selected, onSelect, onViewChange }: Props) {
  const container = useRef<HTMLDivElement>(null)
  const map = useRef<MapLibre | null>(null)
  const [ready, setReady] = useState(false)
  const [hasWebGL] = useState(webglAvailable)
  const handlers = useRef({ onSelect, onViewChange })
  useEffect(() => {
    handlers.current = { onSelect, onViewChange }
  })
  const bboxKey = bbox?.join(',')

  useEffect(() => {
    if (!container.current || !hasWebGL) return
    const m = new MapLibre({
      container: container.current,
      style: import.meta.env.VITE_MAP_STYLE || OSM_STYLE,
      attributionControl: { compact: true },
    })
    map.current = m
    // On phones the search card covers the top of the map.
    const narrow = window.matchMedia('(max-width: 767px)').matches
    m.addControl(new NavigationControl({ showCompass: false }), narrow ? 'bottom-right' : 'top-right')
    const emitView = () => {
      const b = m.getBounds()
      handlers.current.onViewChange?.([b.getWest(), b.getSouth(), b.getEast(), b.getNorth()])
    }
    m.on('load', () => {
      m.addSource('spots', { type: 'geojson', data: toGeoJSON([], null) })
      m.addLayer({
        id: 'spots',
        type: 'circle',
        source: 'spots',
        paint: {
          'circle-color': [
            'match',
            ['get', 'color'],
            'green', CROWD_HEX.green,
            'yellow', CROWD_HEX.yellow,
            'red', CROWD_HEX.red,
            CROWD_HEX.none,
          ],
          'circle-radius': ['case', ['get', 'selected'], 11, 8],
          'circle-stroke-color': ['case', ['get', 'selected'], '#0f172a', '#ffffff'],
          'circle-stroke-width': ['case', ['get', 'selected'], 3, 2],
        },
      })
      m.on('click', 'spots', (e) => {
        const id = e.features?.[0]?.properties?.id
        if (typeof id === 'number') handlers.current.onSelect(id)
      })
      m.on('click', (e) => {
        if (!m.queryRenderedFeatures(e.point, { layers: ['spots'] }).length) handlers.current.onSelect(null)
      })
      m.on('mouseenter', 'spots', () => (m.getCanvas().style.cursor = 'pointer'))
      m.on('mouseleave', 'spots', () => (m.getCanvas().style.cursor = ''))
      // The first view is emitted after the campus fit below.
      m.on('moveend', emitView)
      setReady(true)
    })
    return () => {
      m.remove()
      map.current = null
      setReady(false)
    }
  }, [hasWebGL])

  useEffect(() => {
    if (!ready) return
    ;(map.current?.getSource('spots') as GeoJSONSource | undefined)?.setData(toGeoJSON(spots, selected))
  }, [ready, spots, selected])

  // Keyed by value so a new array with the same campus bbox does not refit.
  useEffect(() => {
    if (!ready || !bboxKey) return
    const b = bboxKey.split(',').map(Number) as Bbox
    map.current?.fitBounds(b, { padding: 40, duration: 0 })
  }, [ready, bboxKey])

  if (!hasWebGL)
    return (
      <div className="flex h-full items-center justify-center bg-slate-100 p-6 text-center text-sm text-slate-500">
        The map needs WebGL, which this browser does not provide. The list still works.
      </div>
    )
  return <div ref={container} className="h-full w-full" data-testid="spot-map" />
}
