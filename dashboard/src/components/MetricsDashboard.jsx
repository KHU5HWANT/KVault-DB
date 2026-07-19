// ============================================================================
// MetricsDashboard.jsx — Live Engine Metrics
// ============================================================================
// Displays MemTable size, WAL size, and SSTable count as animated cards.
// Numbers pop when they change (CSS animation triggered via key prop trick).
// ============================================================================

import { useRef, useEffect, useState } from 'react'
import './MetricsDashboard.css'

// Flush threshold — should match EngineConfig::memtable_flush_threshold_bytes
const MEMTABLE_THRESHOLD_BYTES = 4096

function formatBytes(bytes) {
  if (bytes === null || bytes === undefined) return '—'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

/**
 * Single metric card with optional progress bar and animated value.
 */
function MetricCard({ icon, label, value, sub, accent1, accent2, fill, fillMax, delay }) {
  const [animKey, setAnimKey] = useState(0)
  const prevValue = useRef(value)

  // Trigger pop animation when value changes
  useEffect(() => {
    if (prevValue.current !== value) {
      setAnimKey(k => k + 1)
      prevValue.current = value
    }
  }, [value])

  const pct = fillMax ? Math.min(100, (fill / fillMax) * 100) : 0

  return (
    <div
      className="glass-card metric-card fade-up"
      style={{
        '--card-accent-1': accent1,
        '--card-accent-2': accent2,
        animationDelay: delay,
      }}
    >
      <div className="metric-icon">{icon}</div>
      <div className="metric-label">{label}</div>
      <div className="metric-value changed" key={animKey}>
        {value ?? '—'}
      </div>
      {sub && <div className="metric-sub">{sub}</div>}
      {fillMax !== undefined && (
        <div className="metric-bar-track">
          <div className="metric-bar-fill" style={{ width: `${pct}%` }} />
        </div>
      )}
    </div>
  )
}

/**
 * @param {{ metrics: object|null }} props
 */
export default function MetricsDashboard({ metrics }) {
  const memBytes   = metrics?.memtable_size_bytes ?? null
  const walBytes   = metrics?.wal_size_bytes       ?? null
  const ssts       = metrics?.sstable_count        ?? null

  return (
    <div className="metrics-grid">
      <MetricCard
        icon="🧠"
        label="MemTable Size"
        value={formatBytes(memBytes)}
        sub={memBytes !== null ? `${((memBytes / MEMTABLE_THRESHOLD_BYTES) * 100).toFixed(0)}% of flush threshold` : 'Waiting for backend…'}
        accent1="#6c63ff"
        accent2="#00d4ff"
        fill={memBytes}
        fillMax={MEMTABLE_THRESHOLD_BYTES}
        delay="0ms"
      />
      <MetricCard
        icon="📝"
        label="WAL Size"
        value={formatBytes(walBytes)}
        sub="Write-Ahead Log (crash recovery)"
        accent1="#00d4ff"
        accent2="#51cf66"
        fill={walBytes}
        fillMax={MEMTABLE_THRESHOLD_BYTES * 2}
        delay="60ms"
      />
      <MetricCard
        icon="💾"
        label="SSTable Files"
        value={ssts !== null ? ssts.toString() : '—'}
        sub={ssts === 0 ? 'No flushes yet' : ssts === 1 ? '1 immutable level-0 file' : `${ssts} immutable level-0 files`}
        accent1="#fcc419"
        accent2="#ff6b6b"
        delay="120ms"
      />
    </div>
  )
}
