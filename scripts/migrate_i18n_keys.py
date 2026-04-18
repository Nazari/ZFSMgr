#!/usr/bin/env python3
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
I18N = ROOT / "i18n"
FILES = [I18N / "es.json", I18N / "en.json", I18N / "zh.json"]
LEGACY = I18N / "legacy_keys.json"


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def dump_json(path: Path, data):
    text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True)
    path.write_text(text + "\n", encoding="utf-8")


def make_slug(text: str) -> str:
    s = text.lower()
    s = re.sub(r"[^a-z0-9]+", "_", s)
    s = re.sub(r"_+", "_", s).strip("_")
    if not s:
        s = "text"
    return s


def make_id(text: str, used: set[str]) -> str:
    slug = make_slug(text)
    h = hashlib.sha1(text.encode("utf-8")).hexdigest()
    for hlen in (6, 8, 10):
        for slen in range(10, 2, -1):
            key = f"t_{slug[:slen]}_{h[:hlen]}"
            if len(key) > 20:
                continue
            if key not in used:
                used.add(key)
                return key
    i = 0
    while True:
        key = f"t_id_{h[:10-len(str(i))]}{i}"
        key = key[:20]
        if key not in used:
            used.add(key)
            return key
        i += 1


def main():
    parser = argparse.ArgumentParser(
        description="Migra las claves legacy de i18n a ids cortos y actualiza legacy_keys.json."
    )
    parser.parse_args()

    docs = [load_json(p) for p in FILES]
    all_keys = []
    seen = set()
    for d in docs:
        t = d.get("translations", {})
        for k in t.keys():
            if k not in seen:
                seen.add(k)
                all_keys.append(k)

    used_ids = set()
    key_map = {}
    for old in sorted(all_keys):
        key_map[old] = make_id(old, used_ids)

    for path, doc in zip(FILES, docs):
        old_t = doc.get("translations", {})
        new_t = {}
        for old_k, value in old_t.items():
            new_t[key_map[old_k]] = value
        doc["translations"] = new_t
        dump_json(path, doc)

    legacy = {
        "version": 1,
        "legacy_keys": {k: key_map[k] for k in sorted(key_map.keys())},
    }
    dump_json(LEGACY, legacy)
    print(f"Migrated {len(key_map)} keys.")


if __name__ == "__main__":
    main()
