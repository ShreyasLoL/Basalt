#!/usr/bin/env python3
"""Verify native FetchContent pins and generate BASALT's CycloneDX SBOM."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
import uuid
from pathlib import Path
from urllib.parse import quote, urlsplit, urlunsplit


ROOT = Path(__file__).resolve().parents[1]
PIN_LOCK = ROOT / "dependencies" / "cmake-fetchcontent.lock.json"
PACKAGE_LOCK = ROOT / "frontend" / "package-lock.json"
DEFAULT_BOM = ROOT / "sbom" / "basalt.cdx.json"
CM_SOURCE = ROOT / "CMakeLists.txt"


def fail(message: str) -> None:
    raise SystemExit(message)


def verify_fetchcontent_pins(lock: dict) -> None:
    cmake = CM_SOURCE.read_text(encoding="utf-8")
    declarations: dict[str, tuple[str, str]] = {}
    declaration_pattern = re.compile(r"FetchContent_Declare\(\s*([A-Za-z0-9_]+)(.*?)\)", re.DOTALL)
    for match in declaration_pattern.finditer(cmake):
        name, body = match.groups()
        repo_match = re.search(r"\bGIT_REPOSITORY\s+([^\s)]+)", body)
        tag_match = re.search(r"\bGIT_TAG\s+([^\s)]+)", body)
        if not repo_match or not tag_match:
            fail(f"FetchContent dependency {name!r} must declare a repository and immutable GIT_TAG")
        declarations[name] = (repo_match.group(1), tag_match.group(1))

    expected = {item["cmakeName"]: item for item in lock["dependencies"]}
    if declarations.keys() != expected.keys():
        missing = sorted(expected.keys() - declarations.keys())
        extra = sorted(declarations.keys() - expected.keys())
        fail(f"FetchContent lock mismatch; missing={missing}, unlisted={extra}")

    for name, item in expected.items():
        repository, commit = declarations[name]
        if repository != item["repository"]:
            fail(f"FetchContent repository drift for {name}: {repository!r}")
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            fail(f"FetchContent {name} uses a mutable or malformed GIT_TAG: {commit!r}")
        if commit != item["commit"]:
            fail(f"FetchContent commit drift for {name}: expected {item['commit']}, got {commit}")


def project_version() -> str:
    cmake = CM_SOURCE.read_text(encoding="utf-8")
    match = re.search(r"project\(BASALT\s+VERSION\s+([^\s)]+)", cmake)
    if not match:
        fail("Could not read BASALT version from CMakeLists.txt")
    return match.group(1)


def npm_purl(name: str, version: str) -> str:
    encoded_name = quote(name, safe="/-._~")
    encoded_version = quote(version, safe="-._~+")
    return f"pkg:npm/{encoded_name}@{encoded_version}"


def npm_bom_ref(package_key: str) -> str:
    return "urn:basalt:npm:" + quote(package_key, safe="-._~")


def dependency_key(package_key: str, dependency_name: str, package_keys: set[str]) -> str | None:
    prefix = package_key
    while prefix:
        candidate = f"{prefix}/node_modules/{dependency_name}"
        if candidate in package_keys:
            return candidate
        nested = prefix.rfind("/node_modules/")
        if nested < 0:
            break
        prefix = prefix[:nested]
    root_candidate = f"node_modules/{dependency_name}"
    return root_candidate if root_candidate in package_keys else None


def integrity_hashes(integrity: str | None) -> list[dict[str, str]]:
    if not integrity:
        return []
    names = {"sha1": "SHA-1", "sha256": "SHA-256", "sha384": "SHA-384", "sha512": "SHA-512"}
    result: list[dict[str, str]] = []
    for token in integrity.split():
        algorithm, separator, digest = token.partition("-")
        content_type = names.get(algorithm.lower())
        if not separator or not content_type:
            continue
        try:
            base64.b64decode(digest, validate=True)
        except ValueError:
            continue
        result.append({"alg": content_type, "content": digest})
    return result


def safe_distribution_url(value: str | None) -> str | None:
    if not value:
        return None
    parsed = urlsplit(value)
    if parsed.username or parsed.password:
        return None
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path, "", ""))


def build_bom(pin_lock: dict, package_lock: dict, app_version: str) -> dict:
    root_ref = f"pkg:generic/basalt@{app_version}"
    components: list[dict] = []
    dependencies: dict[str, set[str]] = {root_ref: set()}

    for item in pin_lock["dependencies"]:
        component = {
            "bom-ref": item["purl"],
            "type": "library",
            "name": item["name"],
            "version": item["version"],
            "purl": item["purl"],
            "externalReferences": [{"type": "vcs", "url": item["repository"]}],
            "properties": [{"name": "basalt:immutable-commit", "value": item["commit"]}],
        }
        components.append(component)
        dependencies[root_ref].add(component["bom-ref"])
        dependencies[component["bom-ref"]] = set()

    packages = package_lock.get("packages", {})
    package_keys = {key for key, value in packages.items() if key and value.get("version")}
    npm_refs: dict[str, str] = {}
    npm_components: list[dict] = []
    for key in sorted(package_keys):
        info = packages[key]
        name = info.get("name")
        if not name:
            suffix = key.rsplit("node_modules/", 1)[-1]
            name = suffix
        version = info["version"]
        purl = npm_purl(name, version)
        ref = npm_bom_ref(key)
        npm_refs[key] = ref
        component = {
            "bom-ref": ref,
            "type": "library",
            "name": name,
            "version": version,
            "purl": purl,
            "properties": [{"name": "basalt:package-lock-path", "value": key}],
        }
        hashes = integrity_hashes(info.get("integrity"))
        if hashes:
            component["hashes"] = hashes
        resolved = safe_distribution_url(info.get("resolved"))
        if resolved:
            component["externalReferences"] = [{"type": "distribution", "url": resolved}]
        npm_components.append(component)
        components.append(component)

    root_info = packages.get("", {})
    root_dependencies = root_info.get("dependencies", {})
    root_dependencies.update(root_info.get("devDependencies", {}))
    root_dependencies.update(root_info.get("optionalDependencies", {}))
    for name in root_dependencies:
        key = dependency_key("", name, package_keys)
        if key and key in npm_refs:
            dependencies[root_ref].add(npm_refs[key])

    for key, ref in npm_refs.items():
        info = packages[key]
        dependencies[ref] = set()
        required = dict(info.get("dependencies", {}))
        required.update(info.get("optionalDependencies", {}))
        required.update(info.get("peerDependencies", {}))
        for name in required:
            child = dependency_key(key, name, package_keys)
            if child and child in npm_refs:
                dependencies[ref].add(npm_refs[child])

    dependency_records = [
        {"ref": ref, "dependsOn": sorted(refs)} for ref, refs in sorted(dependencies.items())
    ]
    bom_without_serial = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "bom-ref": root_ref,
                "type": "application",
                "name": "BASALT",
                "version": app_version,
                "purl": root_ref,
            }
        },
        "components": sorted(components, key=lambda component: component["bom-ref"]),
        "dependencies": dependency_records,
    }
    content = json.dumps(bom_without_serial, sort_keys=True, separators=(",", ":"))
    bom_without_serial["serialNumber"] = "urn:uuid:" + str(
        uuid.uuid5(uuid.NAMESPACE_URL, hashlib.sha256(content.encode("utf-8")).hexdigest())
    )
    return bom_without_serial


def render_bom(pin_lock: dict, package_lock: dict, app_version: str) -> str:
    bom = build_bom(pin_lock, package_lock, app_version)
    return json.dumps(bom, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify pins and checked-in BOM without writing")
    parser.add_argument("--output", type=Path, default=DEFAULT_BOM, help="CycloneDX output path")
    args = parser.parse_args()

    pin_lock = json.loads(PIN_LOCK.read_text(encoding="utf-8"))
    package_lock = json.loads(PACKAGE_LOCK.read_text(encoding="utf-8"))
    verify_fetchcontent_pins(pin_lock)
    rendered = render_bom(pin_lock, package_lock, project_version())

    if args.check:
        try:
            existing = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            fail(f"CycloneDX BOM is missing: {args.output}")
        if existing != rendered:
            fail(f"CycloneDX BOM is stale; regenerate it with: python {Path(__file__).as_posix()}")
        print(f"FetchContent pins verified; CycloneDX BOM is current at {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"Wrote {args.output} with {len(json.loads(rendered)['components'])} components")
    return 0


if __name__ == "__main__":
    sys.exit(main())
