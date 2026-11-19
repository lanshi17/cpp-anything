"""A workload for `cpp-anything profile`: exercise the fixture the way an app would."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "fixtures" / "python" / "textkit"))

from textkit import digits_only, slug, summarize, truncate, word_count  # noqa: E402

WORDS = [
    "Hello World", "  Mixed CASE  ", "café — naïve", "one\ntwo\tthree", "123",
    "Ünïcødé ☃", "trailing---", "line1\nline2", "Москва", "a" * 120,
]


def main() -> None:
    total = 0
    for index in range(3000):
        text = WORDS[index % len(WORDS)]
        total += len(slug(text))
        total += len(truncate(text, 8))
        total += word_count(text)
        try:
            total += digits_only(text)
        except ValueError:
            total += 1
        total += len(summarize(text, 2))
    print(total)


if __name__ == "__main__":
    main()
