// ============================================================================
// useMetrics.js — Live Engine Metrics Polling Hook
// ============================================================================
// Polls /api/metrics every 2 seconds to keep the dashboard "alive".
// Also fetches /api/memtable/snapshot for the SkipList visualizer.
//
// Returns: { metrics, snapshot, backendStatus, lastUpdated }
// ============================================================================

import { useState, useEffect, useRef, useCallback } from 'react'

const API_BASE = '/api'
const POLL_INTERVAL_MS = 2000

/**
 * @returns {{
 *   metrics: { memtable_size_bytes: number, wal_size_bytes: number, sstable_count: number } | null,
 *   snapshot: Array<{ key: string, value: string, type: 'put'|'tombstone' }>,
 *   backendStatus: 'online'|'offline'|'connecting',
 *   lastUpdated: Date | null,
 * }}
 */
export function useMetrics() {
  const [metrics, setMetrics]             = useState(null)
  const [snapshot, setSnapshot]           = useState([])
  const [backendStatus, setBackendStatus] = useState('connecting')
  const [lastUpdated, setLastUpdated]     = useState(null)

  // Track previous metric values to trigger CSS animation on change
  const prevMetricsRef = useRef(null)

  const fetchAll = useCallback(async () => {
    try {
      const [metricsRes, snapshotRes] = await Promise.all([
        fetch(`${API_BASE}/metrics`),
        fetch(`${API_BASE}/memtable/snapshot`),
      ])

      if (!metricsRes.ok || !snapshotRes.ok) {
        setBackendStatus('offline')
        return
      }

      const metricsData  = await metricsRes.json()
      const snapshotData = await snapshotRes.json()

      prevMetricsRef.current = metrics
      setMetrics(metricsData)
      setSnapshot(snapshotData.entries ?? [])
      setBackendStatus('online')
      setLastUpdated(new Date())
    } catch {
      setBackendStatus('offline')
    }
  }, [metrics])

  useEffect(() => {
    // Immediate fetch on mount
    fetchAll()

    const intervalId = setInterval(fetchAll, POLL_INTERVAL_MS)
    return () => clearInterval(intervalId)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  return { metrics, snapshot, backendStatus, lastUpdated, refetch: fetchAll }
}
