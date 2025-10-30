import ctypes
import subprocess
from pathlib import Path

# Paths
REPO_ROOT = Path(__file__).resolve().parents[1]
BACKEND_DIR = REPO_ROOT / "lib" / "backend"
LIB_PATH = BACKEND_DIR / "libobjbackend.so"

# Ensure the backend library is built so we can load validation helpers
subprocess.run(["make"], cwd=str(BACKEND_DIR), check=True, capture_output=True)

# Constants mirrored from metadata_schema.h
OBJM_PAYLOAD_DESCRIPTOR_VERSION = 1
OBJM_MAX_VARIANTS = 8
OBJM_VARIANT_ID_MAX = 32

OBJM_ENCODING_IDENTITY = 0
OBJM_ENCODING_GZIP = 1
OBJM_ENCODING_BROTLI = 2
OBJM_ENCODING_ZSTD = 3
OBJM_ENCODING_CUSTOM = 255

OBJM_CAP_IDENTITY = 1 << 0
OBJM_CAP_GZIP = 1 << 1
OBJM_CAP_ESI_FLATTENED = 1 << 2
OBJM_CAP_RANGE_READY = 1 << 3
OBJM_CAP_ZERO_COPY = 1 << 4

OBJM_PAYLOAD_FLAG_HAS_VARIANTS = 1 << 0


class ObjmVariantDescriptor(ctypes.Structure):
    _fields_ = [
        ("variant_id", ctypes.c_char * OBJM_VARIANT_ID_MAX),
        ("capabilities", ctypes.c_uint32),
        ("encoding", ctypes.c_uint32),
        ("logical_length", ctypes.c_uint64),
        ("storage_length", ctypes.c_uint64),
        ("range_granularity", ctypes.c_uint64),
        ("is_primary", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 7),
    ]


class ObjmPayloadDescriptor(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("variant_count", ctypes.c_uint32),
        ("manifest_flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("variants", ObjmVariantDescriptor * OBJM_MAX_VARIANTS),
    ]


lib = ctypes.CDLL(str(LIB_PATH))
lib.objm_payload_descriptor_init.argtypes = [ctypes.POINTER(ObjmPayloadDescriptor)]
lib.objm_payload_descriptor_init.restype = None
lib.objm_payload_descriptor_validate.argtypes = [
    ctypes.POINTER(ObjmPayloadDescriptor),
    ctypes.POINTER(ctypes.c_char),
    ctypes.c_size_t,
]
lib.objm_payload_descriptor_validate.restype = ctypes.c_int


def make_variant(
    variant_id: str,
    *,
    capabilities: int,
    encoding: int,
    logical_length: int,
    storage_length: int,
    primary: bool = False,
    range_granularity: int = 0,
) -> ObjmVariantDescriptor:
    variant = ObjmVariantDescriptor()
    variant.variant_id = variant_id.encode("ascii")
    variant.capabilities = capabilities
    variant.encoding = encoding
    variant.logical_length = logical_length
    variant.storage_length = storage_length
    variant.range_granularity = range_granularity
    variant.is_primary = 1 if primary else 0
    return variant


def validate(descriptor: ObjmPayloadDescriptor) -> tuple[int, str]:
    err_buf = ctypes.create_string_buffer(256)
    rc = lib.objm_payload_descriptor_validate(
        ctypes.byref(descriptor),
        err_buf,
        ctypes.sizeof(err_buf),
    )
    return rc, err_buf.value.decode("ascii", errors="ignore")


def test_valid_descriptor_accepts_identity_and_gzip_variants():
    descriptor = ObjmPayloadDescriptor()
    lib.objm_payload_descriptor_init(ctypes.byref(descriptor))
    descriptor.manifest_flags = OBJM_PAYLOAD_FLAG_HAS_VARIANTS
    descriptor.variant_count = 2
    descriptor.variants[0] = make_variant(
        "identity",
        capabilities=OBJM_CAP_IDENTITY | OBJM_CAP_ZERO_COPY,
        encoding=OBJM_ENCODING_IDENTITY,
        logical_length=1024,
        storage_length=1024,
        primary=True,
    )
    descriptor.variants[1] = make_variant(
        "gzip",
        capabilities=OBJM_CAP_GZIP | OBJM_CAP_ZERO_COPY,
        encoding=OBJM_ENCODING_GZIP,
        logical_length=1024,
        storage_length=768,
    )

    rc, err = validate(descriptor)
    assert rc == 0, err


def test_descriptor_requires_single_primary_variant():
    descriptor = ObjmPayloadDescriptor()
    lib.objm_payload_descriptor_init(ctypes.byref(descriptor))
    descriptor.variant_count = 1
    descriptor.variants[0] = make_variant(
        "identity",
        capabilities=OBJM_CAP_IDENTITY,
        encoding=OBJM_ENCODING_IDENTITY,
        logical_length=512,
        storage_length=512,
        primary=False,
    )

    rc, err = validate(descriptor)
    assert rc == -1
    assert "no primary" in err


def test_range_capability_requires_granularity():
    descriptor = ObjmPayloadDescriptor()
    lib.objm_payload_descriptor_init(ctypes.byref(descriptor))
    descriptor.variant_count = 1
    descriptor.variants[0] = make_variant(
        "range",
        capabilities=OBJM_CAP_IDENTITY | OBJM_CAP_RANGE_READY,
        encoding=OBJM_ENCODING_IDENTITY,
        logical_length=2048,
        storage_length=2048,
        primary=True,
        range_granularity=0,
    )

    rc, err = validate(descriptor)
    assert rc == -1
    assert "range-ready" in err
