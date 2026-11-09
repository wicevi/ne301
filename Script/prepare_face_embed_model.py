#!/usr/bin/env python3
"""Prepare the face-embedding Model B package for the NE302 dual-model demo.

Turns a float face-embedding ONNX (default: the insightface MobileFaceNet
``w600k_mbf`` from the buffalo_s release) into the QDQ int8 ONNX + deployment
JSON pair the ST Edge AI toolchain expects:

    Model/weights/mobilenet_face_embed_112_qdq_int8_rgb.{onnx,json}

Pipeline
  1. source ONNX  (``--input`` or ``--download`` of the buffalo_s zip)
  2. graph surgery: prepend (x - 127.5) / 127.5 normalization - the insightface
     runtime applies it outside the graph, but the NE302 pipe feeds raw RGB888
     uint8, so it must live INSIDE the network (``--no-normalize`` skips this
     when the source model already normalizes)
  3. optional BGR->RGB first-conv weight swap (``--swap-bgr-rgb``) - only for
     models trained on BGR; the insightface models take RGB (their loader uses
     swapRB=True), so the default is off
  4. numeric verification with onnxruntime: modified(raw input) vs
     original(preprocessed input) must match to float precision
  5. static QDQ int8 quantization (per-channel weights, calibration on
     synthetic face-ish images; ``--calib-dir`` uses real JPEG crops instead)
  6. writes the ONNX next to a deployment JSON bound to ``pp_face_embed``

The result is compiled/flashed like any other model:
    make model MODEL_NAME=mobilenet_face_embed_112_qdq_int8_rgb
    # burn to the AI_2 slot (Model B) per Docs/Face_Recognition_Demo.md
"""

import argparse
import io
import json
import math
import os
import sys
import urllib.request
import zipfile

DEFAULT_URL = "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_s.zip"
DEFAULT_MEMBER = "w600k_mbf.onnx"  # MobileFaceNet-class ArcFace, 112x112 RGB
DEFAULT_INPUT_SIZE = 112
DEFAULT_OUT_NAME = "mobilenet_face_embed_112_qdq_int8_rgb"

INPUT_MEAN = 127.5
INPUT_STD = 127.5


def log(msg):
    print(f"[prepare_face_embed] {msg}")


def download_model(url, member, workdir):
    zip_path = os.path.join(workdir, os.path.basename(url))
    if not os.path.exists(zip_path):
        log(f"downloading {url}")
        urllib.request.urlretrieve(url, zip_path)
    with zipfile.ZipFile(zip_path) as zf:
        data = zf.read(member)
        out = os.path.join(workdir, member)
        with open(out, "wb") as f:
            f.write(data)
    log(f"extracted {member} ({len(data)} bytes)")
    return out


def first_conv_weight_swap_to_rgb(model):
    """Reverse the input-channel slice of the first conv weight (BGR->RGB).

    Exact-equivalence surgery: conv is linear over input channels and a
    channel permutation is an involution, so swapping the slices makes a BGR
    model consume RGB with bit-identical outputs (same trick as
    Script/yunet_bgr_to_rgb.py)."""
    import onnx
    from onnx import numpy_helper

    graph = model.graph
    input_name = graph.input[0].name
    conv = None
    for node in graph.node:
        if node.op_type == "Conv" and node.input[0] == input_name:
            conv = node
            break
    if conv is None:
        raise RuntimeError("no Conv consumes the graph input - cannot swap channels")

    init_names = {i.name for i in graph.initializer}
    if conv.input[1] not in init_names:
        raise RuntimeError("first conv weight is not a constant initializer")

    for init in graph.initializer:
        if init.name == conv.input[1]:
            w = numpy_helper.to_array(init).copy()
            if w.ndim != 4:
                raise RuntimeError(f"unexpected weight rank {w.ndim}")
            w[:, :3, :, :] = w[:, ::-1, :, :]
            new_init = numpy_helper.from_array(w, init.name)
            init.CopyFrom(new_init)
            log(f"swapped first-conv input channels (weight {tuple(w.shape)})")
            return
    raise RuntimeError("weight initializer not found")


def prepend_normalization(model, mean, std):
    """Rename the graph input and prepend (x - mean) / std on the raw path."""
    import onnx
    from onnx import helper, TensorProto

    graph = model.graph
    orig = graph.input[0]
    shape = [d.dim_value if d.dim_value else d.dim_param for d in orig.type.tensor_type.shape.dim]
    new_in = helper.make_tensor_value_info(
        "embed_raw_input", orig.type.tensor_type.elem_type, shape)

    sub_out = "embed_norm_sub"
    div_out = orig.name  # feed the original graph unchanged
    sub_node = helper.make_node(
        "Sub", ["embed_raw_input", "embed_mean_const"], [sub_out], name="embed_norm_sub")
    div_node = helper.make_node(
        "Div", [sub_out, "embed_std_const"], [div_out], name="embed_norm_div")
    mean_init = helper.make_tensor(
        "embed_mean_const", TensorProto.FLOAT, [], [mean])
    std_init = helper.make_tensor(
        "embed_std_const", TensorProto.FLOAT, [], [std])

    graph.node.insert(0, sub_node)
    graph.node.insert(1, div_node)
    graph.input.insert(0, new_in)
    graph.input.remove(orig)
    graph.initializer.append(mean_init)
    graph.initializer.append(std_init)
    log(f"prepended normalization (x - {mean}) / {std}")


def verify_equivalence(src_path, mod_path, size, swap_rgb, mean, std):
    import numpy as np
    import onnxruntime as ort

    src = ort.InferenceSession(src_path, providers=["CPUExecutionProvider"])
    mod = ort.InferenceSession(mod_path, providers=["CPUExecutionProvider"])
    src_in, mod_in = src.get_inputs()[0].name, mod.get_inputs()[0].name

    rng = np.random.default_rng(7)
    max_diff = 0.0
    for _ in range(5):
        raw = rng.integers(0, 256, size=(1, 3, size, size), dtype=np.uint8).astype(np.float32)
        if swap_rgb:
            raw_ref = raw[:, ::-1, :, :]
        else:
            raw_ref = raw
        norm = (raw_ref - mean) / std  # what the original model expects
        out_src = src.run(None, {src_in: norm})[0]
        out_mod = mod.run(None, {mod_in: raw})[0]
        max_diff = max(max_diff, float(np.abs(out_src - out_mod).max()))
    log(f"verification max diff = {max_diff:.3e} (expect ~1e-5)")
    return max_diff


def calib_images(calib_dir, size, n):
    """Synthetic face-ish images (or real JPEGs from --calib-dir)."""
    import numpy as np

    rng = np.random.default_rng(42)
    if calib_dir:
        try:
            import cv2  # optional
        except ImportError:
            raise RuntimeError("--calib-dir requires opencv-python")
        files = [os.path.join(calib_dir, f) for f in os.listdir(calib_dir)
                 if f.lower().endswith((".jpg", ".jpeg", ".png"))][:n]
        if not files:
            raise RuntimeError(f"no images under {calib_dir}")
        out = []
        for p in files:
            img = cv2.imread(p)
            if img is None:
                continue
            img = cv2.cvtColor(cv2.resize(img, (size, size)), cv2.COLOR_BGR2RGB)
            out.append(img.astype(np.float32).transpose(2, 0, 1)[None])
        if out:
            log(f"calibrating on {len(out)} real images")
            return out

    # Synthetic: smooth color blobs in skin/lighting ranges - good enough to
    # bound activation ranges for the demo; real data is recommended for
    # production accuracy.
    out = []
    for _ in range(n):
        base = rng.uniform(50, 205, size=(3, 1, 1)).astype(np.float32)
        noise = rng.normal(0, 18, size=(3, size, size)).astype(np.float32)
        img = np.clip(base + noise, 0, 255)
        out.append(img[None])
    log(f"calibrating on {n} synthetic images")
    return out


class _CalibReader:
    def __init__(self, batches, input_name):
        self.batches = batches
        self.input_name = input_name
        self._it = iter(batches)

    def get_next(self):
        try:
            return {self.input_name: next(self._it)}
        except StopIteration:
            return None


def quantize_qdq(float_path, out_path, size, calib_dir, n_calib, per_channel):
    from onnxruntime.quantization import (CalibrationDataReader, QuantFormat,
                                          QuantType, quant_pre_process,
                                          quantize_static)

    import numpy as np  # noqa: F401  (reader closure uses the list only)
    import onnxruntime as ort

    # ATON (ST Edge AI ONNX frontend) rejects DequantizeLinear nodes carrying
    # the per-axis `axis` attribute, which per-channel quantization emits ->
    # per-channel weights only via the TFLite route; QDQ stays per-tensor.
    preprocessed = float_path.replace(".onnx", ".preprocessed.onnx")
    quant_pre_process(float_path, preprocessed, skip_symbolic_shape=True)

    sess = ort.InferenceSession(preprocessed, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    batches = calib_images(calib_dir, size, n_calib)
    reader = _CalibReader(batches, input_name)

    quantize_static(
        preprocessed,
        out_path,
        reader,
        quant_format=QuantFormat.QDQ,
        activation_type=QuantType.QInt8,
        weight_type=QuantType.QInt8,
        per_channel=per_channel,
        reduce_range=False,
        extra_options={"QDQKeepRemovableActivations": True},
    )
    log(f"quantized (QDQ int8, per_channel={per_channel}) -> {out_path}")


def make_uint8_input(qmodel_path):
    """Convert the QDQ model's float input into a raw uint8 input.

    ST Edge AI's ``--input-data-type uint8`` expects the ST zoo pattern:
    graph input (uint8) -> DequantizeLinear -> float body. The onnxruntime
    quantizer keeps a float input, so rewire: input becomes uint8 and a
    DequantizeLinear(scale=1.0, zp=0 uint8) head reproduces the exact
    0..255 float values the calibration assumed."""
    import onnx
    from onnx import helper, TensorProto, numpy_helper
    import numpy as np

    model = onnx.load(qmodel_path)
    graph = model.graph
    inp = graph.input[0]
    inp.type.tensor_type.elem_type = TensorProto.UINT8

    float_name = "embed_input_float"
    for node in graph.node:
        for i, n in enumerate(node.input):
            if n == inp.name:
                node.input[i] = float_name

    dq = helper.make_node(
        "DequantizeLinear",
        [inp.name, "embed_input_scale", "embed_input_zero_point"],
        [float_name],
        name="embed_input_dequant",
    )
    graph.node.insert(0, dq)
    graph.initializer.append(numpy_helper.from_array(
        np.array([1.0], dtype="float32"), "embed_input_scale"))
    graph.initializer.append(numpy_helper.from_array(
        np.array([0], dtype="uint8"), "embed_input_zero_point"))

    onnx.checker.check_model(model, full_check=False)
    onnx.save(model, qmodel_path)
    log("rewired input to raw uint8 (input -> DequantizeLinear -> body)")


def probe_output_spec(quant_path):
    import onnx
    import onnxruntime as ort

    model = onnx.load(quant_path)
    dim = None
    for out in model.graph.output:
        dims = [d.dim_value for d in out.type.tensor_type.shape.dim]
        if dims and all(d > 0 for d in dims):
            flat = 1
            for d in dims:
                flat *= d
            if flat <= 1024 * 16:
                dim = flat
                name = out.name
                break
    if dim is None:
        # dynamic shape: run once
        import numpy as np
        sess = ort.InferenceSession(quant_path, providers=["CPUExecutionProvider"])
        name = sess.get_inputs()[0].name
        dummy = np.zeros((1, 3, DEFAULT_INPUT_SIZE, DEFAULT_INPUT_SIZE), dtype=np.float32)
        out0 = sess.run(None, {name: dummy})[0]
        dim = int(out0.size)
        name = sess.get_outputs()[0].name
    return name, dim


def write_deployment_json(path, model_name, out_dim, size):
    doc = {
        "version": "1.0.0",
        "model_info": {
            "name": f"{model_name} Face Embedding Model (ONNX QDQ Int8, RGB input)",
            "version": "1.0.0",
            "description": (
                f"MobileFaceNet-class face embedding ({out_dim}-d, L2-normalized by "
                "pp_face_embed) for the NE302 dual-model engine: Model A (face detection) "
                "crops faces, this Model B extracts templates matched locally against the "
                "on-device face database. Normalization (x-127.5)/127.5 is embedded in the "
                "graph; input is raw RGB888 uint8. Prepared by Script/prepare_face_embed_model.py."
            ),
            "type": "FACE_EMBEDDING",
            "framework": "ONNX",
            "author": "insightface (w600k_mbf) via NE302 preparation script",
            "source": DEFAULT_URL,
        },
        "input_spec": {
            "width": size,
            "height": size,
            "channels": 3,
            "data_type": "uint8",
            "color_format": "RGB888",
            "normalization": {"enabled": False, "scale": 1.0, "offset": 0.0},
        },
        "output_spec": {
            "num_outputs": 1,
            "outputs": [{
                "name": "embedding",
                "batch": 1,
                "height": 1,
                "width": out_dim,
                "channels": 1,
                "data_type": "int8",
                "scale": 0.0,
                "zero_point": -128,
            }],
        },
        "postprocess_type": "pp_face_embed",
        "confidence_threshold": 0.0,
        "nms_threshold": 0.0,
        "embedding_dim": out_dim,
        "embedding_l2_normalize": True,
        "embedding_output_index": 0,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
    log(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", help="source float ONNX (default: download buffalo_s)")
    ap.add_argument("--url", default=DEFAULT_URL, help="download URL override")
    ap.add_argument("--member", default=DEFAULT_MEMBER, help="zip member to extract")
    ap.add_argument("--size", type=int, default=DEFAULT_INPUT_SIZE, help="input resolution")
    ap.add_argument("--swap-bgr-rgb", action="store_true",
                    help="reverse first-conv input channels (for BGR-trained models)")
    ap.add_argument("--no-normalize", action="store_true",
                    help="source graph already embeds normalization")
    ap.add_argument("--no-verify", action="store_true", help="skip onnxruntime check")
    ap.add_argument("--calib-dir", help="directory of face crop JPEGs for calibration")
    ap.add_argument("--calib-n", type=int, default=64, help="calibration sample count")
    ap.add_argument("--per-channel", action="store_true",
                    help="per-channel weight quantization (NOTE: ATON rejects the "
                         "axis attribute this emits - for TFLite exports only)")
    ap.add_argument("--out-name", default=DEFAULT_OUT_NAME, help="output model base name")
    ap.add_argument("--workdir", default=None, help="scratch dir (default: script dir/_face_embed_work)")
    args = ap.parse_args()

    import onnx

    here = os.path.dirname(os.path.abspath(__file__))
    workdir = args.workdir or os.path.join(here, "_face_embed_work")
    os.makedirs(workdir, exist_ok=True)
    weights_dir = os.path.normpath(os.path.join(here, "..", "Model", "weights"))

    # 1) source
    src = args.input or download_model(args.url, args.member, workdir)

    # 2) surgery on a copy
    model = onnx.load(src)
    onnx.checker.check_model(model, full_check=False)
    if args.swap_bgr_rgb:
        first_conv_weight_swap_to_rgb(model)
    if not args.no_normalize:
        prepend_normalization(model, INPUT_MEAN, INPUT_STD)
    float_mod = os.path.join(workdir, "float_normalized.onnx")
    onnx.save(model, float_mod)

    # 3) verify the surgery is numerically invisible
    if not args.no_verify:
        diff = verify_equivalence(src, float_mod, args.size, args.swap_bgr_rgb,
                                  INPUT_MEAN, INPUT_STD)
        if diff > 1e-3:
            log(f"WARNING: verification diff {diff:.3e} is large - check channel order")

    # 4) quantize + rewire the input to the ST zoo uint8 pattern
    quant_name = f"{args.out_name}.onnx"
    quant_path = os.path.join(weights_dir, quant_name)
    quantize_qdq(float_mod, quant_path, args.size, args.calib_dir, args.calib_n,
                 args.per_channel)
    make_uint8_input(quant_path)

    # 5) deployment json
    _, out_dim = probe_output_spec(quant_path)
    log(f"embedding dimension: {out_dim}")
    write_deployment_json(os.path.join(weights_dir, f"{args.out_name}.json"),
                          args.out_name, out_dim, args.size)

    log("done. Build with:")
    log(f"  make model MODEL_NAME={args.out_name}")
    log("then burn the package to the AI_2 (Model B) slot - see Docs/Face_Recognition_Demo.md")


if __name__ == "__main__":
    sys.exit(main())
