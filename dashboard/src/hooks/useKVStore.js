// ============================================================================
// useKVStore.js — Custom Hook for KVault REST API Interactions
// ============================================================================
// Abstracts all fetch() calls to the C++ backend:
//   PUT  → POST  /api/kv
//   GET  → GET   /api/kv/<key>
//   DEL  → DELETE /api/kv/<key>
//
// Returns: { execute, lastResult, isLoading, error, history }
// ============================================================================

import { useState, useCallback } from 'react'

const API_BASE = '/api'

/**
 * @returns {{
 *   execute: (op: 'PUT'|'GET'|'DELETE', key: string, value?: string) => Promise<void>,
 *   lastResult: { op: string, key: string, value: string, status: 'ok'|'error'|'not_found' } | null,
 *   isLoading: boolean,
 *   error: string | null,
 *   history: Array,
 * }}
 */
export function useKVStore() {
  const [lastResult, setLastResult] = useState(null)
  const [isLoading, setIsLoading] = useState(false)
  const [error, setError]         = useState(null)
  const [history, setHistory]     = useState([])

  const execute = useCallback(async (op, key, value = '') => {
    if (!key.trim()) {
      setError('Key cannot be empty.')
      return
    }

    setIsLoading(true)
    setError(null)

    const timestamp = new Date().toLocaleTimeString()

    try {
      let response
      if (op === 'PUT') {
        response = await fetch(`${API_BASE}/kv`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ key: key.trim(), value }),
        })
      } else if (op === 'GET') {
        response = await fetch(`${API_BASE}/kv/${encodeURIComponent(key.trim())}`)
      } else if (op === 'DELETE') {
        response = await fetch(`${API_BASE}/kv/${encodeURIComponent(key.trim())}`, {
          method: 'DELETE',
        })
      } else {
        throw new Error(`Unknown operation: ${op}`)
      }

      let resultValue = ''
      let status = 'ok'

      if (op === 'GET') {
        if (response.status === 404) {
          status = 'not_found'
          resultValue = '(key not found)'
        } else if (response.ok) {
          resultValue = await response.text()
        } else {
          status = 'error'
          resultValue = await response.text()
        }
      } else {
        if (!response.ok) {
          status = 'error'
          resultValue = await response.text()
        }
      }

      const entry = { op, key: key.trim(), value: resultValue, status, timestamp }
      setLastResult(entry)
      setHistory(prev => [entry, ...prev].slice(0, 50)) // keep last 50 ops

    } catch (err) {
      const msg = err.message.includes('fetch')
        ? 'Backend offline. Start kvault_server.exe and retry.'
        : err.message
      setError(msg)
      const entry = { op, key: key.trim(), value: '', status: 'error', timestamp }
      setLastResult(entry)
      setHistory(prev => [entry, ...prev].slice(0, 50))
    } finally {
      setIsLoading(false)
    }
  }, [])

  return { execute, lastResult, isLoading, error, history }
}
