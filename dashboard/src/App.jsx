import { useState, useEffect, useRef, useCallback } from "react";

// ── Leaflet + OpenStreetMap (100% FREE, no API key) ──────────────
// npm install leaflet react-leaflet
import L from "leaflet";
import { MapContainer, TileLayer, Polyline, Marker, Popup } from "react-leaflet";
import "leaflet/dist/leaflet.css";

// Fix Leaflet default marker icons
delete L.Icon.Default.prototype._getIconUrl;
L.Icon.Default.mergeOptions({
  iconRetinaUrl: "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/images/marker-icon-2x.png",
  iconUrl: "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/images/marker-icon.png",
  shadowUrl: "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/images/marker-shadow.png",
});

// ── Config ──────────────────────────────────────────────────────
const BACKEND = import.meta.env.VITE_BACKEND_URL || "https://your-app.onrender.com";

// ══════════════════════════════════════════════════════════════════
//  UTILITIES
// ══════════════════════════════════════════════════════════════════
function fmt(isoStr) {
  if (!isoStr) return "—";
  return new Date(isoStr).toLocaleString();
}

function threatColor(level) {
  const map = {
    LOW: { bg: "#dcfce7", text: "#15803d", dot: "#22c55e" },
    MEDIUM: { bg: "#fef9c3", text: "#a16207", dot: "#eab308" },
    HIGH: { bg: "#fee2e2", text: "#b91c1c", dot: "#ef4444" },
    CRITICAL: { bg: "#fce7f3", text: "#9d174d", dot: "#ec4899" },
  };
  return map[level] || map.LOW;
}

// ══════════════════════════════════════════════════════════════════
//  API HELPER
// ══════════════════════════════════════════════════════════════════
async function api(path, opts = {}, token = null) {
  const headers = { "Content-Type": "application/json", ...(opts.headers || {}) };
  if (token) headers["Authorization"] = `Bearer ${token}`;
  const res = await fetch(`${BACKEND}${path}`, { ...opts, headers });
  if (!res.ok) throw new Error(`API ${path} → ${res.status}`);
  return res.json();
}

// ══════════════════════════════════════════════════════════════════
//  QR AUTH SCREEN
// ══════════════════════════════════════════════════════════════════
function QRAuthScreen({ onAuth }) {
  const [qrLoaded, setQrLoaded] = useState(false);
  const [code, setCode] = useState("");
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(false);
  const inputRef = useRef(null);

  useEffect(() => {
    if (inputRef.current) inputRef.current.focus();
  }, []);

  async function handleVerify() {
    if (code.length !== 6) { setError("Enter 6-digit code"); return; }
    setLoading(true);
    setError("");
    try {
      const res = await api("/api/auth/verify", {
        method: "POST",
        body: JSON.stringify({ code }),
      });
      onAuth(res.token);
    } catch {
      setError("Invalid code. Try again.");
      setCode("");
    } finally {
      setLoading(false);
    }
  }

  return (
    <div style={{
      minHeight: "100vh",
      background: "#f8fafc",
      display: "flex",
      alignItems: "center",
      justifyContent: "center",
      fontFamily: "'DM Mono', 'Courier New', monospace",
    }}>
      <div style={{
        background: "#fff",
        border: "1.5px solid #e2e8f0",
        borderRadius: 16,
        padding: "48px 40px",
        width: 380,
        boxShadow: "0 4px 24px rgba(0,0,0,0.07)",
        textAlign: "center",
      }}>
        {/* Logo */}
        <div style={{
          width: 56, height: 56, borderRadius: 14,
          background: "#ef4444", margin: "0 auto 20px",
          display: "flex", alignItems: "center", justifyContent: "center",
        }}>
          <span style={{ color: "#fff", fontSize: 26, fontWeight: 900 }}>S</span>
        </div>

        <div style={{ fontSize: 11, letterSpacing: 3, color: "#94a3b8", marginBottom: 4, fontWeight: 700 }}>
          SAAK-GUARD
        </div>
        <h1 style={{ fontSize: 22, fontWeight: 700, color: "#0f172a", margin: "0 0 4px" }}>
          Officer Dashboard
        </h1>
        <p style={{ color: "#64748b", fontSize: 13, marginBottom: 32 }}>
          Forensic Evidence Platform
        </p>

        {/* QR Code */}
        <div style={{
          background: "#f1f5f9", borderRadius: 12, padding: 16,
          marginBottom: 24, display: "inline-block",
        }}>
          <img
            src={`${BACKEND}/api/auth/qr`}
            alt="TOTP QR Code"
            width={160} height={160}
            style={{ display: "block", borderRadius: 8 }}
            onLoad={() => setQrLoaded(true)}
          />
        </div>

        <p style={{ fontSize: 12, color: "#64748b", marginBottom: 20, lineHeight: 1.5 }}>
          Scan with Google Authenticator or Authy.<br />
          Enter the 6-digit code below to unlock.
        </p>

        {/* Code input */}
        <div style={{ display: "flex", gap: 8, marginBottom: 12 }}>
          <input
            ref={inputRef}
            type="text"
            inputMode="numeric"
            maxLength={6}
            value={code}
            onChange={e => setCode(e.target.value.replace(/\D/g, "").slice(0, 6))}
            onKeyDown={e => e.key === "Enter" && handleVerify()}
            placeholder="000 000"
            style={{
              flex: 1,
              padding: "12px 16px",
              border: `1.5px solid ${error ? "#ef4444" : "#e2e8f0"}`,
              borderRadius: 10,
              fontSize: 20,
              letterSpacing: 6,
              textAlign: "center",
              fontFamily: "inherit",
              outline: "none",
              color: "#0f172a",
            }}
          />
          <button
            onClick={handleVerify}
            disabled={loading}
            style={{
              padding: "12px 20px",
              background: loading ? "#94a3b8" : "#ef4444",
              color: "#fff",
              border: "none",
              borderRadius: 10,
              fontSize: 14,
              fontWeight: 700,
              cursor: loading ? "not-allowed" : "pointer",
              fontFamily: "inherit",
              transition: "background 0.2s",
            }}
          >
            {loading ? "..." : "VERIFY"}
          </button>
        </div>

        {error && (
          <p style={{ color: "#ef4444", fontSize: 13, margin: 0 }}>{error}</p>
        )}

        <p style={{ fontSize: 11, color: "#cbd5e1", marginTop: 24 }}>
          Code refreshes every 30 seconds
        </p>
      </div>
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  SOS ALERT BANNER
// ══════════════════════════════════════════════════════════════════
function SOSBanner({ activeEvent, onView }) {
  const [flash, setFlash] = useState(true);

  useEffect(() => {
    if (!activeEvent) return;
    const t = setInterval(() => setFlash(f => !f), 500);
    try {
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain); gain.connect(ctx.destination);
      osc.type = "square";
      osc.frequency.setValueAtTime(880, ctx.currentTime);
      gain.gain.setValueAtTime(0.1, ctx.currentTime);
      osc.start(); osc.stop(ctx.currentTime + 1.5);
    } catch {}
    return () => clearInterval(t);
  }, [activeEvent]);

  if (!activeEvent) return null;

  return (
    <div style={{
      position: "fixed", top: 0, left: 0, right: 0, zIndex: 9999,
      background: flash ? "#ef4444" : "#b91c1c",
      color: "#fff",
      padding: "14px 24px",
      display: "flex", alignItems: "center", gap: 16,
      transition: "background 0.1s",
      boxShadow: "0 4px 20px rgba(239,68,68,0.5)",
    }}>
      <span style={{ fontSize: 22 }}>🚨</span>
      <div style={{ flex: 1 }}>
        <div style={{ fontWeight: 800, fontSize: 15, letterSpacing: 1 }}>
          SOS ACTIVE — {activeEvent.device_id}
        </div>
        <div style={{ fontSize: 12, opacity: 0.9 }}>
          Trigger: {activeEvent.trigger} &nbsp;|&nbsp;
          GPS: {activeEvent.lat?.toFixed(6)}, {activeEvent.lon?.toFixed(6)} &nbsp;|&nbsp;
          {fmt(activeEvent.timestamp)}
        </div>
      </div>
      <button
        onClick={onView}
        style={{
          background: "#fff", color: "#ef4444",
          border: "none", borderRadius: 8,
          padding: "8px 16px", fontWeight: 700, fontSize: 13,
          cursor: "pointer", fontFamily: "inherit",
        }}
      >
        OPEN LIVE FEED →
      </button>
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  LIVE MAP — Leaflet/OpenStreetMap (FREE)
// ══════════════════════════════════════════════════════════════════
function LiveMap({ breadcrumbs, activeEvent }) {
  const mapRef = useRef(null);
  const markerRef = useRef(null);

  useEffect(() => {
    if (!breadcrumbs?.length || !mapRef.current) return;

    const coords = breadcrumbs.map(b => [b.lat, b.lon]);
    const last = coords[coords.length - 1];

    // Remove old marker if exists
    if (markerRef.current) {
      mapRef.current.removeLayer(markerRef.current);
    }

    // Draw polyline (breadcrumb trail)
    L.polyline(coords, {
      color: "#ef4444",
      weight: 3,
      opacity: 0.8,
    }).addTo(mapRef.current);

    // Add pulsing marker at current location
    markerRef.current = L.circleMarker(last, {
      radius: 8,
      fillColor: "#ef4444",
      color: "#fff",
      weight: 3,
      opacity: 0.9,
      fillOpacity: 0.8,
    }).addTo(mapRef.current);

    // Center map on live location
    mapRef.current.setView(last, 15);
  }, [breadcrumbs]);

  return (
    <MapContainer
      center={[13.0827, 80.2707]}  // Default: Chennai
      zoom={12}
      style={{ width: "100%", height: "100%", borderRadius: 12 }}
      ref={mapRef}
    >
      <TileLayer
        url="https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png"
        attribution='&copy; OpenStreetMap contributors'
      />
      
      {breadcrumbs?.length > 0 && (
        <>
          <Polyline
            positions={breadcrumbs.map(b => [b.lat, b.lon])}
            color="#ef4444"
            weight={3}
            opacity={0.8}
          />
          <Marker
            position={[breadcrumbs[breadcrumbs.length - 1].lat, 
                      breadcrumbs[breadcrumbs.length - 1].lon]}
          >
            <Popup>Current Location</Popup>
          </Marker>
        </>
      )}
    </MapContainer>
  );
}

// ══════════════════════════════════════════════════════════════════
//  SESSION DETAIL PANEL
// ══════════════════════════════════════════════════════════════════
function SessionPanel({ session, token, onClose }) {
  const [frames, setFrames]             = useState([]);
  const [breadcrumbs, setBreadcrumbs]   = useState([]);
  const [aiData, setAiData]             = useState(null);
  const [currentFrame, setCurrentFrame] = useState(0);
  const [tab, setTab]                   = useState("live");
  const wsRef = useRef(null);

  useEffect(() => {
    if (!session) return;

    api(`/api/sessions/${session.id}/breadcrumbs`, {}, token).then(setBreadcrumbs).catch(() => {});
    api(`/api/sessions/${session.id}/frames`, {}, token).then(setFrames).catch(() => {});

    if (session.ai_threat_level) {
      setAiData({
        threat_level: session.ai_threat_level,
        summary: session.ai_summary,
        tone_analysis: session.ai_transcript,
      });
    }

    const wsUrl = `${BACKEND.replace("https://", "wss://").replace("http://", "ws://")}/ws/${session.id}`;
    wsRef.current = new WebSocket(wsUrl);
    wsRef.current.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.type === "new_frame") {
        setFrames(prev => [...prev, { frame_num: msg.frame_num, file_path: msg.frame_url }]);
        setCurrentFrame(msg.frame_num - 1);
        if (msg.lat && msg.lon) {
          setBreadcrumbs(prev => [...prev, { lat: msg.lat, lon: msg.lon }]);
        }
      }
      if (msg.type === "ai_analysis") setAiData(msg.analysis);
    };

    return () => wsRef.current?.close();
  }, [session, token]);

  if (!session) return null;
  const tc = threatColor(session.ai_threat_level || "LOW");

  const tabs = ["live", "map", "chain"];

  return (
    <div style={{
      position: "fixed", inset: 0, background: "rgba(0,0,0,0.5)",
      zIndex: 1000, display: "flex", alignItems: "center", justifyContent: "center",
      padding: 24,
    }}>
      <div style={{
        background: "#fff", borderRadius: 16, width: "100%", maxWidth: 1100,
        maxHeight: "90vh", overflow: "hidden", display: "flex", flexDirection: "column",
        boxShadow: "0 24px 80px rgba(0,0,0,0.2)",
      }}>
        {/* Header */}
        <div style={{
          padding: "20px 24px", borderBottom: "1px solid #f1f5f9",
          display: "flex", alignItems: "center", gap: 12,
        }}>
          <div style={{
            background: "#fee2e2", color: "#ef4444",
            padding: "4px 10px", borderRadius: 6,
            fontSize: 11, fontWeight: 800, letterSpacing: 1,
          }}>
            {session.status?.toUpperCase()}
          </div>
          <div>
            <div style={{ fontWeight: 700, fontSize: 16, color: "#0f172a" }}>
              {session.id}
            </div>
            <div style={{ fontSize: 12, color: "#64748b" }}>
              {session.device_id} · {fmt(session.started_at)}
            </div>
          </div>
          {session.ai_threat_level && (
            <div style={{
              marginLeft: "auto",
              background: tc.bg, color: tc.text,
              padding: "6px 14px", borderRadius: 20,
              fontSize: 12, fontWeight: 800, letterSpacing: 0.5,
              display: "flex", alignItems: "center", gap: 6,
            }}>
              <span style={{ width: 8, height: 8, borderRadius: "50%", background: tc.dot, display: "inline-block" }} />
              {session.ai_threat_level}
            </div>
          )}
          <button
            onClick={onClose}
            style={{
              marginLeft: session.ai_threat_level ? 0 : "auto",
              background: "#f1f5f9", border: "none", borderRadius: 8,
              width: 36, height: 36, fontSize: 18, cursor: "pointer",
              display: "flex", alignItems: "center", justifyContent: "center",
            }}
          >×</button>
        </div>

        {/* Tabs */}
        <div style={{ display: "flex", borderBottom: "1px solid #f1f5f9", padding: "0 24px" }}>
          {tabs.map(t => (
            <button key={t} onClick={() => setTab(t)} style={{
              padding: "12px 20px", border: "none", background: "none",
              color: tab === t ? "#ef4444" : "#64748b",
              borderBottom: tab === t ? "2px solid #ef4444" : "2px solid transparent",
              fontWeight: tab === t ? 700 : 500,
              fontSize: 13, cursor: "pointer", textTransform: "uppercase",
              letterSpacing: 0.5, fontFamily: "inherit",
            }}>
              {t === "live" ? `📹 Live Feed (${frames.length})` :
               t === "map"  ? "🗺️ GPS Map" :
                              "🔐 Chain of Custody"}
            </button>
          ))}
        </div>

        {/* Tab Content */}
        <div style={{ flex: 1, overflow: "auto", padding: 24 }}>
          {/* LIVE FEED */}
          {tab === "live" && (
            <div style={{ display: "flex", gap: 16, height: "100%" }}>
              <div style={{ flex: 2, display: "flex", flexDirection: "column", gap: 12 }}>
                {frames.length > 0 ? (
                  <>
                    <img
                      src={`${BACKEND}/media/${session.id}/watermarked/frame_${String(frames[currentFrame]?.frame_num || 1).padStart(4, "0")}.jpg`}
                      alt="Live frame"
                      style={{ width: "100%", borderRadius: 10, border: "1px solid #e2e8f0" }}
                    />
                    <div style={{ fontSize: 12, color: "#64748b", textAlign: "center" }}>
                      Frame {frames[currentFrame]?.frame_num} / {frames.length}
                    </div>
                    <input
                      type="range" min={0} max={frames.length - 1}
                      value={currentFrame}
                      onChange={e => setCurrentFrame(+e.target.value)}
                      style={{ width: "100%", accentColor: "#ef4444" }}
                    />
                  </>
                ) : (
                  <div style={{
                    background: "#f8fafc", borderRadius: 10, border: "1.5px dashed #e2e8f0",
                    display: "flex", alignItems: "center", justifyContent: "center",
                    height: 300, flexDirection: "column", gap: 8, color: "#94a3b8",
                  }}>
                    <span style={{ fontSize: 36 }}>📷</span>
                    <div>Waiting for frames…</div>
                  </div>
                )}
              </div>
              {/* Frame list */}
              <div style={{
                flex: 1, overflowY: "auto", display: "flex", flexDirection: "column", gap: 6,
                maxHeight: 420,
              }}>
                {frames.slice(-20).reverse().map((f, i) => (
                  <button
                    key={f.frame_num}
                    onClick={() => setCurrentFrame(frames.indexOf(f))}
                    style={{
                      padding: "8px 12px", border: "1.5px solid #e2e8f0",
                      borderRadius: 8, background: "#f8fafc",
                      fontSize: 12, cursor: "pointer", textAlign: "left",
                      fontFamily: "inherit", color: "#0f172a",
                    }}
                  >
                    Frame {f.frame_num} · GPS {f.gps_lat?.toFixed(4)}, {f.gps_lon?.toFixed(4)}
                  </button>
                ))}
              </div>
            </div>
          )}

          {/* MAP */}
          {tab === "map" && (
            <div style={{ height: 480 }}>
              <LiveMap breadcrumbs={breadcrumbs} activeEvent={session} />
            </div>
          )}

          {/* CHAIN OF CUSTODY */}
          {tab === "chain" && (
            <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
              {[
                { label: "Session ID", value: session.id },
                { label: "Device ID", value: session.device_id },
                { label: "Trigger", value: session.trigger_type },
                { label: "Started", value: fmt(session.started_at) },
                { label: "Ended", value: fmt(session.ended_at) },
                { label: "Frames Captured", value: session.frame_count },
                { label: "File Hash (SHA-256)", value: session.file_hash || "Pending…" },
                { label: "ECDSA Signature", value: session.signature || "Pending…" },
                { label: "Status", value: session.status },
              ].map(({ label, value }) => (
                <div key={label} style={{
                  display: "flex", gap: 16, padding: "12px 16px",
                  background: "#f8fafc", borderRadius: 10, border: "1px solid #e2e8f0",
                }}>
                  <div style={{ minWidth: 160, fontWeight: 700, fontSize: 12, color: "#64748b" }}>
                    {label}
                  </div>
                  <div style={{
                    flex: 1, fontSize: 12, color: "#0f172a",
                    wordBreak: "break-all", fontFamily: "'DM Mono', monospace",
                  }}>
                    {String(value)}
                  </div>
                </div>
              ))}
              {session.video_path && (
                <a
                  href={`${BACKEND}/media/${session.id}/evidence.mp4`}
                  target="_blank"
                  rel="noreferrer"
                  style={{
                    display: "block", textAlign: "center", padding: "14px",
                    background: "#0f172a", color: "#fff", borderRadius: 10,
                    textDecoration: "none", fontWeight: 700, fontSize: 14,
                    marginTop: 8,
                  }}
                >
                  ⬇️ Download Evidence MP4
                </a>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  SESSIONS LIST
// ══════════════════════════════════════════════════════════════════
function SessionsList({ sessions, onSelect }) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
      {sessions.length === 0 && (
        <div style={{
          textAlign: "center", padding: 40, color: "#94a3b8", fontSize: 13,
        }}>
          No sessions recorded yet.
        </div>
      )}
      {sessions.map(s => {
        const tc = threatColor(s.ai_threat_level || "LOW");
        return (
          <button
            key={s.id}
            onClick={() => onSelect(s)}
            style={{
              padding: "14px 16px", background: "#fff",
              border: "1.5px solid #e2e8f0", borderRadius: 12,
              cursor: "pointer", textAlign: "left", fontFamily: "inherit",
              display: "flex", alignItems: "center", gap: 14,
              transition: "border-color 0.15s, box-shadow 0.15s",
            }}
            onMouseEnter={e => {
              e.currentTarget.style.borderColor = "#ef4444";
              e.currentTarget.style.boxShadow = "0 2px 12px rgba(239,68,68,0.1)";
            }}
            onMouseLeave={e => {
              e.currentTarget.style.borderColor = "#e2e8f0";
              e.currentTarget.style.boxShadow = "none";
            }}
          >
            <div style={{
              width: 10, height: 10, borderRadius: "50%",
              background: s.status === "active" ? "#ef4444" : "#94a3b8",
              flexShrink: 0,
              boxShadow: s.status === "active" ? "0 0 0 3px rgba(239,68,68,0.2)" : "none",
            }} />

            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 700, fontSize: 13, color: "#0f172a", marginBottom: 2 }}>
                {s.id}
              </div>
              <div style={{ fontSize: 11, color: "#64748b" }}>
                {s.device_id} · {fmt(s.started_at)} · {s.frame_count} frames
              </div>
              {s.ai_summary && (
                <div style={{
                  fontSize: 11, color: "#475569", marginTop: 4,
                  overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap",
                }}>
                  {s.ai_summary}
                </div>
              )}
            </div>

            {s.ai_threat_level && (
              <div style={{
                background: tc.bg, color: tc.text,
                padding: "3px 10px", borderRadius: 20,
                fontSize: 11, fontWeight: 700, flexShrink: 0,
              }}>
                {s.ai_threat_level}
              </div>
            )}

            <span style={{ color: "#cbd5e1", fontSize: 18 }}>›</span>
          </button>
        );
      })}
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  STATS CARDS
// ══════════════════════════════════════════════════════════════════
function StatsCards({ sessions }) {
  const total    = sessions.length;
  const active   = sessions.filter(s => s.status === "active").length;
  const critical = sessions.filter(s => s.ai_threat_level === "CRITICAL").length;
  const high     = sessions.filter(s => s.ai_threat_level === "HIGH").length;

  const cards = [
    { label: "Total Events", value: total,    color: "#0f172a" },
    { label: "Active SOS",   value: active,   color: "#ef4444" },
    { label: "Critical",     value: critical, color: "#db2777" },
    { label: "High Threat",  value: high,     color: "#d97706" },
  ];

  return (
    <div style={{ display: "grid", gridTemplateColumns: "repeat(4,1fr)", gap: 12, marginBottom: 24 }}>
      {cards.map(({ label, value, color }) => (
        <div key={label} style={{
          background: "#fff", border: "1.5px solid #e2e8f0",
          borderRadius: 12, padding: "16px 20px",
        }}>
          <div style={{ fontSize: 28, fontWeight: 800, color, lineHeight: 1 }}>{value}</div>
          <div style={{ fontSize: 11, color: "#94a3b8", marginTop: 4, fontWeight: 600 }}>{label}</div>
        </div>
      ))}
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  MAIN DASHBOARD
// ══════════════════════════════════════════════════════════════════
function Dashboard({ token, onLogout }) {
  const [sessions, setSessions]         = useState([]);
  const [activeEvent, setActiveEvent]   = useState(null);
  const [selectedSession, setSelected]  = useState(null);
  const [liveBC, setLiveBC]             = useState([]);
  const [wsStatus, setWsStatus]         = useState("connecting");
  const wsRef = useRef(null);

  const loadSessions = useCallback(async () => {
    try {
      const data = await api("/api/sessions", {}, token);
      setSessions(data);
    } catch (e) {
      console.error("Load sessions:", e);
    }
  }, [token]);

  useEffect(() => {
    loadSessions();
    const t = setInterval(loadSessions, 30000);
    return () => clearInterval(t);
  }, [loadSessions]);

  useEffect(() => {
    const wsUrl = `${BACKEND.replace("https://", "wss://").replace("http://", "ws://")}/ws/global`;
    wsRef.current = new WebSocket(wsUrl);

    wsRef.current.onopen  = () => setWsStatus("connected");
    wsRef.current.onclose = () => setWsStatus("disconnected");
    wsRef.current.onerror = () => setWsStatus("error");

    wsRef.current.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.type === "sos_active") {
        setActiveEvent(msg);
        loadSessions();
      }
      if (msg.type === "sos_complete") {
        setActiveEvent(prev => prev?.session_id === msg.session_id ? null : prev);
        loadSessions();
      }
      if (msg.type === "gps_update") {
        setLiveBC(prev => [...prev, { lat: msg.lat, lon: msg.lon }]);
      }
    };

    return () => wsRef.current?.close();
  }, [token, loadSessions]);

  const wsColors = { connected: "#22c55e", connecting: "#f59e0b", disconnected: "#94a3b8", error: "#ef4444" };

  return (
    <div style={{
      minHeight: "100vh", background: "#f8fafc",
      fontFamily: "'DM Mono', 'Courier New', monospace",
    }}>
      {/* SOS Banner */}
      <SOSBanner
        activeEvent={activeEvent}
        onView={() => {
          const s = sessions.find(ss => ss.id === activeEvent?.session_id);
          if (s) setSelected(s);
        }}
      />

      {/* Top Nav */}
      <nav style={{
        background: "#fff", borderBottom: "1.5px solid #e2e8f0",
        padding: "0 24px", display: "flex", alignItems: "center",
        height: 60, gap: 16,
        marginTop: activeEvent ? 56 : 0,
        position: "sticky", top: activeEvent ? 56 : 0, zIndex: 100,
      }}>
        <div style={{
          width: 32, height: 32, background: "#ef4444", borderRadius: 8,
          display: "flex", alignItems: "center", justifyContent: "center",
        }}>
          <span style={{ color: "#fff", fontWeight: 900, fontSize: 16 }}>S</span>
        </div>
        <div>
          <div style={{ fontSize: 13, fontWeight: 800, color: "#0f172a", letterSpacing: 0.5 }}>
            SAAK-GUARD
          </div>
          <div style={{ fontSize: 10, color: "#94a3b8", letterSpacing: 1 }}>
            FORENSIC DASHBOARD
          </div>
        </div>

        <div style={{ marginLeft: "auto", display: "flex", alignItems: "center", gap: 12 }}>
          {/* WS status */}
          <div style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 11, color: "#64748b" }}>
            <div style={{
              width: 7, height: 7, borderRadius: "50%",
              background: wsColors[wsStatus],
            }} />
            {wsStatus}
          </div>

          <button
            onClick={loadSessions}
            style={{
              padding: "6px 14px", background: "#f1f5f9",
              border: "none", borderRadius: 8, fontSize: 12,
              cursor: "pointer", fontFamily: "inherit", color: "#475569",
            }}
          >
            ↻ Refresh
          </button>
          <button
            onClick={onLogout}
            style={{
              padding: "6px 14px", background: "#fff",
              border: "1.5px solid #e2e8f0", borderRadius: 8, fontSize: 12,
              cursor: "pointer", fontFamily: "inherit", color: "#64748b",
            }}
          >
            Logout
          </button>
        </div>
      </nav>

      {/* Main Content */}
      <div style={{ maxWidth: 1200, margin: "0 auto", padding: "28px 24px" }}>
        <StatsCards sessions={sessions} />

        <div style={{ display: "grid", gridTemplateColumns: "1fr 1.2fr", gap: 20 }}>
          {/* Left: Session list */}
          <div>
            <div style={{
              fontSize: 11, fontWeight: 700, color: "#94a3b8",
              letterSpacing: 2, marginBottom: 12,
            }}>
              ALL EVENTS ({sessions.length})
            </div>
            <SessionsList sessions={sessions} onSelect={setSelected} />
          </div>

          {/* Right: Live Map */}
          <div>
            <div style={{
              fontSize: 11, fontWeight: 700, color: "#94a3b8",
              letterSpacing: 2, marginBottom: 12,
            }}>
              LIVE TRACKING MAP (OpenStreetMap — Free)
            </div>
            <div style={{ height: 480 }}>
              <LiveMap breadcrumbs={liveBC} activeEvent={activeEvent} />
            </div>
          </div>
        </div>
      </div>

      {/* Session Detail Modal */}
      {selectedSession && (
        <SessionPanel
          session={selectedSession}
          token={token}
          onClose={() => setSelected(null)}
        />
      )}

      <style>{`
        * { box-sizing: border-box; }
        body { margin: 0; }
        ::-webkit-scrollbar { width: 6px; }
        ::-webkit-scrollbar-track { background: #f1f5f9; }
        ::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 3px; }
      `}</style>
    </div>
  );
}

// ══════════════════════════════════════════════════════════════════
//  APP ROOT
// ══════════════════════════════════════════════════════════════════
export default function App() {
  const [token, setToken] = useState(() => sessionStorage.getItem("saak_token"));

  function handleAuth(t) {
    sessionStorage.setItem("saak_token", t);
    setToken(t);
  }

  function handleLogout() {
    sessionStorage.removeItem("saak_token");
    setToken(null);
  }

  if (!token) return <QRAuthScreen onAuth={handleAuth} />;
  return <Dashboard token={token} onLogout={handleLogout} />;
}
