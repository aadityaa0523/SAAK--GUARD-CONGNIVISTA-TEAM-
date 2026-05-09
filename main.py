"""
╔══════════════════════════════════════════════════════════════════╗
  SAAK-Guard Forensic Backend  v4.0
  FastAPI + PostgreSQL + WebSocket + AI Analysis

  Deploy on Render.com:
    1. Create a PostgreSQL database on Render
    2. Set environment variables (see below)
    3. Create a Web Service pointing to this file
    4. Start command: uvicorn main:app --host 0.0.0.0 --port $PORT

  Required environment variables (set in Render dashboard):
    DATABASE_URL       - PostgreSQL connection string from Render
    ANTHROPIC_API_KEY  - Your Anthropic API key
    AES_KEY_HEX        - 64-char hex string (same as firmware)
    ECDSA_PRIVATE_KEY  - PEM encoded ECDSA-P256 private key
    MAPBOX_TOKEN       - Your Mapbox public token
    TOTP_SECRET        - Base32 secret for QR auth (generate once)
    ALLOWED_DEVICE_IDS - Comma-separated list of registered device IDs
╚══════════════════════════════════════════════════════════════════╝
"""

import os
import json
import time
import hashlib
import hmac
import base64
import struct
import asyncio
import io
from datetime import datetime, timezone
from typing import Optional, Dict, List
from pathlib import Path
import logging

# ── Core Dependencies ──────────────────────────────────────────────
from fastapi import (FastAPI, WebSocket, WebSocketDisconnect, HTTPException,
                     UploadFile, File, Header, Depends, BackgroundTasks,
                     Request, Response)
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
import uvicorn

# ── Database ───────────────────────────────────────────────────────
import asyncpg
from asyncpg.pool import Pool

# ── Crypto ────────────────────────────────────────────────────────
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend

# ── Media Processing ──────────────────────────────────────────────
import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import ffmpeg  # python-ffmpeg

# ── AI / Anthropic ────────────────────────────────────────────────
import anthropic

# ── QR Auth ───────────────────────────────────────────────────────
import pyotp
import qrcode

# ── Setup ─────────────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("saak-guard")

app = FastAPI(title="SAAK-Guard Forensic Backend", version="4.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # Restrict in production to your dashboard URL
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── Config from env ────────────────────────────────────────────────
DATABASE_URL       = os.getenv("DATABASE_URL", "postgresql://user:pass@localhost/saakguard")
ANTHROPIC_API_KEY  = os.getenv("ANTHROPIC_API_KEY", "")
AES_KEY_HEX        = os.getenv("AES_KEY_HEX", "0" * 64)
TOTP_SECRET        = os.getenv("TOTP_SECRET", pyotp.random_base32())
ALLOWED_DEVICES    = set(os.getenv("ALLOWED_DEVICE_IDS", "SAAK-001").split(","))
ECDSA_PRIVATE_PEM  = os.getenv("ECDSA_PRIVATE_KEY", "")
MEDIA_DIR          = Path("media")
MEDIA_DIR.mkdir(exist_ok=True)

AES_KEY = bytes.fromhex(AES_KEY_HEX)

# ── Global State ──────────────────────────────────────────────────
db_pool: Optional[Pool]             = None
ws_connections: Dict[str, List[WebSocket]] = {}   # session_id → [ws]
active_sessions: Dict[str, dict]    = {}           # session_id → metadata
ai_client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY) if ANTHROPIC_API_KEY else None


# ══════════════════════════════════════════════════════════════════
#  DATABASE
# ══════════════════════════════════════════════════════════════════
async def get_db() -> Pool:
    return db_pool

async def init_db():
    global db_pool
    db_pool = await asyncpg.create_pool(DATABASE_URL, min_size=2, max_size=10)
    async with db_pool.acquire() as conn:
        await conn.execute("""
            CREATE TABLE IF NOT EXISTS sos_sessions (
                id              TEXT PRIMARY KEY,
                device_id       TEXT NOT NULL,
                trigger_type    TEXT NOT NULL,
                started_at      TIMESTAMPTZ DEFAULT NOW(),
                ended_at        TIMESTAMPTZ,
                gps_lat         DOUBLE PRECISION,
                gps_lon         DOUBLE PRECISION,
                frame_count     INTEGER DEFAULT 0,
                audio_duration  REAL DEFAULT 0,
                ai_summary      TEXT,
                ai_threat_level TEXT,
                ai_transcript   TEXT,
                video_path      TEXT,
                audio_path      TEXT,
                signature       TEXT,
                file_hash       TEXT,
                status          TEXT DEFAULT 'active',
                metadata        JSONB DEFAULT '{}'
            );

            CREATE TABLE IF NOT EXISTS gps_breadcrumbs (
                id          SERIAL PRIMARY KEY,
                session_id  TEXT REFERENCES sos_sessions(id),
                lat         DOUBLE PRECISION,
                lon         DOUBLE PRECISION,
                speed       REAL,
                heading     REAL,
                recorded_at TIMESTAMPTZ DEFAULT NOW()
            );

            CREATE TABLE IF NOT EXISTS frames (
                id          SERIAL PRIMARY KEY,
                session_id  TEXT REFERENCES sos_sessions(id),
                frame_num   INTEGER,
                file_path   TEXT,
                file_hash   TEXT,
                gps_lat     DOUBLE PRECISION,
                gps_lon     DOUBLE PRECISION,
                watermarked BOOLEAN DEFAULT FALSE,
                recorded_at TIMESTAMPTZ DEFAULT NOW()
            );

            CREATE TABLE IF NOT EXISTS auth_sessions (
                token       TEXT PRIMARY KEY,
                created_at  TIMESTAMPTZ DEFAULT NOW(),
                expires_at  TIMESTAMPTZ,
                is_valid    BOOLEAN DEFAULT TRUE
            );
        """)
    logger.info("Database initialized")


# ══════════════════════════════════════════════════════════════════
#  CRYPTO HELPERS
# ══════════════════════════════════════════════════════════════════
def aes_decrypt(encrypted_bytes: bytes, iv: bytes) -> bytes:
    """AES-256-CBC decrypt and strip PKCS#7 padding."""
    cipher     = Cipher(algorithms.AES(AES_KEY), modes.CBC(iv), backend=default_backend())
    decryptor  = cipher.decryptor()
    padded     = decryptor.update(encrypted_bytes) + decryptor.finalize()
    pad_len    = padded[-1]
    return padded[:-pad_len]


def sha256_file(file_path: str) -> str:
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def ecdsa_sign_file(file_path: str) -> str:
    """Sign a file with ECDSA-P256. Returns base64 DER signature."""
    if not ECDSA_PRIVATE_PEM:
        return "NO_KEY_CONFIGURED"
    try:
        private_key = serialization.load_pem_private_key(
            ECDSA_PRIVATE_PEM.encode(), password=None, backend=default_backend()
        )
        file_hash = hashlib.sha256(open(file_path, "rb").read()).digest()
        signature = private_key.sign(file_hash, ec.ECDSA(hashes.SHA256()))
        return base64.b64encode(signature).decode()
    except Exception as e:
        logger.error(f"ECDSA signing failed: {e}")
        return "SIGN_ERROR"


# ══════════════════════════════════════════════════════════════════
#  FORENSIC WATERMARKING (OpenCV)
# ══════════════════════════════════════════════════════════════════
def apply_watermark(
    frame_bytes: bytes,
    device_id: str,
    session_id: str,
    frame_num: int,
    lat: float,
    lon: float,
    timestamp: str
) -> bytes:
    """
    Burn permanent forensic watermark onto JPEG frame.
    Returns watermarked JPEG bytes.
    """
    # Decode JPEG
    arr = np.frombuffer(frame_bytes, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        return frame_bytes

    h, w = img.shape[:2]

    # Semi-transparent dark banner at bottom
    overlay = img.copy()
    banner_h = max(40, int(h * 0.08))
    cv2.rectangle(overlay, (0, h - banner_h), (w, h), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.65, img, 0.35, 0, img)

    # Top-left stamp
    stamp_h = max(24, int(h * 0.05))
    cv2.rectangle(img, (0, 0), (w, stamp_h), (0, 0, 80), -1)

    # Watermark text
    font       = cv2.FONT_HERSHEY_DUPLEX
    font_scale = max(0.35, h / 1200.0)
    color_wht  = (220, 220, 220)
    color_red  = (60, 60, 220)
    thick      = 1

    bottom_y   = h - banner_h + int(banner_h * 0.35)

    lines = [
        f"[OFFICIAL USE ONLY]  Device: {device_id}  Session: {session_id[:8]}",
        f"Frame: {frame_num:04d}  GPS: {lat:.6f}, {lon:.6f}  {timestamp}",
    ]
    cv2.putText(img, lines[0], (8, bottom_y),        font, font_scale, color_red, thick, cv2.LINE_AA)
    cv2.putText(img, lines[1], (8, bottom_y + int(banner_h * 0.55)),
                font, font_scale, color_wht, thick, cv2.LINE_AA)

    # Top banner text
    cv2.putText(img, f"SAAK-GUARD FORENSIC EVIDENCE — {timestamp}",
                (8, stamp_h - 5), font, font_scale * 0.9, color_wht, thick, cv2.LINE_AA)

    # Diagonal "EVIDENCE" ghost watermark in center
    center_text = "EVIDENCE"
    (tw, tth), _ = cv2.getTextSize(center_text, cv2.FONT_HERSHEY_SIMPLEX, 2.0, 2)
    cx, cy = (w - tw) // 2, (h + tth) // 2
    ghost = img.copy()
    cv2.putText(ghost, center_text, (cx, cy),
                cv2.FONT_HERSHEY_SIMPLEX, 2.0, (200, 200, 200), 2, cv2.LINE_AA)
    cv2.addWeighted(ghost, 0.08, img, 0.92, 0, img)

    # Encode back to JPEG
    _, enc = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 88])
    return enc.tobytes()


# ══════════════════════════════════════════════════════════════════
#  AI ANALYSIS (Claude)
# ══════════════════════════════════════════════════════════════════
async def run_ai_analysis(session_id: str, audio_path: Optional[str] = None):
    """
    Generate First Responder Summary using Claude.
    Since Whisper is not bundled, we use Claude for analysis
    of whatever metadata / context we have.
    For full transcription: integrate OpenAI Whisper API with audio_path.
    """
    if not ai_client:
        logger.warning("No Anthropic API key — skipping AI analysis")
        return

    session = active_sessions.get(session_id, {})

    prompt = f"""You are a forensic AI assistant for emergency responders.

Analyze this SOS emergency event and produce a First Responder Summary:

Device ID: {session.get('device_id', 'Unknown')}
Trigger: {session.get('trigger', 'Unknown')}
Time: {session.get('started_at', 'Unknown')}
Location: Lat {session.get('lat', 0):.6f}, Lon {session.get('lon', 0):.6f}
Frames Captured: {session.get('frame_count', 0)}

[In production, audio transcript would be inserted here from Whisper API]

Respond ONLY with a valid JSON object with these fields:
{{
  "threat_level": "LOW|MEDIUM|HIGH|CRITICAL",
  "summary": "2-3 sentence summary for first responders",
  "tone_analysis": "description of audio tone if available",
  "recommended_action": "specific action first responders should take",
  "estimated_danger": "assessment of immediate danger level",
  "key_observations": ["observation 1", "observation 2"]
}}"""

    try:
        msg = ai_client.messages.create(
            model="claude-sonnet-4-20250514",
            max_tokens=800,
            messages=[{"role": "user", "content": prompt}]
        )
        raw = msg.content[0].text.strip()
        # Strip markdown fences if present
        if raw.startswith("```"):
            raw = raw.split("\n", 1)[1].rsplit("```", 1)[0]
        analysis = json.loads(raw)

        async with db_pool.acquire() as conn:
            await conn.execute("""
                UPDATE sos_sessions
                SET ai_summary      = $1,
                    ai_threat_level = $2,
                    ai_transcript   = $3
                WHERE id = $4
            """, analysis.get("summary"),
                 analysis.get("threat_level"),
                 analysis.get("tone_analysis"),
                 session_id)

        # Push analysis to all connected dashboards
        await ws_broadcast(session_id, {
            "type": "ai_analysis",
            "session_id": session_id,
            "analysis": analysis
        })

        logger.info(f"AI analysis complete for {session_id}: {analysis['threat_level']}")
    except Exception as e:
        logger.error(f"AI analysis error: {e}")


# ══════════════════════════════════════════════════════════════════
#  VIDEO ASSEMBLY (JPEG → MP4)
# ══════════════════════════════════════════════════════════════════
def assemble_video(session_dir: Path, output_path: str) -> bool:
    """Stitch watermarked JPEGs into MP4 using ffmpeg."""
    try:
        pattern = str(session_dir / "watermarked" / "frame_%04d.jpg")
        (
            ffmpeg
            .input(pattern, framerate=10)
            .output(output_path, vcodec="libx264", crf=22, pix_fmt="yuv420p")
            .overwrite_output()
            .run(quiet=True)
        )
        return True
    except Exception as e:
        logger.error(f"Video assembly failed: {e}")
        return False


# ══════════════════════════════════════════════════════════════════
#  WEBSOCKET MANAGER
# ══════════════════════════════════════════════════════════════════
async def ws_broadcast(session_id: str, message: dict):
    conns = ws_connections.get(session_id, [])
    dead  = []
    for ws in conns:
        try:
            await ws.send_json(message)
        except Exception:
            dead.append(ws)
    for ws in dead:
        conns.remove(ws)

async def ws_broadcast_all(message: dict):
    """Broadcast to ALL connected websocket clients."""
    all_conns = [ws for conns in ws_connections.values() for ws in conns]
    for ws in all_conns:
        try:
            await ws.send_json(message)
        except Exception:
            pass


# ══════════════════════════════════════════════════════════════════
#  QR AUTH
# ══════════════════════════════════════════════════════════════════
def verify_totp(code: str) -> bool:
    totp = pyotp.TOTP(TOTP_SECRET, interval=30)
    return totp.verify(code, valid_window=1)

@app.get("/api/auth/qr")
async def get_qr_code():
    """Generate a QR code encoding the TOTP URI for authenticator apps."""
    totp     = pyotp.TOTP(TOTP_SECRET)
    uri      = totp.provisioning_uri(name="Officer", issuer_name="SAAK-Guard")
    qr_img   = qrcode.make(uri)
    buf      = io.BytesIO()
    qr_img.save(buf, format="PNG")
    buf.seek(0)
    return StreamingResponse(buf, media_type="image/png")

@app.post("/api/auth/verify")
async def verify_auth(request: Request):
    body = await request.json()
    code = str(body.get("code", ""))
    if verify_totp(code):
        token = base64.b64encode(os.urandom(32)).decode()
        expires = datetime.now(timezone.utc).timestamp() + 3600  # 1 hour
        async with db_pool.acquire() as conn:
            await conn.execute("""
                INSERT INTO auth_sessions (token, expires_at)
                VALUES ($1, to_timestamp($2))
            """, token, expires)
        return {"token": token, "expires_in": 3600}
    raise HTTPException(status_code=401, detail="Invalid TOTP code")

async def require_auth(authorization: str = Header(None)):
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="No auth token")
    token = authorization.split(" ", 1)[1]
    async with db_pool.acquire() as conn:
        row = await conn.fetchrow("""
            SELECT is_valid, expires_at FROM auth_sessions
            WHERE token = $1
        """, token)
    if not row or not row["is_valid"]:
        raise HTTPException(status_code=401, detail="Invalid token")
    if row["expires_at"] < datetime.now(timezone.utc):
        raise HTTPException(status_code=401, detail="Token expired")
    return token


# ══════════════════════════════════════════════════════════════════
#  SOS ENDPOINTS (Device → Server)
# ══════════════════════════════════════════════════════════════════
def validate_device(device_id: str):
    if device_id not in ALLOWED_DEVICES:
        raise HTTPException(status_code=403, detail=f"Unregistered device: {device_id}")

@app.post("/api/sos/register")
async def sos_register(
    request: Request,
    background_tasks: BackgroundTasks,
    x_device_id: str = Header(None)
):
    """Device calls this when SOS is triggered. Creates a session."""
    validate_device(x_device_id)
    body       = await request.json()
    session_id = f"{x_device_id}_{int(time.time()*1000)}"
    lat        = float(body.get("gps_lat", 0))
    lon        = float(body.get("gps_lon", 0))
    trigger    = body.get("trigger", "unknown")

    async with db_pool.acquire() as conn:
        await conn.execute("""
            INSERT INTO sos_sessions (id, device_id, trigger_type, gps_lat, gps_lon)
            VALUES ($1, $2, $3, $4, $5)
        """, session_id, x_device_id, trigger, lat, lon)

    active_sessions[session_id] = {
        "device_id":   x_device_id,
        "trigger":     trigger,
        "started_at":  datetime.now(timezone.utc).isoformat(),
        "lat":         lat,
        "lon":         lon,
        "frame_count": 0,
    }

    # Create media directory
    session_dir = MEDIA_DIR / session_id
    session_dir.mkdir(parents=True, exist_ok=True)
    (session_dir / "watermarked").mkdir(exist_ok=True)

    # Notify dashboard via WebSocket
    await ws_broadcast_all({
        "type":       "sos_active",
        "session_id": session_id,
        "device_id":  x_device_id,
        "trigger":    trigger,
        "lat":        lat,
        "lon":        lon,
        "timestamp":  datetime.now(timezone.utc).isoformat(),
    })

    logger.info(f"SOS registered: {session_id}")
    return {"session_id": session_id, "status": "recording"}


@app.post("/api/sos/frame")
async def receive_frame(
    request: Request,
    x_device_id:  str = Header(None),
    x_session_id: str = Header(None),
    x_frame_num:  str = Header(None),
    x_gps_lat:    str = Header("0"),
    x_gps_lon:    str = Header("0"),
    x_frame_hash: str = Header(""),
    x_iv:         str = Header(None),
):
    """Receive an encrypted JPEG frame from the device."""
    validate_device(x_device_id)

    encrypted_bytes = await request.body()
    frame_num       = int(x_frame_num or 0)
    lat             = float(x_gps_lat)
    lon             = float(x_gps_lon)

    # Decrypt frame
    try:
        if x_iv:
            iv_bytes  = x_iv.encode()[:16].ljust(16, b'\x00')
            raw_frame = aes_decrypt(encrypted_bytes, iv_bytes)
        else:
            raw_frame = encrypted_bytes  # fallback: unencrypted
    except Exception as e:
        logger.warning(f"Decrypt failed frame {frame_num}: {e} — storing raw")
        raw_frame = encrypted_bytes

    # Verify integrity
    actual_hash = hashlib.sha256(raw_frame).hexdigest()
    if x_frame_hash and actual_hash != x_frame_hash:
        logger.warning(f"Frame {frame_num} hash mismatch!")

    # Apply forensic watermark
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    wm_frame = apply_watermark(raw_frame, x_device_id, x_session_id,
                               frame_num, lat, lon, ts)

    # Save to disk
    session_dir  = MEDIA_DIR / x_session_id
    raw_path     = session_dir / f"frame_{frame_num:04d}.jpg"
    wm_path      = session_dir / "watermarked" / f"frame_{frame_num:04d}.jpg"

    raw_path.write_bytes(raw_frame)
    wm_path.write_bytes(wm_frame)

    # Update GPS breadcrumb
    if lat != 0.0 or lon != 0.0:
        async with db_pool.acquire() as conn:
            await conn.execute("""
                INSERT INTO gps_breadcrumbs (session_id, lat, lon)
                VALUES ($1, $2, $3)
            """, x_session_id, lat, lon)

    # Update session frame count
    if x_session_id in active_sessions:
        active_sessions[x_session_id]["frame_count"] = frame_num
        active_sessions[x_session_id]["lat"] = lat
        active_sessions[x_session_id]["lon"] = lon

    async with db_pool.acquire() as conn:
        await conn.execute("""
            INSERT INTO frames (session_id, frame_num, file_path, file_hash, gps_lat, gps_lon, watermarked)
            VALUES ($1, $2, $3, $4, $5, $6, TRUE)
        """, x_session_id, frame_num, str(wm_path), actual_hash, lat, lon)
        await conn.execute("""
            UPDATE sos_sessions SET frame_count = $1, gps_lat = $2, gps_lon = $3
            WHERE id = $4
        """, frame_num, lat, lon, x_session_id)

    # Push live frame URL to dashboard via WebSocket
    await ws_broadcast(x_session_id, {
        "type":       "new_frame",
        "session_id": x_session_id,
        "frame_num":  frame_num,
        "frame_url":  f"/media/{x_session_id}/watermarked/frame_{frame_num:04d}.jpg",
        "lat":        lat,
        "lon":        lon,
        "timestamp":  ts,
    })

    return {"status": "ok", "frame": frame_num}


@app.post("/api/sos/audio")
async def receive_audio(
    file: UploadFile = File(...),
    x_device_id:  str = Header(None),
    x_session_id: str = Header(None),
):
    """Receive WAV audio from device. Saves and queues AI transcription."""
    validate_device(x_device_id)

    audio_data  = await file.read()
    session_dir = MEDIA_DIR / x_session_id
    audio_path  = session_dir / "audio.wav"
    audio_path.write_bytes(audio_data)

    async with db_pool.acquire() as conn:
        await conn.execute("""
            UPDATE sos_sessions SET audio_path = $1 WHERE id = $2
        """, str(audio_path), x_session_id)

    # Trigger AI analysis in background
    asyncio.create_task(run_ai_analysis(x_session_id, str(audio_path)))

    return {"status": "audio_received"}


@app.post("/api/sos/end")
async def sos_end(
    request: Request,
    background_tasks: BackgroundTasks,
    x_device_id:  str = Header(None),
    x_session_id: str = Header(None),
):
    """Device signals end of recording. Triggers video assembly and signing."""
    validate_device(x_device_id)

    session_dir = MEDIA_DIR / x_session_id
    video_path  = str(session_dir / "evidence.mp4")

    # Assemble video in background
    background_tasks.add_task(finalize_session, x_session_id, session_dir, video_path)

    return {"status": "finalizing"}


async def finalize_session(session_id: str, session_dir: Path, video_path: str):
    """Assemble MP4, sign files, update DB."""
    # Assemble watermarked video
    assembled = assemble_video(session_dir, video_path)
    if assembled:
        vh   = sha256_file(video_path)
        vsig = ecdsa_sign_file(video_path)
    else:
        vh, vsig = "", ""

    audio_path = str(session_dir / "audio.wav")
    ah         = sha256_file(audio_path) if Path(audio_path).exists() else ""
    asig       = ecdsa_sign_file(audio_path) if Path(audio_path).exists() else ""

    async with db_pool.acquire() as conn:
        await conn.execute("""
            UPDATE sos_sessions
            SET video_path = $1,
                audio_path = $2,
                file_hash  = $3,
                signature  = $4,
                status     = 'complete',
                ended_at   = NOW()
            WHERE id = $5
        """, video_path, audio_path,
             json.dumps({"video_sha256": vh, "audio_sha256": ah}),
             json.dumps({"video_sig": vsig, "audio_sig": asig}),
             session_id)

    # Notify dashboard: SOS complete
    await ws_broadcast_all({
        "type":       "sos_complete",
        "session_id": session_id,
        "video_hash": vh,
        "signature":  vsig,
    })

    # Run AI analysis if not done yet
    await run_ai_analysis(session_id)

    if session_id in active_sessions:
        del active_sessions[session_id]

    logger.info(f"Session finalized: {session_id}")


# ══════════════════════════════════════════════════════════════════
#  DASHBOARD ENDPOINTS (Officer Dashboard → Server)
# ══════════════════════════════════════════════════════════════════
@app.get("/api/sessions")
async def list_sessions(
    limit: int = 50,
    offset: int = 0,
    _token = Depends(require_auth)
):
    async with db_pool.acquire() as conn:
        rows = await conn.fetch("""
            SELECT id, device_id, trigger_type, started_at, ended_at,
                   gps_lat, gps_lon, frame_count, ai_threat_level,
                   ai_summary, status
            FROM sos_sessions
            ORDER BY started_at DESC
            LIMIT $1 OFFSET $2
        """, limit, offset)
    return [dict(r) for r in rows]


@app.get("/api/sessions/{session_id}")
async def get_session(session_id: str, _token = Depends(require_auth)):
    async with db_pool.acquire() as conn:
        row = await conn.fetchrow("""
            SELECT * FROM sos_sessions WHERE id = $1
        """, session_id)
    if not row:
        raise HTTPException(status_code=404, detail="Session not found")
    return dict(row)


@app.get("/api/sessions/{session_id}/breadcrumbs")
async def get_breadcrumbs(session_id: str, _token = Depends(require_auth)):
    async with db_pool.acquire() as conn:
        rows = await conn.fetch("""
            SELECT lat, lon, recorded_at
            FROM gps_breadcrumbs
            WHERE session_id = $1
            ORDER BY recorded_at ASC
        """, session_id)
    return [dict(r) for r in rows]


@app.get("/api/sessions/{session_id}/frames")
async def list_frames(session_id: str, _token = Depends(require_auth)):
    async with db_pool.acquire() as conn:
        rows = await conn.fetch("""
            SELECT frame_num, file_path, file_hash, gps_lat, gps_lon, recorded_at
            FROM frames WHERE session_id = $1
            ORDER BY frame_num ASC
        """, session_id)
    return [dict(r) for r in rows]


@app.get("/api/active")
async def get_active_sessions(_token = Depends(require_auth)):
    return list(active_sessions.values())


# ══════════════════════════════════════════════════════════════════
#  WEBSOCKET (Live Dashboard Push)
# ══════════════════════════════════════════════════════════════════
@app.websocket("/ws/{session_id}")
async def websocket_session(websocket: WebSocket, session_id: str):
    """Per-session live updates."""
    await websocket.accept()
    if session_id not in ws_connections:
        ws_connections[session_id] = []
    ws_connections[session_id].append(websocket)
    logger.info(f"WS connected: {session_id}")
    try:
        while True:
            await websocket.receive_text()   # keep-alive ping
    except WebSocketDisconnect:
        ws_connections[session_id].remove(websocket)
        logger.info(f"WS disconnected: {session_id}")


@app.websocket("/ws/global")
async def websocket_global(websocket: WebSocket):
    """Global dashboard stream (all events)."""
    await websocket.accept()
    if "global" not in ws_connections:
        ws_connections["global"] = []
    ws_connections["global"].append(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        ws_connections["global"].remove(websocket)


# ══════════════════════════════════════════════════════════════════
#  MEDIA FILES (Protected)
# ══════════════════════════════════════════════════════════════════
@app.get("/media/{session_id}/{filename:path}")
async def serve_media(
    session_id: str,
    filename: str,
    _token = Depends(require_auth)
):
    file_path = MEDIA_DIR / session_id / filename
    if not file_path.exists():
        raise HTTPException(status_code=404)
    return FileResponse(str(file_path))


# ══════════════════════════════════════════════════════════════════
#  STARTUP / SHUTDOWN
# ══════════════════════════════════════════════════════════════════
@app.on_event("startup")
async def startup():
    await init_db()
    logger.info("SAAK-Guard backend started")

@app.on_event("shutdown")
async def shutdown():
    if db_pool:
        await db_pool.close()

@app.get("/health")
async def health():
    return {"status": "ok", "version": "4.0", "time": datetime.now().isoformat()}


# ══════════════════════════════════════════════════════════════════
#  ENTRY POINT
# ══════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    port = int(os.getenv("PORT", 8000))
    uvicorn.run("main:app", host="0.0.0.0", port=port, reload=False)
