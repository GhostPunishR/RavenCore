#!/usr/bin/env python3
"""Exécute une suite GB/GBC externe déclarée par un manifeste strict.

Le script ne télécharge et ne copie aucun artefact. Les chemins restent sous
la racine fournie, et chaque ROM/boot ROM est identifiée par son SHA-256 avant
que le runner natif ne soit lancé.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any
from urllib.parse import urlparse
import zlib


SCHEMA_VERSION = 1
CATEGORIES = frozenset(
    {
        "cpu",
        "timing",
        "interrupts",
        "timer",
        "halt",
        "ei",
        "stop",
        "memory",
        "oam-dma",
        "vram-dma",
        "ppu",
        "stat",
        "palettes",
        "speed-switch",
        "serial",
        "apu",
        "mbc",
    }
)
HARDWARE_MODES = frozenset({"auto", "dmg", "cgb", "agb"})
REDISTRIBUTION_POLICIES = frozenset({"external-only", "redistributable"})
AMBIGUOUS_LICENSES = frozenset(
    {"", "unknown", "inconnu", "inconnue", "unspecified", "n/a", "none", "todo"}
)
IDENTIFIER_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
SHA256_RE = re.compile(r"[0-9a-fA-F]{64}\Z")


class ManifestError(ValueError):
    """Le manifeste ne satisfait pas le contrat versionné."""


SCREEN_WIDTH = 160
SCREEN_HEIGHT = 144
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def decode_png_rgb(data: bytes) -> bytes:
    """Décode une capture PNG 160 × 144 non entrelacée en RGB 8 bits par canal.

    Couvre les types de couleur du PNG (gris, RGB, palette, avec ou sans
    alpha, 1 à 16 bits) ; l'alpha est ignoré. La décompression est bornée à la
    taille attendue d'une image d'écran.
    """
    if not data.startswith(PNG_SIGNATURE):
        raise ManifestError("capture: signature PNG absente")
    position = len(PNG_SIGNATURE)
    header: tuple[int, ...] | None = None
    palette = b""
    compressed = bytearray()
    while position + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[position : position + 8])
        body = data[position + 8 : position + 8 + length]
        if len(body) != length:
            raise ManifestError("capture: chunk PNG tronqué")
        position += 12 + length
        if kind == b"IHDR":
            if length != 13:
                raise ManifestError("capture: en-tête PNG invalide")
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ManifestError("capture: en-tête PNG absent")
    width, height, depth, color_type, compression, filtering, interlace = header
    if (width, height) != (SCREEN_WIDTH, SCREEN_HEIGHT):
        raise ManifestError(f"capture: {width}x{height} au lieu de 160x144")
    if color_type not in PNG_CHANNELS or compression or filtering or interlace:
        raise ManifestError("capture: format PNG non pris en charge")
    allowed_depths = {3: (1, 2, 4, 8), 0: (1, 2, 4, 8, 16)}.get(color_type, (8, 16))
    if depth not in allowed_depths:
        raise ManifestError("capture: profondeur PNG invalide")
    channels = PNG_CHANNELS[color_type]
    bits_per_pixel = channels * depth
    row_bytes = (width * bits_per_pixel + 7) // 8
    expected = height * (row_bytes + 1)
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(bytes(compressed), expected + 1)
    except zlib.error as error:
        raise ManifestError("capture: données PNG corrompues") from error
    if len(raw) != expected:
        raise ManifestError("capture: taille des données PNG inattendue")

    step = max(1, bits_per_pixel // 8)
    previous = bytearray(row_bytes)
    rgb = bytearray()
    for row in range(height):
        start = row * (row_bytes + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1 : start + 1 + row_bytes])
        for index in range(row_bytes):
            left = line[index - step] if index >= step else 0
            up = previous[index]
            corner = previous[index - step] if index >= step else 0
            if kind == 1:
                predictor = left
            elif kind == 2:
                predictor = up
            elif kind == 3:
                predictor = (left + up) // 2
            elif kind == 4:
                estimate = left + up - corner
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - corner))
                predictor = (left, up, corner)[distances.index(min(distances))]
            elif kind == 0:
                predictor = 0
            else:
                raise ManifestError("capture: filtre PNG inconnu")
            line[index] = (line[index] + predictor) & 0xFF
        previous = line
        if depth < 8:
            mask = (1 << depth) - 1
            samples = [
                (line[(column * depth) // 8] >> (8 - depth - (column * depth) % 8)) & mask
                for column in range(width)
            ]
        elif depth == 8:
            samples = list(line)
        else:
            samples = list(line[::2])
        for column in range(width):
            if color_type == 3:
                index = samples[column]
                if 3 * index + 3 > len(palette):
                    raise ManifestError("capture: index de palette PNG hors limites")
                rgb += palette[3 * index : 3 * index + 3]
            elif color_type in (0, 4):
                value = samples[column * channels]
                if depth < 8:
                    value = value * 255 // ((1 << depth) - 1)
                rgb += bytes((value, value, value))
            else:
                rgb += bytes(samples[column * channels : column * channels + 3])
    return bytes(rgb)


def _object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ManifestError(f"{context}: objet JSON attendu")
    return value


def _only_keys(value: dict[str, Any], allowed: set[str], context: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise ManifestError(f"{context}: champs inconnus: {', '.join(unknown)}")


def _required(value: dict[str, Any], names: set[str], context: str) -> None:
    missing = sorted(names - set(value))
    if missing:
        raise ManifestError(f"{context}: champs requis absents: {', '.join(missing)}")


def _string(value: Any, context: str, *, maximum: int = 4096) -> str:
    if not isinstance(value, str) or not value or "\x00" in value or len(value) > maximum:
        raise ManifestError(f"{context}: chaîne non vide invalide")
    return value


def _integer(value: Any, context: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ManifestError(f"{context}: entier attendu entre {minimum} et {maximum}")
    return value


def _number(value: Any, context: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool):
        raise ManifestError(f"{context}: adresse/valeur invalide")
    if isinstance(value, int):
        result = value
    elif isinstance(value, str):
        try:
            result = int(value, 16 if value.lower().startswith("0x") else 10)
        except ValueError as error:
            raise ManifestError(f"{context}: adresse/valeur invalide") from error
    else:
        raise ManifestError(f"{context}: adresse/valeur invalide")
    if not minimum <= result <= maximum:
        raise ManifestError(f"{context}: valeur hors limites {minimum}..{maximum}")
    return result


def _sha256(value: Any, context: str) -> str:
    if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
        raise ManifestError(f"{context}: SHA-256 attendu sur 64 chiffres hexadécimaux")
    return value.lower()


def _relative_path(value: Any, context: str) -> str:
    text = _string(value, context, maximum=1024)
    if "\\" in text or re.match(r"^[A-Za-z]:", text):
        raise ManifestError(f"{context}: utiliser un chemin POSIX relatif")
    path = PurePosixPath(text)
    if path.is_absolute() or path == PurePosixPath(".") or ".." in path.parts:
        raise ManifestError(f"{context}: chemin absolu ou traversée interdits")
    return path.as_posix()


def _artifact(value: Any, context: str) -> dict[str, str]:
    result = _object(value, context)
    allowed = {"path", "sha256", "source", "license", "redistribution"}
    _only_keys(result, allowed, context)
    _required(result, allowed, context)
    source = _string(result["source"], f"{context}.source", maximum=2048)
    parsed_source = urlparse(source)
    if parsed_source.scheme != "https" or not parsed_source.netloc:
        raise ManifestError(f"{context}.source: URL HTTPS d'origine requise")
    license_name = _string(result["license"], f"{context}.license", maximum=256)
    if license_name.strip().casefold() in AMBIGUOUS_LICENSES:
        raise ManifestError(f"{context}.license: licence explicite requise")
    redistribution = _string(
        result["redistribution"], f"{context}.redistribution", maximum=32
    )
    if redistribution not in REDISTRIBUTION_POLICIES:
        raise ManifestError(
            f"{context}.redistribution: external-only ou redistributable attendu"
        )
    return {
        "path": _relative_path(result["path"], f"{context}.path"),
        "sha256": _sha256(result["sha256"], f"{context}.sha256"),
        "source": source,
        "license": license_name,
        "redistribution": redistribution,
    }


def _success(value: Any, context: str, timeout_frames: int) -> dict[str, Any]:
    result = _object(value, context)
    allowed = {
        "serial",
        "register_signature",
        "memory",
        "memory_before",
        "framebuffer",
        "framebuffer_sha256",
        "screenshot",
        "minimum_pass_frames",
        "memory_stability_samples",
    }
    _only_keys(result, allowed, context)

    serial_pass = ""
    serial_fail = ""
    if "serial" in result:
        serial = _object(result["serial"], f"{context}.serial")
        _only_keys(serial, {"pass", "fail"}, f"{context}.serial")
        if "pass" in serial:
            serial_pass = _string(serial["pass"], f"{context}.serial.pass")
        if "fail" in serial:
            serial_fail = _string(serial["fail"], f"{context}.serial.fail")

    register_signature = result.get("register_signature", False)
    if not isinstance(register_signature, bool):
        raise ManifestError(f"{context}.register_signature: booléen attendu")

    def memory_list(key: str) -> list[dict[str, int]]:
        expectations: list[dict[str, int]] = []
        seen_addresses: set[int] = set()
        raw_memory = result[key]
        if not isinstance(raw_memory, list) or not raw_memory:
            raise ManifestError(f"{context}.{key}: liste non vide attendue")
        for index, raw_expectation in enumerate(raw_memory):
            item_context = f"{context}.{key}[{index}]"
            item = _object(raw_expectation, item_context)
            _only_keys(item, {"address", "value"}, item_context)
            _required(item, {"address", "value"}, item_context)
            address = _number(item["address"], f"{item_context}.address", 0, 0xFFFF)
            expected = _number(item["value"], f"{item_context}.value", 0, 0xFF)
            if address in seen_addresses:
                raise ManifestError(f"{item_context}: adresse mémoire dupliquée")
            seen_addresses.add(address)
            expectations.append({"address": address, "value": expected})
        return expectations

    memory = memory_list("memory") if "memory" in result else []
    # État à observer avant d'accepter la réussite mémoire (par exemple
    # $A000 = $80, « test en cours », pour les ROMs de Blargg).
    memory_before = memory_list("memory_before") if "memory_before" in result else []
    if memory_before and not memory:
        raise ManifestError(f"{context}.memory_before: exige une réussite mémoire")

    framebuffer_artifact: dict[str, str] | None = None
    if "framebuffer" in result:
        framebuffer_artifact = _artifact(
            result["framebuffer"], f"{context}.framebuffer"
        )

    screenshot: dict[str, str] | None = None
    if "screenshot" in result:
        screenshot = _artifact(result["screenshot"], f"{context}.screenshot")

    framebuffer = ""
    if "framebuffer_sha256" in result:
        framebuffer = _sha256(
            result["framebuffer_sha256"], f"{context}.framebuffer_sha256"
        )

    minimum_frames = _integer(
        result.get("minimum_pass_frames", 0),
        f"{context}.minimum_pass_frames",
        0,
        timeout_frames - 1,
    )
    stability_present = "memory_stability_samples" in result
    memory_stability = _integer(
        result.get("memory_stability_samples", 1),
        f"{context}.memory_stability_samples",
        1,
        1_000_000,
    )
    if memory and (not stability_present or memory_stability < 2):
        raise ManifestError(
            f"{context}: une réussite mémoire exige "
            "memory_stability_samples >= 2"
        )
    if (framebuffer_artifact is not None) + bool(framebuffer) + (screenshot is not None) > 1:
        raise ManifestError(
            f"{context}: choisir une seule image attendue "
            "(framebuffer, framebuffer_sha256 ou screenshot)"
        )
    if not (
        serial_pass
        or register_signature
        or memory
        or framebuffer_artifact is not None
        or framebuffer
        or screenshot is not None
    ):
        raise ManifestError(f"{context}: aucun mécanisme de réussite déclaré")

    return {
        "serial_pass": serial_pass,
        "serial_fail": serial_fail,
        "register_signature": register_signature,
        "memory": memory,
        "memory_before": memory_before,
        "framebuffer": framebuffer_artifact,
        "framebuffer_sha256": framebuffer,
        "screenshot": screenshot,
        "minimum_pass_frames": minimum_frames,
        "memory_stability_samples": memory_stability,
    }


def validate_manifest(raw: Any) -> dict[str, Any]:
    manifest = _object(raw, "manifest")
    _only_keys(manifest, {"schema_version", "suite", "tests"}, "manifest")
    _required(manifest, {"schema_version", "suite", "tests"}, "manifest")
    if manifest["schema_version"] != SCHEMA_VERSION:
        raise ManifestError(
            f"manifest.schema_version: version {SCHEMA_VERSION} requise, "
            f"reçu {manifest['schema_version']!r}"
        )

    suite = _object(manifest["suite"], "manifest.suite")
    _only_keys(suite, {"name", "description"}, "manifest.suite")
    _required(suite, {"name"}, "manifest.suite")
    normalized_suite: dict[str, str] = {
        "name": _string(suite["name"], "manifest.suite.name", maximum=256)
    }
    if "description" in suite:
        normalized_suite["description"] = _string(
            suite["description"], "manifest.suite.description", maximum=4096
        )

    tests = manifest["tests"]
    if not isinstance(tests, list) or not tests or len(tests) > 10_000:
        raise ManifestError("manifest.tests: liste de 1 à 10000 tests attendue")
    normalized_tests: list[dict[str, Any]] = []
    identifiers: set[str] = set()
    required = {"id", "category", "hardware", "rom", "timeout_frames", "success"}
    allowed = required | {"boot_rom"}
    for index, raw_test in enumerate(tests):
        context = f"manifest.tests[{index}]"
        test = _object(raw_test, context)
        _only_keys(test, allowed, context)
        _required(test, required, context)
        identifier = _string(test["id"], f"{context}.id", maximum=128)
        if IDENTIFIER_RE.fullmatch(identifier) is None:
            raise ManifestError(f"{context}.id: identifiant portable invalide")
        if identifier in identifiers:
            raise ManifestError(f"{context}.id: identifiant dupliqué {identifier}")
        identifiers.add(identifier)

        category = _string(test["category"], f"{context}.category", maximum=32)
        if category not in CATEGORIES:
            raise ManifestError(f"{context}.category: catégorie inconnue {category}")
        hardware = _string(test["hardware"], f"{context}.hardware", maximum=8)
        if hardware not in HARDWARE_MODES:
            raise ManifestError(f"{context}.hardware: auto, dmg, cgb ou agb attendu")
        timeout_frames = _integer(
            test["timeout_frames"], f"{context}.timeout_frames", 1, 1_000_000
        )
        normalized: dict[str, Any] = {
            "id": identifier,
            "category": category,
            "hardware": hardware,
            "rom": _artifact(test["rom"], f"{context}.rom"),
            "timeout_frames": timeout_frames,
            "success": _success(test["success"], f"{context}.success", timeout_frames),
        }
        if "boot_rom" in test:
            normalized["boot_rom"] = _artifact(
                test["boot_rom"], f"{context}.boot_rom"
            )
        normalized_tests.append(normalized)

    return {
        "schema_version": SCHEMA_VERSION,
        "suite": normalized_suite,
        "tests": normalized_tests,
    }


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            return validate_manifest(json.load(stream))
    except json.JSONDecodeError as error:
        raise ManifestError(
            f"{path}: JSON invalide ligne {error.lineno}, colonne {error.colno}"
        ) from error
    except OSError as error:
        raise ManifestError(f"impossible de lire {path}: {error}") from error


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve_artifact(root: Path, artifact: dict[str, str]) -> Path:
    candidate = (root / artifact["path"]).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ManifestError(
            f"{artifact['path']}: le chemin résolu sort de la racine des artefacts"
        ) from error
    return candidate


def runner_command(
    runner: Path,
    case: dict[str, Any],
    rom_path: Path,
    boot_path: Path | None,
    framebuffer_path: Path | None,
    screenshot_rgb_path: Path | None = None,
) -> list[str]:
    success = case["success"]
    command = [
        str(runner),
        str(rom_path),
        "--hardware",
        case["hardware"],
        "--category",
        case["category"],
        "--timeout-frames",
        str(case["timeout_frames"]),
        "--minimum-pass-frames",
        str(success["minimum_pass_frames"]),
        "--memory-stability-samples",
        str(success["memory_stability_samples"]),
        "--serial-pass",
        success["serial_pass"],
        "--serial-fail",
        success["serial_fail"],
    ]
    if boot_path is not None:
        command.extend(("--boot-rom", str(boot_path)))
    if framebuffer_path is not None:
        command.extend(("--expect-frame-argb8888", str(framebuffer_path)))
    if screenshot_rgb_path is not None:
        # Les captures de référence supposent, en mode compatibilité CGB, les
        # palettes que le boot ROM donne à une cartouche inconnue.
        command.extend(
            (
                "--expect-frame-rgb888",
                str(screenshot_rgb_path),
                "--compatibility-palette",
                "boot-default",
            )
        )
    if not success["register_signature"]:
        command.append("--no-mooneye-signature")
    for expectation in success["memory"]:
        command.extend(
            (
                "--expect-memory",
                f"0x{expectation['address']:04x}=0x{expectation['value']:02x}",
            )
        )
    for expectation in success["memory_before"]:
        command.extend(
            (
                "--expect-memory-before",
                f"0x{expectation['address']:04x}=0x{expectation['value']:02x}",
            )
        )
    if success["memory"]:
        command.append("--memory-require-mismatch")
    if success["framebuffer_sha256"]:
        command.extend(("--expect-frame-sha256", success["framebuffer_sha256"]))
    return command


def _artifact_error(
    artifact: dict[str, str],
    path: Path,
    kind: str,
    allow_missing: bool,
) -> tuple[str, str] | None:
    if not path.is_file():
        if allow_missing:
            return "skipped", f"{kind}-missing"
        return "error", f"{kind}-missing"
    try:
        actual = file_sha256(path)
    except OSError:
        return "error", f"{kind}-unreadable"
    if actual != artifact["sha256"]:
        return "error", f"{kind}-sha256-mismatch"
    return None


def execute_case(
    runner: Path,
    root: Path,
    case: dict[str, Any],
    allow_missing: bool,
    wall_timeout_seconds: float,
) -> dict[str, Any]:
    started = time.monotonic()
    result: dict[str, Any] = {
        "id": case["id"],
        "category": case["category"],
        "hardware": case["hardware"],
        "rom": {
            "path": case["rom"]["path"],
            "sha256": case["rom"]["sha256"],
            "source": case["rom"]["source"],
            "license": case["rom"]["license"],
            "redistribution": case["rom"]["redistribution"],
        },
        "timeout_frames": case["timeout_frames"],
        "minimum_pass_frames": case["success"]["minimum_pass_frames"],
        "memory_stability_samples": case["success"]["memory_stability_samples"],
    }
    try:
        rom_path = resolve_artifact(root, case["rom"])
        problem = _artifact_error(case["rom"], rom_path, "rom", allow_missing)
        if problem is not None:
            result["status"], result["reason"] = problem
            return result

        boot_path: Path | None = None
        if "boot_rom" in case:
            boot_path = resolve_artifact(root, case["boot_rom"])
            result["boot_rom"] = {
                "path": case["boot_rom"]["path"],
                "sha256": case["boot_rom"]["sha256"],
                "source": case["boot_rom"]["source"],
                "license": case["boot_rom"]["license"],
                "redistribution": case["boot_rom"]["redistribution"],
            }
            problem = _artifact_error(
                case["boot_rom"], boot_path, "boot-rom", allow_missing
            )
            if problem is not None:
                result["status"], result["reason"] = problem
                return result

        framebuffer_path: Path | None = None
        framebuffer = case["success"]["framebuffer"]
        if framebuffer is not None:
            framebuffer_path = resolve_artifact(root, framebuffer)
            result["framebuffer"] = {
                "path": framebuffer["path"],
                "sha256": framebuffer["sha256"],
                "source": framebuffer["source"],
                "license": framebuffer["license"],
                "redistribution": framebuffer["redistribution"],
            }
            problem = _artifact_error(
                framebuffer, framebuffer_path, "framebuffer", allow_missing
            )
            if problem is not None:
                result["status"], result["reason"] = problem
                return result

        screenshot = case["success"]["screenshot"]
        with tempfile.TemporaryDirectory(prefix="ravenemu-screenshot-") as scratch:
            screenshot_rgb_path: Path | None = None
            if screenshot is not None:
                screenshot_path = resolve_artifact(root, screenshot)
                result["screenshot"] = {
                    "path": screenshot["path"],
                    "sha256": screenshot["sha256"],
                    "source": screenshot["source"],
                    "license": screenshot["license"],
                    "redistribution": screenshot["redistribution"],
                }
                problem = _artifact_error(
                    screenshot, screenshot_path, "screenshot", allow_missing
                )
                if problem is not None:
                    result["status"], result["reason"] = problem
                    return result
                screenshot_rgb_path = Path(scratch) / "screenshot.rgb888"
                screenshot_rgb_path.write_bytes(
                    decode_png_rgb(screenshot_path.read_bytes())
                )
            command = runner_command(
                runner, case, rom_path, boot_path, framebuffer_path, screenshot_rgb_path
            )
            completed = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=wall_timeout_seconds,
            )
        result["runner_exit_code"] = completed.returncode
        result["stdout"] = completed.stdout
        result["stderr"] = completed.stderr
        if completed.returncode == 0:
            result["status"] = "pass"
            result["reason"] = "runner-pass"
        elif completed.returncode == 1:
            timed_out = any(
                line.startswith("TIMEOUT ") for line in completed.stdout.splitlines()
            )
            result["status"] = "timeout" if timed_out else "fail"
            result["reason"] = "emulated-timeout" if timed_out else "runner-failure"
        elif completed.returncode == 2:
            result["status"] = "error"
            result["reason"] = "runner-error"
        elif completed.returncode < 0:
            result["status"] = "crash"
            result["reason"] = f"runner-signal-{-completed.returncode}"
        else:
            result["status"] = "error"
            result["reason"] = f"runner-unexpected-exit-{completed.returncode}"
    except subprocess.TimeoutExpired as error:
        result["status"] = "timeout"
        result["reason"] = "host-wall-timeout"
        result["stdout"] = (
            error.stdout.decode("utf-8", errors="replace")
            if isinstance(error.stdout, bytes)
            else error.stdout or ""
        )
        result["stderr"] = (
            error.stderr.decode("utf-8", errors="replace")
            if isinstance(error.stderr, bytes)
            else error.stderr or ""
        )
    except (ManifestError, OSError, RuntimeError) as error:
        result["status"] = "error"
        result["reason"] = "orchestration-error"
        result["error"] = str(error)
    finally:
        result["duration_seconds"] = round(time.monotonic() - started, 6)
    return result


def write_report(path: Path, report: dict[str, Any]) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def run_manifest(
    manifest_path: Path,
    runner: Path,
    root: Path,
    report_path: Path,
    categories: set[str],
    allow_missing: bool,
    wall_timeout_seconds: float,
    identifiers: set[str] | None = None,
) -> int:
    manifest = load_manifest(manifest_path)
    runner = runner.resolve()
    root = root.resolve()
    if not runner.is_file():
        raise ManifestError(f"runner natif absent: {runner}")
    if not root.is_dir():
        raise ManifestError(f"racine des artefacts absente: {root}")

    if identifiers:
        unknown = identifiers - {case["id"] for case in manifest["tests"]}
        if unknown:
            raise ManifestError(f"identifiants absents du manifeste: {', '.join(sorted(unknown))}")
    selected = [
        case
        for case in manifest["tests"]
        if (not categories or case["category"] in categories)
        and (not identifiers or case["id"] in identifiers)
    ]
    if not selected:
        raise ManifestError("aucun test ne correspond aux filtres")

    started = time.time()
    results: list[dict[str, Any]] = []
    for case in selected:
        result = execute_case(
            runner, root, case, allow_missing, wall_timeout_seconds
        )
        results.append(result)
        print(
            f"{result['status'].upper()} id={result['id']} "
            f"category={result['category']} reason={result['reason']}"
        )
        if result["status"] != "pass":
            # Le verdict détaillé du runner (registres, image, mémoire) reste
            # dans le rapport ; il est aussi recopié ici pour les journaux de CI.
            for line in (result.get("stdout", "") + result.get("stderr", "")).splitlines():
                print(f"  {line}")
            if "error" in result:
                print(f"  {result['error']}")

    statuses = ("pass", "fail", "timeout", "error", "crash", "skipped")
    summary = {status: sum(item["status"] == status for item in results) for status in statuses}
    summary["total"] = len(results)
    overall_pass = not any(summary[status] for status in ("fail", "timeout", "error", "crash"))
    report = {
        "schema_version": SCHEMA_VERSION,
        "suite": manifest["suite"],
        "manifest": str(manifest_path.resolve()),
        "artifact_root": str(root),
        "selected_categories": sorted(categories),
        "allow_missing": allow_missing,
        "started_unix_seconds": started,
        "duration_seconds": round(time.time() - started, 6),
        "result": "pass" if overall_pass else "fail",
        "summary": summary,
        "tests": results,
    }
    write_report(report_path, report)
    print(
        "SUMMARY "
        + " ".join(f"{name}={summary[name]}" for name in (*statuses, "total"))
        + f" report={report_path}"
    )
    return 0 if overall_pass else 1


def list_manifest(manifest_path: Path, categories: set[str]) -> int:
    manifest = load_manifest(manifest_path)
    selected = [
        case for case in manifest["tests"] if not categories or case["category"] in categories
    ]
    if not selected:
        raise ManifestError("aucun test ne correspond au filtre de catégories")
    for case in selected:
        print(
            f"{case['id']}\t{case['category']}\t{case['hardware']}\t{case['rom']['path']}"
        )
    return 0


def make_self_test_rom() -> bytes:
    rom = bytearray(0x8000)
    rom[0x0100:0x0103] = bytes((0xC3, 0x50, 0x01))
    cursor = 0x0150
    marker = bytes((0x3E, 0x42, 0xEA, 0x00, 0xC0))
    rom[cursor : cursor + len(marker)] = marker
    cursor += len(marker)
    for character in b"Passed":
        send = bytes(
            (
                0x3E,
                character,
                0xE0,
                0x01,
                0x3E,
                0x81,
                0xE0,
                0x02,
                0xF0,
                0x02,
                0xCB,
                0x7F,
                0x20,
                0xFA,
            )
        )
        rom[cursor : cursor + len(send)] = send
        cursor += len(send)
    rom[cursor : cursor + 2] = bytes((0x18, 0xFE))
    rom[0x0147] = 0x00
    return bytes(rom)


def encode_gray_png(width: int, height: int, value: int) -> bytes:
    """PNG gris 8 bits uni, pour l'auto-test du décodeur et des captures."""

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    rows = b"".join(b"\x00" + bytes((value,)) * width for _ in range(height))
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def self_test(runner: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="ravenemu-conformance-") as directory:
        root = Path(directory)
        rom_path = root / "synthetic.gb"
        rom_bytes = make_self_test_rom()
        rom_path.write_bytes(rom_bytes)
        digest = hashlib.sha256(rom_bytes).hexdigest()
        framebuffer_path = root / "initial-frame.argb8888"
        framebuffer_bytes = bytes(160 * 144 * 4)
        framebuffer_path.write_bytes(framebuffer_bytes)
        provenance = {
            "path": rom_path.name,
            "sha256": digest,
            "source": "https://github.com/GhostPunishR/RavenEmu",
            "license": "MIT",
            "redistribution": "redistributable",
        }
        framebuffer_provenance = {
            "path": framebuffer_path.name,
            "sha256": hashlib.sha256(framebuffer_bytes).hexdigest(),
            "source": "https://github.com/GhostPunishR/RavenEmu",
            "license": "MIT",
            "redistribution": "redistributable",
        }
        # Première image DMG : le gris 0, blanc dans l'espace des captures.
        screenshots = {}
        for name, value in (("white", 0xFF), ("black", 0x00)):
            png = encode_gray_png(160, 144, value)
            (root / f"{name}.png").write_bytes(png)
            screenshots[name] = {
                "path": f"{name}.png",
                "sha256": hashlib.sha256(png).hexdigest(),
                "source": "https://github.com/GhostPunishR/RavenEmu",
                "license": "MIT",
                "redistribution": "redistributable",
            }
        if decode_png_rgb((root / "white.png").read_bytes()) != b"\xff" * (160 * 144 * 3):
            raise RuntimeError("le décodeur PNG a mal lu une capture grise")
        raw_manifest = {
            "schema_version": SCHEMA_VERSION,
            "suite": {"name": "RavenEmu synthetic manifest self-test"},
            "tests": [
                {
                    "id": "serial-pass",
                    "category": "serial",
                    "hardware": "dmg",
                    "rom": provenance,
                    "timeout_frames": 2,
                    "success": {
                        "serial": {"pass": "Passed", "fail": "Failed"},
                        "register_signature": False,
                    },
                },
                {
                    "id": "memory-pass-after-one-frame",
                    "category": "memory",
                    "hardware": "dmg",
                    "rom": provenance,
                    "timeout_frames": 2,
                    "success": {
                        "serial": {"fail": "Failed"},
                        "register_signature": False,
                        "memory": [{"address": "0xC000", "value": "0x42"}],
                        "minimum_pass_frames": 1,
                        "memory_stability_samples": 8,
                    },
                },
                {
                    "id": "exact-framebuffer-pass",
                    "category": "ppu",
                    "hardware": "dmg",
                    "rom": provenance,
                    "timeout_frames": 2,
                    "success": {
                        "serial": {"fail": "Failed"},
                        "register_signature": False,
                        "framebuffer": framebuffer_provenance,
                        "minimum_pass_frames": 1,
                    },
                },
                {
                    "id": "screenshot-pass",
                    "category": "ppu",
                    "hardware": "dmg",
                    "rom": provenance,
                    "timeout_frames": 2,
                    "success": {
                        "serial": {"fail": "Failed"},
                        "register_signature": False,
                        "screenshot": screenshots["white"],
                        "minimum_pass_frames": 1,
                    },
                },
            ],
        }
        manifest_path = root / "manifest.json"
        manifest_path.write_text(
            json.dumps(raw_manifest, ensure_ascii=False), encoding="utf-8"
        )
        report_path = root / "report.json"
        exit_code = run_manifest(
            manifest_path,
            runner,
            root,
            report_path,
            set(),
            False,
            30.0,
        )
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if exit_code != 0 or report["summary"]["pass"] != 4:
            raise RuntimeError("le manifeste synthétique n'a pas produit quatre réussites")

        selected_report = root / "selected-report.json"
        run_manifest(
            manifest_path, runner, root, selected_report, set(), False, 30.0, {"screenshot-pass"}
        )
        selected = json.loads(selected_report.read_text(encoding="utf-8"))
        if [item["id"] for item in selected["tests"]] != ["screenshot-pass"]:
            raise RuntimeError("--id n'a pas sélectionné le seul test demandé")

        wrong_screen = json.loads(json.dumps(raw_manifest))
        wrong_screen["tests"] = [wrong_screen["tests"][3]]
        wrong_screen["tests"][0]["success"]["screenshot"] = screenshots["black"]
        wrong_screen["tests"][0]["success"]["serial"] = {"fail": "Failed"}
        wrong_path = root / "wrong-screenshot.json"
        wrong_path.write_text(json.dumps(wrong_screen), encoding="utf-8")
        wrong_report = root / "wrong-screenshot-report.json"
        wrong_exit = run_manifest(
            wrong_path, runner, root, wrong_report, set(), False, 30.0
        )
        wrong_data = json.loads(wrong_report.read_text(encoding="utf-8"))
        if wrong_exit == 0 or wrong_data["tests"][0]["status"] != "timeout":
            raise RuntimeError("une capture différente de l'écran a produit une réussite")

        bad_manifest = json.loads(json.dumps(raw_manifest))
        bad_manifest["tests"][0]["rom"]["sha256"] = "0" * 64
        bad_manifest["tests"] = bad_manifest["tests"][:1]
        bad_path = root / "bad-hash.json"
        bad_path.write_text(json.dumps(bad_manifest), encoding="utf-8")
        bad_report = root / "bad-hash-report.json"
        bad_exit = run_manifest(
            bad_path, runner, root, bad_report, set(), False, 30.0
        )
        bad_data = json.loads(bad_report.read_text(encoding="utf-8"))
        if bad_exit == 0 or bad_data["tests"][0]["reason"] != "rom-sha256-mismatch":
            raise RuntimeError("une ROM de SHA-256 incorrect a été exécutée")

        missing_manifest = json.loads(json.dumps(raw_manifest))
        missing_manifest["tests"] = missing_manifest["tests"][:1]
        missing_manifest["tests"][0]["rom"]["path"] = "absent.gb"
        missing_path = root / "missing.json"
        missing_path.write_text(json.dumps(missing_manifest), encoding="utf-8")
        missing_report = root / "missing-report.json"
        missing_exit = run_manifest(
            missing_path, runner, root, missing_report, set(), True, 30.0
        )
        missing_data = json.loads(missing_report.read_text(encoding="utf-8"))
        if missing_exit != 0 or missing_data["tests"][0]["status"] != "skipped":
            raise RuntimeError("--allow-missing n'a pas produit un skip explicite")

        unchanged_manifest = json.loads(json.dumps(raw_manifest))
        unchanged_manifest["tests"] = [unchanged_manifest["tests"][1]]
        unchanged_manifest["tests"][0]["id"] = "unchanged-initial-memory"
        unchanged_manifest["tests"][0]["timeout_frames"] = 1
        unchanged_manifest["tests"][0]["success"]["minimum_pass_frames"] = 0
        unchanged_manifest["tests"][0]["success"]["memory"] = [
            {"address": "0xC001", "value": "0x00"}
        ]
        unchanged_path = root / "unchanged-memory.json"
        unchanged_path.write_text(json.dumps(unchanged_manifest), encoding="utf-8")
        unchanged_report = root / "unchanged-memory-report.json"
        unchanged_exit = run_manifest(
            unchanged_path, runner, root, unchanged_report, set(), False, 30.0
        )
        unchanged_data = json.loads(unchanged_report.read_text(encoding="utf-8"))
        if unchanged_exit == 0 or unchanged_data["tests"][0]["status"] != "timeout":
            raise RuntimeError("la valeur mémoire initiale inchangée a produit une réussite")

        never_running = json.loads(json.dumps(raw_manifest))
        never_running["tests"] = [never_running["tests"][1]]
        never_running["tests"][0]["id"] = "memory-before-never-observed"
        never_running["tests"][0]["success"]["memory_before"] = [
            {"address": "0xC000", "value": "0x80"}
        ]
        never_path = root / "memory-before.json"
        never_path.write_text(json.dumps(never_running), encoding="utf-8")
        never_report = root / "memory-before-report.json"
        never_exit = run_manifest(
            never_path, runner, root, never_report, set(), False, 30.0
        )
        never_data = json.loads(never_report.read_text(encoding="utf-8"))
        if never_exit == 0 or never_data["tests"][0]["status"] != "timeout":
            raise RuntimeError("une réussite mémoire a précédé l'état intermédiaire exigé")

        invalid_manifest = json.loads(json.dumps(raw_manifest))
        del invalid_manifest["tests"][1]["success"]["memory_stability_samples"]
        try:
            validate_manifest(invalid_manifest)
        except ManifestError:
            pass
        else:
            raise RuntimeError("une réussite mémoire sans stabilité explicite a été acceptée")

        traversal_manifest = json.loads(json.dumps(raw_manifest))
        traversal_manifest["tests"][0]["rom"]["path"] = "../outside.gb"
        try:
            validate_manifest(traversal_manifest)
        except ManifestError:
            pass
        else:
            raise RuntimeError("une traversée hors de la racine a été acceptée")

        unknown_license = json.loads(json.dumps(raw_manifest))
        unknown_license["tests"][0]["rom"]["license"] = "unknown"
        try:
            validate_manifest(unknown_license)
        except ManifestError:
            pass
        else:
            raise RuntimeError("une licence inconnue a été acceptée")

    print("PASS conformance-manifest-self-test")
    return 0


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Exécute des ROMs de conformité GB/GBC externes vérifiées."
    )
    parser.add_argument("manifest", nargs="?", type=Path)
    parser.add_argument("--runner", type=Path)
    parser.add_argument("--rom-root", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--category", action="append", choices=sorted(CATEGORIES), default=[])
    parser.add_argument("--id", action="append", default=[], dest="identifiers")
    parser.add_argument("--allow-missing", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--wall-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        if arguments.wall_timeout_seconds <= 0:
            raise ManifestError("--wall-timeout-seconds doit être positif")
        if arguments.self_test:
            if arguments.manifest is not None:
                raise ManifestError("--self-test n'accepte pas de manifeste")
            if arguments.runner is None:
                raise ManifestError("--runner est requis par --self-test")
            return self_test(arguments.runner)
        if arguments.manifest is None:
            raise ManifestError("chemin du manifeste absent")
        categories = set(arguments.category)
        if arguments.list:
            return list_manifest(arguments.manifest, categories)
        if arguments.runner is None or arguments.rom_root is None or arguments.report is None:
            raise ManifestError(
                "--runner, --rom-root et --report sont requis pour l'exécution"
            )
        return run_manifest(
            arguments.manifest,
            arguments.runner,
            arguments.rom_root,
            arguments.report,
            categories,
            arguments.allow_missing,
            arguments.wall_timeout_seconds,
            set(arguments.identifiers),
        )
    except (ManifestError, OSError, RuntimeError) as error:
        print(f"ERROR {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
