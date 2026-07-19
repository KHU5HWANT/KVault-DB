// ============================================================================
// SkipListVisualizer.jsx — SVG Skip List Visualization
// ============================================================================
// Renders the current MemTable contents as a probabilistic skip list diagram.
//
// Since the backend exposes sorted level-0 entries (the real skip list data),
// we simulate the multi-level structure client-side using the same p=0.5
// coin-flip logic as the C++ engine, seeded deterministically from each key.
//
// Layout:
//   - X-axis  → nodes sorted by key (level-0)
//   - Y-axis  → skip list levels (level 0 at bottom, higher = fewer nodes)
//   - Lines   → forward pointers connecting nodes at each level
// ============================================================================

import { useMemo, useState, useRef } from 'react'
import './SkipListVisualizer.css'

// Visual constants
const NODE_R     = 20     // node circle radius
const H_GAP      = 64     // horizontal gap between node centres
const V_GAP      = 52     // vertical gap between levels
const PAD_X      = 48     // left/right padding
const PAD_Y      = 40     // top/bottom padding
const MAX_LEVELS = 5      // max levels to display
const MAX_NODES  = 32     // beyond this, collapse to table view

// Colors
const COLOR_PUT       = '#6c63ff'
const COLOR_TOMBSTONE = '#ff6b6b'
const COLOR_SENTINEL  = '#343a40'
const COLOR_LANE      = 'rgba(255,255,255,0.06)'
const COLOR_POINTER   = 'rgba(255,255,255,0.18)'

// Deterministic pseudo-random level assignment based on key string.
// Mirrors the skip list's geometric distribution (p=0.5) from the C++ engine.
function assignLevel(key, maxLevels) {
  let hash = 2166136261
  for (let i = 0; i < key.length; i++) {
    hash ^= key.charCodeAt(i)
    hash = (hash * 16777619) >>> 0
  }
  let level = 1
  while (level < maxLevels && (hash & (1 << level)) !== 0) level++
  return level
}

function truncate(str, max = 8) {
  return str.length > max ? str.slice(0, max - 1) + '…' : str
}

// Tooltip component that follows the hovered node
function Tooltip({ x, y, node }) {
  if (!node) return null
  return (
    <div
      className="node-tooltip"
      style={{ left: x + 12, top: y - 8 }}
    >
      <strong style={{ color: 'var(--color-accent-2)' }}>{node.key}</strong>
      {node.type === 'tombstone'
        ? <div style={{ color: 'var(--color-accent-3)' }}>🪦 tombstone</div>
        : <div>→ &quot;{node.value}&quot;</div>
      }
    </div>
  )
}

// Compact table fallback when there are too many entries to visualize
function TableFallback({ entries }) {
  return (
    <div style={{
      display: 'grid',
      gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))',
      gap: '8px',
      padding: '16px',
      maxHeight: '280px',
      overflowY: 'auto',
    }}>
      {entries.map(e => (
        <div key={e.key} style={{
          display: 'flex', gap: '8px', alignItems: 'center',
          padding: '6px 10px', borderRadius: '6px',
          background: 'rgba(255,255,255,0.04)',
          border: `1px solid ${e.type === 'tombstone' ? 'rgba(255,107,107,0.2)' : 'rgba(108,99,255,0.2)'}`,
          fontSize: '12px', fontFamily: 'var(--font-mono)',
        }}>
          <span style={{ color: e.type === 'tombstone' ? COLOR_TOMBSTONE : COLOR_PUT, minWidth: '12px' }}>
            {e.type === 'tombstone' ? '🪦' : '●'}
          </span>
          <span style={{ color: 'var(--color-accent-2)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
            {e.key}
          </span>
          {e.type !== 'tombstone' && (
            <span style={{ color: 'var(--color-text-secondary)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
              ={truncate(e.value, 14)}
            </span>
          )}
        </div>
      ))}
    </div>
  )
}

export default function SkipListVisualizer({ entries }) {
  const [tooltip, setTooltip]     = useState(null)
  const [tooltipPos, setTooltipPos] = useState({ x: 0, y: 0 })
  const containerRef = useRef(null)

  // Derive skip list layout from entries
  const { nodes, svgWidth, svgHeight, lanes } = useMemo(() => {
    if (!entries || entries.length === 0) {
      return { nodes: [], svgWidth: 200, svgHeight: 100, lanes: [] }
    }

    // Assign levels deterministically
    const withLevels = entries.map(e => ({
      ...e,
      level: assignLevel(e.key, MAX_LEVELS),
    }))

    const maxLevel = Math.max(...withLevels.map(e => e.level), 1)

    // Build sentinel nodes (−∞ and +∞)
    const allNodes = [
      { key: '−∞', value: '', type: 'sentinel', level: maxLevel, isSentinel: true },
      ...withLevels,
      { key: '+∞', value: '', type: 'sentinel', level: maxLevel, isSentinel: true },
    ]

    const totalNodes = allNodes.length
    const w = PAD_X * 2 + (totalNodes - 1) * H_GAP
    const h = PAD_Y * 2 + maxLevel * V_GAP

    // x, y positions for level-0 (bottom row)
    const positioned = allNodes.map((n, i) => ({
      ...n,
      x: PAD_X + i * H_GAP,
      y0: h - PAD_Y, // level-0 y (bottom)
      yAtLevel: (lvl) => h - PAD_Y - (lvl - 1) * V_GAP,
    }))

    // Lane backgrounds (one per level, drawn as horizontal bands)
    const laneArr = Array.from({ length: maxLevel }, (_, i) => ({
      level: i + 1,
      y: h - PAD_Y - i * V_GAP,
      label: `L${i}`,
    }))

    return { nodes: positioned, svgWidth: w, svgHeight: h, lanes: laneArr }
  }, [entries])

  const handleMouseMove = (e, node) => {
    if (!containerRef.current) return
    const rect = containerRef.current.getBoundingClientRect()
    setTooltipPos({ x: e.clientX - rect.left, y: e.clientY - rect.top })
    setTooltip(node)
  }

  if (!entries || entries.length === 0) {
    return (
      <div className="glass-card visualizer-wrapper fade-up-delay-3">
        <div className="visualizer-title">🌐 MemTable Skip List</div>
        <div className="visualizer-empty">
          <div style={{ fontSize: '36px', marginBottom: '12px' }}>∅</div>
          <div>MemTable is empty.</div>
          <div style={{ marginTop: '6px' }}>Insert some keys to see the visualization.</div>
        </div>
      </div>
    )
  }

  // Table fallback for very large memtables
  if (entries.length > MAX_NODES) {
    return (
      <div className="glass-card visualizer-wrapper fade-up-delay-3">
        <div className="visualizer-title">
          🌐 MemTable Contents
          <span className="visualizer-count">({entries.length} entries — compact view)</span>
        </div>
        <TableFallback entries={entries} />
        <div className="visualizer-legend">
          <div className="legend-item"><div className="legend-dot" style={{ background: COLOR_PUT }} />Live entry</div>
          <div className="legend-item"><div className="legend-dot" style={{ background: COLOR_TOMBSTONE }} />Tombstone</div>
        </div>
      </div>
    )
  }

  const maxLevel = Math.max(...nodes.filter(n => !n.isSentinel).map(n => n.level), 1)

  return (
    <div
      className="glass-card visualizer-wrapper fade-up-delay-3"
      ref={containerRef}
      style={{ position: 'relative' }}
      onMouseLeave={() => setTooltip(null)}
    >
      <div className="visualizer-title">
        🌐 MemTable Skip List
        <span className="visualizer-count">({entries.length} entr{entries.length === 1 ? 'y' : 'ies'}, {maxLevel} level{maxLevel > 1 ? 's' : ''})</span>
      </div>

      <Tooltip x={tooltipPos.x} y={tooltipPos.y} node={tooltip} />

      <div className="visualizer-svg-container">
        <svg
          width={svgWidth}
          height={svgHeight}
          style={{ display: 'block', minWidth: '100%' }}
          aria-label="Skip List visualization of current MemTable contents"
        >
          {/* --- Level lane backgrounds --- */}
          {lanes.map(lane => (
            <g key={lane.level}>
              <line
                x1={0} y1={lane.y}
                x2={svgWidth} y2={lane.y}
                stroke={COLOR_LANE} strokeWidth={1} strokeDasharray="4 4"
              />
              <text x={6} y={lane.y - 6} fontSize={9} fill="rgba(255,255,255,0.2)" fontFamily="var(--font-mono)">
                {lane.label}
              </text>
            </g>
          ))}

          {/* --- Vertical connectors (level-0 links between all nodes) --- */}
          {nodes.slice(0, -1).map((node, i) => {
            const next = nodes[i + 1]
            return (
              <line
                key={`h0-${i}`}
                x1={node.x} y1={node.y0}
                x2={next.x} y2={next.y0}
                stroke={COLOR_POINTER} strokeWidth={1.5}
              />
            )
          })}

          {/* --- Express lane forward pointers (levels > 1) --- */}
          {Array.from({ length: maxLevel - 1 }, (_, lvlIdx) => {
            const lvl = lvlIdx + 2 // level 2, 3, 4…
            const levelNodes = nodes.filter(n => n.level >= lvl)
            return levelNodes.slice(0, -1).map((node, i) => {
              const next = levelNodes[i + 1]
              const y = node.yAtLevel(lvl)
              const ny = next.yAtLevel(lvl)
              return (
                <g key={`express-${lvl}-${i}`}>
                  {/* Arrow line */}
                  <line
                    x1={node.x} y1={y}
                    x2={next.x} y2={ny}
                    stroke={`rgba(108,99,255,0.35)`}
                    strokeWidth={1.5}
                    markerEnd="url(#arrow)"
                  />
                </g>
              )
            })
          })}

          {/* --- Arrow marker def --- */}
          <defs>
            <marker id="arrow" markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto">
              <path d="M0,0 L0,6 L6,3 z" fill="rgba(108,99,255,0.5)" />
            </marker>
          </defs>

          {/* --- Vertical stacks (the node column at each x position) --- */}
          {nodes.map((node) => {
            const lvls = Array.from({ length: node.level }, (_, i) => i + 1)
            return lvls.map(lvl => {
              const cy = node.yAtLevel(lvl)
              const isTombstone = node.type === 'tombstone'
              const isSentinel  = node.isSentinel
              const fill = isSentinel ? COLOR_SENTINEL : isTombstone ? COLOR_TOMBSTONE : COLOR_PUT

              return (
                <g key={`node-${node.key}-lvl${lvl}`}>
                  {/* Vertical connector from this node to level below */}
                  {lvl > 1 && (
                    <line
                      x1={node.x} y1={cy}
                      x2={node.x} y2={node.yAtLevel(lvl - 1)}
                      stroke="rgba(255,255,255,0.1)"
                      strokeWidth={1}
                      strokeDasharray="3 3"
                    />
                  )}
                  {/* Node circle */}
                  <circle
                    cx={node.x}
                    cy={cy}
                    r={NODE_R}
                    fill={`${fill}22`}
                    stroke={fill}
                    strokeWidth={isSentinel ? 1 : 1.5}
                    style={{ cursor: isSentinel ? 'default' : 'pointer', transition: 'all 0.2s ease' }}
                    onMouseMove={isSentinel ? undefined : (e) => handleMouseMove(e, node)}
                    onMouseLeave={() => setTooltip(null)}
                  />
                  {/* Node label (only on level-0 for data nodes) */}
                  {lvl === 1 && (
                    <text
                      x={node.x}
                      y={cy + 4}
                      textAnchor="middle"
                      fontSize={isSentinel ? 10 : 9}
                      fontFamily="var(--font-mono)"
                      fill={isSentinel ? 'rgba(255,255,255,0.3)' : isTombstone ? COLOR_TOMBSTONE : 'rgba(255,255,255,0.85)'}
                      style={{ userSelect: 'none', pointerEvents: 'none' }}
                    >
                      {isSentinel ? node.key : truncate(node.key, 6)}
                    </text>
                  )}
                </g>
              )
            })
          })}
        </svg>
      </div>

      {/* Legend */}
      <div className="visualizer-legend">
        <div className="legend-item">
          <div className="legend-dot" style={{ background: COLOR_PUT }} />
          Live entry
        </div>
        <div className="legend-item">
          <div className="legend-dot" style={{ background: COLOR_TOMBSTONE }} />
          Tombstone
        </div>
        <div className="legend-item">
          <div className="legend-dot" style={{ background: COLOR_SENTINEL }} />
          Sentinel (−∞ / +∞)
        </div>
        <div className="legend-item" style={{ color: 'rgba(108,99,255,0.7)' }}>
          ── Express lanes (higher levels)
        </div>
      </div>
    </div>
  )
}
