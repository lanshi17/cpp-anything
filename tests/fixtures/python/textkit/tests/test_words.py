"""The fixture's own suite: `cpp-anything record --from-tests` captures its calls."""
import pytest

from textkit import words


def test_slug_keeps_ascii_words():
    assert words.slug("Hello, World!") == "hello-world"


def test_truncate_counts_code_points():
    assert words.truncate("café — naïve", 4) == "café…"


def test_truncate_rejects_negative_limits():
    with pytest.raises(ValueError):
        words.truncate("anything", -1)


def test_digits_only_parses_signed_numbers():
    assert words.digits_only("  -42  ") == -42


def test_summarize_keeps_the_first_words():
    assert words.summarize("one two three four five", 2) == ["one", "two"]


def test_word_count_splits_on_whitespace():
    assert words.word_count("one\ntwo\tthree") == 3
