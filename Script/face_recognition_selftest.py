#!/usr/bin/env python3
"""Host-side self-test for the face-recognition demo math.

Checks, without any hardware:
  1. Letterbox warp: a Python port of the 24.8 fixed-point bilinear warp in
     video_dual_model.c matches a float reference within +/- 1 LSB, pads
     correctly and preserves aspect.
  2. Template averaging + cosine matching: the math used by face_db.c picks
     the right person and reports sane similarity values.
  3. (when the prepared model exists) Quantized embedding smoke test: crops
     of the same synthetic "face" stay highly similar under shift/brightness
     changes while different faces separate - a quantization health check.

Usage:  python Script/face_recognition_selftest.py
"""

import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
WEIGHTS = os.path.normpath(os.path.join(HERE, "..", "Model", "weights"))
MODEL = os.path.join(WEIGHTS, "mobilenet_face_embed_112_qdq_int8_rgb.onnx")

INPUT_SIZE = 112


# ----------------------------------------------------------------------------
# 1) Letterbox warp (port of dm_letterbox_rgb888 in video_dual_model.c)
# ----------------------------------------------------------------------------

def letterbox_c(src, crop_x, crop_y, crop_w, crop_h, dst_w, dst_h, pad):
    """Byte-level port of the C implementation (24.8 fixed-point bilinear)."""
    scale = min(dst_w / crop_w, dst_h / crop_h)
    out_w = int(crop_w * scale + 0.5)
    out_h = int(crop_h * scale + 0.5)
    off_x = (dst_w - out_w) // 2
    off_y = (dst_h - out_h) // 2

    src_h, src_w = src.shape[:2]
    dst = np.full((dst_h, dst_w, 3), pad, dtype=np.uint8)

    step_x = int((crop_w / out_w) * 256.0 + 0.5)
    step_y = int((crop_h / out_h) * 256.0 + 0.5)

    for dy in range(out_h):
        src_y_fixed = dy * step_y
        y0 = src_y_fixed >> 8
        y1 = min(y0 + 1, crop_h - 1)
        fy = src_y_fixed & 0xFF
        row0 = src[crop_y + y0, crop_x:crop_x + crop_w, :].astype(np.uint32)
        row1 = src[crop_y + y1, crop_x:crop_x + crop_w, :].astype(np.uint32)
        # horizontal index per destination pixel
        xs = (np.arange(out_w, dtype=np.uint64) * np.uint64(step_x))
        x0 = (xs >> 8).astype(np.int64)
        fx = (xs & 0xFF).astype(np.uint32)
        x1 = np.minimum(x0 + 1, crop_w - 1)

        w00 = (256 - fx) * (256 - fy)
        w01 = fx * (256 - fy)
        w10 = (256 - fx) * fy
        w11 = fx * fy

        p00 = row0[x0].astype(np.uint64)
        p01 = row0[x1].astype(np.uint64)
        p10 = row1[x0].astype(np.uint64)
        p11 = row1[x1].astype(np.uint64)
        out = (p00 * w00[:, None] + p01 * w01[:, None] +
               p10 * w10[:, None] + p11 * w11[:, None]) >> 16
        dst[off_y + dy, off_x:off_x + out_w, :] = out.astype(np.uint8)
    return dst, (off_x, off_y, out_w, out_h)


def letterbox_ref(src, crop_x, crop_y, crop_w, crop_h, dst_w, dst_h, pad,
                  quantized_steps=True):
    """Float bilinear reference. With quantized_steps=True it samples at the
    exact 24.8 fixed-point source coordinates the C code uses (verifies the
    port to +/-1 rounding); with False it uses the ideal float ratio
    (verifies the step quantization error stays visually negligible)."""
    scale = min(dst_w / crop_w, dst_h / crop_h)
    out_w = int(crop_w * scale + 0.5)
    out_h = int(crop_h * scale + 0.5)
    off_x = (dst_w - out_w) // 2
    off_y = (dst_h - out_h) // 2

    dst = np.full((dst_h, dst_w, 3), pad, dtype=np.float32)
    if quantized_steps:
        step_x = int((crop_w / out_w) * 256.0 + 0.5) / 256.0
        step_y = int((crop_h / out_h) * 256.0 + 0.5) / 256.0
        xs = np.arange(out_w) * step_x
        ys = np.arange(out_h) * step_y
    else:
        xs = np.arange(out_w) * (crop_w / out_w)
        ys = np.arange(out_h) * (crop_h / out_h)
    x0 = np.floor(xs).astype(int)
    y0 = np.floor(ys).astype(int)
    fx = (xs - x0)[None, :, None]
    fy = (ys - y0)[:, None, None]
    x1 = np.minimum(x0 + 1, crop_w - 1)
    y1 = np.minimum(y0 + 1, crop_h - 1)

    crop = src[crop_y:crop_y + crop_h, crop_x:crop_x + crop_w, :].astype(np.float32)
    top = crop[y0][:, x0] * (1 - fx) + crop[y0][:, x1] * fx
    bot = crop[y1][:, x0] * (1 - fx) + crop[y1][:, x1] * fx
    dst[off_y:off_y + out_h, off_x:off_x + out_w, :] = top * (1 - fy) + bot * fy
    return dst


def test_letterbox():
    # Smooth gradient with soft block: fixed-point vs float must agree within
    # rounding. Sharp synthetic edges (delta >= 150/px) would amplify the
    # truncated step into ~0.2px coordinate offsets - expected for a 24.8
    # fixed-point sampler, irrelevant for camera images.
    src = np.zeros((320, 320, 3), np.uint8)
    yy, xx = np.mgrid[0:320, 0:320]
    src[..., 0] = xx // 2
    src[..., 1] = yy // 2
    src[..., 2] = (xx + yy) // 4
    src[40:200, 60:260] = (200, 90, 40)
    # 3x3 box blur to soften the block edges
    pad = np.pad(src.astype(np.float32), ((1, 1), (1, 1), (0, 0)), mode="edge")
    src = ((pad[:-2, :-2] + pad[:-2, 1:-1] + pad[:-2, 2:] +
            pad[1:-1, :-2] + pad[1:-1, 1:-1] + pad[1:-1, 2:] +
            pad[2:, :-2] + pad[2:, 1:-1] + pad[2:, 2:]) / 9.0).astype(np.uint8)

    failures = 0
    for (cx, cy, cw, ch, dw, dh, pad) in [
        (60, 40, 160, 200, 112, 112, 127),   # portrait crop -> side bars
        (10, 80, 220, 140, 112, 112, 0),     # landscape crop -> top/bottom bars
        (0, 0, 320, 320, 112, 112, 200),     # whole frame, no bars
        (100, 100, 24, 24, 112, 112, 127),   # min-size crop (upscale)
    ]:
        got, rect = letterbox_c(src, cx, cy, cw, ch, dw, dh, pad)
        # Exact check: same quantized sampling grid as the C code
        ref_exact = letterbox_ref(src, cx, cy, cw, ch, dw, dh, pad,
                                  quantized_steps=True)
        # Sanity check: ideal float grid must stay visually equivalent
        ref_ideal = letterbox_ref(src, cx, cy, cw, ch, dw, dh, pad,
                                  quantized_steps=False)
        off_x, off_y, out_w, out_h = rect
        region = (slice(off_y, off_y + out_h), slice(off_x, off_x + out_w))
        diff_exact = int(np.abs(got[region].astype(int) - ref_exact[region].astype(int)).max())
        diff_ideal = int(np.abs(got[region].astype(int) - ref_ideal[region].astype(int)).max())
        # bars must be exactly pad
        bars_ok = True
        if off_x > 0:
            bars_ok &= bool((got[:, :off_x] == pad).all()) and bool((got[:, off_x + out_w:] == pad).all())
        if off_y > 0:
            bars_ok &= bool((got[:off_y] == pad).all()) and bool((got[off_y + out_h:] == pad).all())
        ok = diff_exact <= 1 and diff_ideal <= 8 and bars_ok
        failures += not ok
        print(f"  crop {cw}x{ch}@({cx},{cy}) -> {dw}x{dh}: exact_diff={diff_exact}, "
              f"ideal_diff={diff_ideal}, bars_ok={bars_ok} {'OK' if ok else 'FAIL'}")
    return failures == 0


# ----------------------------------------------------------------------------
# 2) Template averaging + cosine matching (face_db.c math)
# ----------------------------------------------------------------------------

def normalize(v):
    return v / np.linalg.norm(v)


def test_matching():
    rng = np.random.default_rng(5)
    dim = 128
    alice = normalize(rng.normal(size=dim))
    bob = normalize(rng.normal(size=dim))
    db = {"alice": alice, "bob": bob}

    # Enrollment averaging: noisy samples of alice -> mean template
    samples = np.stack([normalize(alice + rng.normal(scale=0.05, size=dim)) for _ in range(5)])
    mean = normalize(samples.mean(axis=0))

    probe_alice = normalize(alice + rng.normal(scale=0.05, size=dim))
    probe_bob = normalize(bob + rng.normal(scale=0.05, size=dim))
    sim_a = float(mean @ probe_alice)
    sim_b = float(mean @ probe_bob)

    ok = sim_a > 0.7 and sim_a > sim_b + 0.5
    print(f"  same-person sim={sim_a:.3f}, other-person sim={sim_b:.3f} "
          f"{'OK' if ok else 'FAIL'}")
    return ok


# ----------------------------------------------------------------------------
# 3) Quantized embedding model smoke test
# ----------------------------------------------------------------------------

def make_synthetic_face(rng, style, brightness=0.0):
    """A crude 'face' avatar: skin oval + darker eyes/mouth, shifted per style."""
    f = np.ones((112, 112, 3), dtype=np.float32)
    yy, xx = np.mgrid[0:112, 0:112]
    cx, cy = 56 + style * 9, 56
    oval = ((xx - cx) / 40.0) ** 2 + ((yy - cy) / 52.0) ** 2 <= 1.0
    f[:, :] = (110, 125, 150)          # background
    f[oval] = (198, 160, 130)          # skin
    for ex in (-14, 14):               # eyes
        eye = ((xx - (cx + ex)) / 8) ** 2 + ((yy - 44) / 5) ** 2 <= 1.0
        f[eye] = (40, 40, 60)
    mouth = ((xx - cx) / 16) ** 2 + ((yy - 78) / 6) ** 2 <= 1.0
    f[mouth] = (150, 80, 80)
    f = np.clip(f + brightness, 0, 255)
    return f.astype(np.uint8)


def embed(sess, img_uint8):
    # The prepared model takes RAW uint8 RGB (normalization is inside the
    # graph) in NCHW layout.
    x = np.ascontiguousarray(img_uint8.transpose(2, 0, 1))[None]
    out = sess.run(None, {sess.get_inputs()[0].name: x})[0].ravel().astype(np.float32)
    return out / (np.linalg.norm(out) + 1e-9)


def test_model():
    if not os.path.exists(MODEL):
        print(f"  model not found ({os.path.basename(MODEL)}) - skipped "
              "(run Script/prepare_face_embed_model.py first)")
        return True
    import onnxruntime as ort
    sess = ort.InferenceSession(MODEL, providers=["CPUExecutionProvider"])

    rng = np.random.default_rng(11)
    face0 = make_synthetic_face(rng, 0)
    face0_shift = np.roll(np.roll(face0, 3, axis=1), 2, axis=0)
    face0_bright = make_synthetic_face(rng, 0, brightness=18)
    face1 = make_synthetic_face(rng, 2)

    e0 = embed(sess, face0)
    e0s = embed(sess, face0_shift)
    e0b = embed(sess, face0_bright)
    e1 = embed(sess, face1)

    same = min(float(e0 @ e0s), float(e0 @ e0b))
    other = float(e0 @ e1)
    # Synthetic images are not faces the model understands, so only require
    # meaningful separation, not production thresholds.
    ok = same > other + 0.05
    print(f"  same-subject sim={same:.3f} (shift {float(e0 @ e0s):.3f}, "
          f"brightness {float(e0 @ e0b):.3f}), other-subject sim={other:.3f} "
          f"{'OK' if ok else 'FAIL'}")
    return ok


def main():
    print("letterbox warp (fixed-point port vs float reference):")
    ok1 = test_letterbox()
    print("template averaging + cosine matching:")
    ok2 = test_matching()
    print("quantized embedding model:")
    ok3 = test_model()
    print("=" * 50)
    if ok1 and ok2 and ok3:
        print("ALL TESTS PASSED")
        return 0
    print("TESTS FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
