import argparse
import json
import sys
import urllib.error
import urllib.request


def make_bar(pct: int, width: int = 20) -> str:
    pct = max(0, min(100, pct))
    filled = round((pct / 100) * width)
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def main():
    parser = argparse.ArgumentParser(description="Tampilkan Antigravity Quota dari HTTP server")
    parser.add_argument("--url", default="http://127.0.0.1:8787/quota", help="URL endpoint /quota")
    args = parser.parse_args()

    try:
        with urllib.request.urlopen(args.url, timeout=5) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as exc:
        print(f"Gagal terhubung ke {args.url}: {exc.reason}", file=sys.stderr)
        sys.exit(1)

    print("=== Antigravity Quota ===")
    print(f"Plan   : {data.get('plan', '-')}")
    print(f"Status : {data.get('status', '-')}")
    print(f"Updated: {data.get('updatedAt') or '-'}")
    if data.get("error"):
        print(f"Error  : {data['error']}")
    print()

    items = data.get("items", [])
    if not items:
        print("Belum ada data quota.")
    else:
        for item in items:
            name = item.get("name", "-")
            pct = int(item.get("pct", 0))
            reset = item.get("reset", "-")
            print(f"{name:<18} {make_bar(pct)} {pct:3d}%  ({reset})")

    print()
    print(f"ESP32 Line: {data.get('line', '-')}")


if __name__ == "__main__":
    main()
