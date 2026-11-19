"""textkit.words — the fixture cpp-anything is tested against.

Every function here is a small behaviour that is easy to get wrong on the way to
C++: unicode offsets, whitespace classes, integer parsing, exception types.
"""


def slug(text: str) -> str:
    """Lowercase, keep ascii alphanumerics, collapse everything else to '-'."""
    out = []
    dash = False
    for ch in text.strip().lower():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
            dash = False
        elif not dash and out:
            out.append("-")
            dash = True
    while out and out[-1] == "-":
        out.pop()
    return "".join(out)


def truncate(text: str, limit: int = 12) -> str:
    """Cut to `limit` code points, adding an ellipsis."""
    if limit < 0:
        raise ValueError(f"limit must be >= 0, got {limit}")
    if len(text) <= limit:
        return text
    return text[:limit] + "…"


def word_count(text: str) -> int:
    return len(text.split())


def digits_only(text: str) -> int:
    return int(text.strip())


def summarize(text: str, max_words: int = 4) -> list[str]:
    words = text.split()
    if not words:
        raise ValueError("nothing to summarize")
    return [word for word in words[:max_words]]


def require_positive(n: int) -> int:
    if n <= 0:
        raise ValueError("expected a positive number")
    return n * 2


def _private_helper():
    return "not ported: leading underscore"


class Splitter:
    """Classes stay in Python; the analyzer reports them as a risk."""

    def split(self, text: str) -> list[str]:
        return text.split()


def _cached(fn):
    """Decorated functions are reported as skipped, not silently dropped."""
    return fn


@_cached
def memoized_score(text: str) -> int:
    return len(text)
