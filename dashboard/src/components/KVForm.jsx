// ============================================================================
// KVForm.jsx — Key-Value Operation Panel
// ============================================================================
// Provides PUT, GET, DELETE inputs with result display and operation history.
// Uses the useKVStore hook for all backend interactions.
// ============================================================================

import { useState, useCallback } from 'react'
import { useKVStore } from '../hooks/useKVStore'
import './KVForm.css'

const STATUS_ICON = {
  ok:        '✓',
  error:     '✗',
  not_found: '?',
}

function ResultPanel({ result }) {
  if (!result) return null

  const { op, key, value, status, timestamp } = result
  return (
    <div className={`kvform-result status-${status}`}>
      <div>
        <span className="result-op-badge">{op}</span>
        <span className="result-key">{key}</span>
        <span style={{ color: 'var(--color-text-muted)', marginLeft: '6px', fontSize: '11px' }}>
          {STATUS_ICON[status]}
        </span>
      </div>
      {op === 'GET' && status === 'ok' && (
        <div className="result-value">→ &quot;{value}&quot;</div>
      )}
      {status === 'not_found' && (
        <div className="result-value" style={{ color: 'var(--color-accent-warn)' }}>Key does not exist</div>
      )}
      {status === 'error' && (
        <div className="result-value" style={{ color: 'var(--color-accent-3)' }}>{value}</div>
      )}
      <div className="result-meta">{timestamp}</div>
    </div>
  )
}

function HistoryPanel({ history }) {
  if (history.length === 0) return (
    <p style={{ fontSize: '12px', color: 'var(--color-text-muted)', textAlign: 'center', padding: 'var(--space-4) 0' }}>
      No operations yet — try a PUT!
    </p>
  )

  return (
    <div className="history-list">
      {history.map((item, idx) => (
        <div key={idx} className="history-item">
          <span className={`history-op ${item.op}`}>{item.op}</span>
          <span className="history-key">{item.key}</span>
          <span className={`history-status-${item.status}`}>{STATUS_ICON[item.status]}</span>
          <span className="history-time">{item.timestamp}</span>
        </div>
      ))}
    </div>
  )
}

export default function KVForm({ onOperationComplete }) {
  const [key, setKey]     = useState('')
  const [value, setValue] = useState('')
  const { execute, lastResult, isLoading, error, history } = useKVStore()

  const handleOp = useCallback(async (op) => {
    await execute(op, key, value)
    // Notify parent to refresh the MemTable snapshot
    if (onOperationComplete) onOperationComplete()
  }, [execute, key, value, onOperationComplete])

  const handleKeyDown = useCallback((e) => {
    if (e.key === 'Enter' && !isLoading) handleOp('PUT')
  }, [handleOp, isLoading])

  return (
    <div className="glass-card kvform-wrapper fade-up-delay-1">
      <div className="kvform-title">⚡ Operations</div>

      <div className="kvform-inputs">
        <div className="input-group">
          <label className="input-label" htmlFor="kv-key">Key</label>
          <input
            id="kv-key"
            className="input-field"
            type="text"
            placeholder="e.g. user:1001"
            value={key}
            onChange={e => setKey(e.target.value)}
            onKeyDown={handleKeyDown}
            autoComplete="off"
            spellCheck={false}
          />
        </div>
        <div className="input-group">
          <label className="input-label" htmlFor="kv-value">Value <span style={{ fontWeight: 400, textTransform: 'none', letterSpacing: 0 }}>(for PUT)</span></label>
          <input
            id="kv-value"
            className="input-field"
            type="text"
            placeholder={'e.g. {"name": "Alice"}'}
            value={value}
            onChange={e => setValue(e.target.value)}
            onKeyDown={handleKeyDown}
            autoComplete="off"
            spellCheck={false}
          />
        </div>
      </div>

      <div className="kvform-actions">
        <button
          id="btn-put"
          className="btn btn-success"
          onClick={() => handleOp('PUT')}
          disabled={isLoading || !key.trim()}
          title="Insert / Update (PUT)"
        >
          {isLoading ? '…' : '📥 PUT'}
        </button>
        <button
          id="btn-get"
          className="btn btn-secondary"
          onClick={() => handleOp('GET')}
          disabled={isLoading || !key.trim()}
          title="Read key (GET)"
        >
          {isLoading ? '…' : '🔍 GET'}
        </button>
        <button
          id="btn-delete"
          className="btn btn-danger"
          onClick={() => handleOp('DELETE')}
          disabled={isLoading || !key.trim()}
          title="Delete key (tombstone)"
        >
          {isLoading ? '…' : '🗑 DEL'}
        </button>
      </div>

      {error && (
        <div className="kvform-result status-error">
          <span style={{ color: 'var(--color-accent-3)' }}>⚠ {error}</span>
        </div>
      )}

      <ResultPanel result={lastResult} />

      <div>
        <div className="kvform-title" style={{ marginBottom: 'var(--space-3)' }}>📋 History</div>
        <HistoryPanel history={history} />
      </div>
    </div>
  )
}
