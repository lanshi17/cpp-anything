//! Word helpers: the supported Rust subset (str, usize, f64, bool, String).

/// Count whitespace-separated words.
pub fn word_count(text: &str) -> usize {
    text.split_whitespace().count()
}

/// Keep ASCII digits only.
pub fn digits_only(text: &str) -> String {
    text.chars().filter(|ch| ch.is_ascii_digit()).collect()
}

/// Count non-overlapping occurrences of `needle`.
pub fn repeat_count(text: &str, needle: &str) -> usize {
    if needle.is_empty() {
        return text.chars().count() + 1;
    }
    text.matches(needle).count()
}

/// Mean word length in bytes, or 0.0 for empty input.
pub fn average_word_length(text: &str) -> f64 {
    let words: Vec<&str> = text.split_whitespace().collect();
    if words.is_empty() {
        return 0.0;
    }
    let total: usize = words.iter().map(|word| word.len()).sum();
    total as f64 / words.len() as f64
}

/// Lowercase, runs of non-alphanumerics become one '-', trimmed.
pub fn slugify(text: &str) -> String {
    let mut out = String::new();
    let mut dash = false;
    for ch in text.chars() {
        if ch.is_alphanumeric() {
            for lower in ch.to_lowercase() {
                out.push(lower);
            }
            dash = false;
        } else if !out.is_empty() && !dash {
            out.push('-');
            dash = true;
        }
    }
    while out.ends_with('-') {
        out.pop();
    }
    out
}

/// True when the character sequence reads the same both ways.
pub fn is_palindrome(text: &str) -> bool {
    let chars: Vec<char> = text.chars().flat_map(|ch| ch.to_lowercase()).collect();
    let left: Vec<char> = chars
        .iter()
        .copied()
        .filter(|ch| ch.is_alphanumeric())
        .collect();
    left.iter().copied().eq(left.iter().copied().rev())
}

/// First character of each whitespace word, uppercased.
pub fn initials(text: &str) -> String {
    text.split_whitespace()
        .filter_map(|word| word.chars().next())
        .flat_map(|ch| ch.to_uppercase())
        .collect()
}
