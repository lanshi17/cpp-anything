"""A tiny CLI: the app-mode fixture for `cpp-anything app record`."""
import sys


def slug(text: str) -> str:
    out = []
    dash = False
    for ch in text.strip().lower():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
            dash = False
        elif not dash and out:
            out.append("-")
            dash = True
    return "".join(out).strip("-")


def main(argv: list[str]) -> int:
    command = argv[1] if len(argv) > 1 else "help"
    if command == "slug":
        print(slug(" ".join(argv[2:])))
        return 0
    if command == "count":
        print(len(sys.stdin.read().split()))
        return 0
    if command == "fail":
        print("boom: the fixture fails on purpose", file=sys.stderr)
        return 2
    print("usage: slugify slug <words...> | count | fail", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
