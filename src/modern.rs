//! What to modernize: the C idioms a C-style C++ unit uses, and the C++20/23
//! facility that replaces each one.
//!
//! This is the point of the `cpp` language in cpp-anything: the port is not just
//! behaviour-preserving, it has to be *modern* — and which facilities are available
//! is answered by the same matrix as everything else (`CPPAN_HAS_*`).

use crate::langc::CFunction;

pub struct Opportunity {
    pub function: String,
    pub line: u32,
    pub idiom: &'static str,
    pub replacement: &'static str,
    /// Feature-test macro the replacement needs, when it needs one.
    pub feature: Option<&'static str>,
}

struct Rule {
    needle: &'static str,
    /// A second token the line must contain: `char` is only a raw buffer when an
    /// array follows it.
    also: Option<&'static str>,
    idiom: &'static str,
    replacement: &'static str,
    feature: Option<&'static str>,
}

const RULES: &[Rule] = &[
    Rule {
        needle: "malloc(",
        also: None,
        idiom: "malloc/free",
        replacement: "std::vector / std::unique_ptr / std::array",
        feature: None,
    },
    Rule {
        needle: "free(",
        also: None,
        idiom: "manual free",
        replacement: "RAII: std::vector / std::unique_ptr",
        feature: None,
    },
    Rule {
        needle: "new ",
        also: None,
        idiom: "raw new",
        replacement: "std::make_unique / std::make_shared",
        feature: None,
    },
    Rule {
        needle: "delete",
        also: None,
        idiom: "raw delete",
        replacement: "RAII: std::unique_ptr / std::shared_ptr",
        feature: None,
    },
    Rule {
        needle: "NULL",
        also: None,
        idiom: "NULL",
        replacement: "nullptr",
        feature: None,
    },
    Rule {
        needle: "printf(",
        also: None,
        idiom: "printf",
        replacement: "std::print / std::format",
        feature: Some("__cpp_lib_print"),
    },
    Rule {
        needle: "sprintf(",
        also: None,
        idiom: "sprintf",
        replacement: "std::format",
        feature: Some("__cpp_lib_format"),
    },
    Rule {
        needle: "strcpy",
        also: None,
        idiom: "strcpy",
        replacement: "std::string assignment / string_view copy",
        feature: Some("__cpp_lib_string_view"),
    },
    Rule {
        needle: "strlen",
        also: None,
        idiom: "strlen in a loop",
        replacement: "std::string_view::size() / std::ranges::distance",
        feature: Some("__cpp_lib_string_view"),
    },
    Rule {
        needle: "memcpy",
        also: None,
        idiom: "memcpy",
        replacement: "std::copy / std::ranges::copy, or a typed container",
        feature: Some("__cpp_lib_ranges"),
    },
    Rule {
        needle: "typedef struct",
        also: None,
        idiom: "typedef struct",
        replacement: "plain struct (no typedef needed in C++)",
        feature: None,
    },
    Rule {
        needle: "(char*)",
        also: None,
        idiom: "C-style cast",
        replacement: "static_cast<char*> / reinterpret_cast where unavoidable",
        feature: None,
    },
    Rule {
        needle: "(int)",
        also: None,
        idiom: "C-style cast",
        replacement: "static_cast<int> / std::size_t with gsl::narrow semantics",
        feature: None,
    },
    Rule {
        needle: "(double)",
        also: None,
        idiom: "C-style cast",
        replacement: "static_cast<double>",
        feature: None,
    },
    Rule {
        needle: "for (int ",
        also: None,
        idiom: "index loop",
        replacement: "range-for / std::views on a span or container",
        feature: Some("__cpp_lib_ranges"),
    },
    Rule {
        needle: "char ",
        also: Some("["),
        idiom: "raw char buffer",
        replacement: "std::string / std::array<char, N> / std::span",
        feature: Some("__cpp_lib_span"),
    },
];

pub fn scan(source: &str, functions: &[CFunction]) -> Vec<Opportunity> {
    let lines: Vec<&str> = source.lines().collect();
    let starts: Vec<(u32, &str)> = functions
        .iter()
        .map(|function| (function.line.max(1), function.name.as_str()))
        .collect();
    let mut out = Vec::new();
    let mut seen: std::collections::BTreeSet<(String, u32, &'static str)> =
        std::collections::BTreeSet::new();
    for (index, line) in lines.iter().enumerate() {
        let number = index as u32 + 1;
        let code = line.split("//").next().unwrap_or("");
        if code.trim_start().starts_with('*') || code.trim_start().starts_with("/*") {
            continue;
        }
        for rule in RULES {
            if !code.contains(rule.needle) {
                continue;
            }
            if let Some(also) = rule.also {
                if !code.contains(also) {
                    continue;
                }
            }
            let function = starts
                .iter()
                .filter(|(start, _)| *start <= number)
                .max_by_key(|(start, _)| *start)
                .map(|(_, name)| (*name).to_string())
                .unwrap_or_else(|| "<top level>".to_string());
            if !seen.insert((function.clone(), number, rule.idiom)) {
                continue;
            }
            out.push(Opportunity {
                function,
                line: number,
                idiom: rule.idiom,
                replacement: rule.replacement,
                feature: rule.feature,
            });
        }
    }
    out
}

impl Opportunity {
    pub fn describe(&self) -> String {
        match self.feature {
            Some(feature) => format!(
                "modernize line {}: {} → {} (needs `{}`)",
                self.line, self.idiom, self.replacement, feature
            ),
            None => format!(
                "modernize line {}: {} → {}",
                self.line, self.idiom, self.replacement
            ),
        }
    }
}
