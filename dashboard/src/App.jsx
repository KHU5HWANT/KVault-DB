// ============================================================================
// App.jsx — KVault Dashboard Root
// ============================================================================
// Unified single-screen dashboard:
//
//   [ Header: Logo | Status | Last Updated ]
//   [ Offline Banner (conditional) ]
//   [ Metrics Row: MemTable | WAL | SSTables ]
//   [ Bottom Grid: KVForm | SkipList Visualizer ]
// ============================================================================

import { useCallback } from 'react'
import { useMetrics } from './hooks/useMetrics'
import MetricsDashboard from './components/MetricsDashboard'
import KVForm from './components/KVForm'
import SkipListVisualizer from './components/SkipListVisualizer'
import './App.css'

function StatusBadge({ status }) {
  const classes = {
    online:     'badge badge-online',
    offline:    'badge badge-offline',
    connecting: 'badge badge-loading',
  }
  const labels = {
    online:     'Engine Online',
    offline:    'Engine Offline',
    connecting: 'Connecting…',
  }
  return (
    <span className={classes[status]}>
      {status === 'online' && <span className="live-dot" />}
      {labels[status]}
    </span>
  )
}

export default function App() {
  const { metrics, snapshot, backendStatus, lastUpdated, refetch } = useMetrics()

  // After every KV operation, immediately re-fetch metrics/snapshot
  // so the visualizer updates without waiting for the next poll tick.
  const handleOperationComplete = useCallback(() => {
    refetch()
  }, [refetch])

  const isOffline = backendStatus === 'offline'

  return (
    <>
      {/* ---- Sticky Header ------------------------------------------------ */}
      <header className="app-header">
        <div className="app-logo">
          <div className="logo-icon">🗄️</div>
          <div className="logo-text">
            <span className="logo-title">KVault</span>
            <span className="logo-sub">LSM-Tree Engine Dashboard</span>
          </div>
        </div>

        <div className="header-right">
          {lastUpdated && (
            <span className="last-updated">
              Updated {lastUpdated.toLocaleTimeString()}
            </span>
          )}
          <StatusBadge status={backendStatus} />
        </div>
      </header>

      {/* ---- Main Content ------------------------------------------------- */}
      <main className="app-main">

        {/* Offline warning */}
        {isOffline && (
          <div className="offline-banner">
            <span style={{ fontSize: '20px' }}>⚠️</span>
            <div>
              <strong>Backend not reachable.</strong>
              {' '}Start the engine:{' '}
              <code>.\build\kvault_server.exe</code>
              {' '}then wait for the dashboard to reconnect automatically.
            </div>
          </div>
        )}

        {/* Live Metrics */}
        <section aria-label="Engine metrics">
          <div className="section-heading">📊 Engine Metrics</div>
          <MetricsDashboard metrics={metrics} />
        </section>

        {/* Operations + Visualizer */}
        <section aria-label="Key-value operations and visualizer">
          <div className="section-heading">🔧 Interact & Visualize</div>
          <div className="app-bottom-grid">
            <KVForm onOperationComplete={handleOperationComplete} />
            <SkipListVisualizer entries={snapshot} />
          </div>
        </section>

      </main>
    </>
  )
}
