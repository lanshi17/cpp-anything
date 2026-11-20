//! Generated inputs, shared by the language plugins that record out-of-process.
//!
//! Deterministic, biased towards edges, and inside int64: a value the wire cannot
//! carry is a risk the analyzer reports, not a case a signature can never satisfy.

use crate::inventory::Function;
use crate::json::Json;
use crate::types::Ty;

const STRINGS: &[&str] = &[
    "",
    " ",
    "Hello World",
    "  Mixed CASE  ",
    "café — naïve",
    "a",
    "123",
    "-",
    "trailing---",
    " leading",
    "one\ntwo",
    "Москва",
    "0",
    "a-b_c",
    "x",
];
const INTS: &[i64] = &[
    0, 1, -1, 7, 42, 12, 100, -100, 10_000, 3, 19, 255, -7, 2_000_000, 8, 11,
];
const FLOATS: &[f64] = &[
    0.0, -0.0, 1.5, -2.25, 0.1, 2.0, -0.5, 1e-9, 123456.789, -1.0, 7.0, 1e6, 3.25, -99.5, 0.25,
];

fn values_for(ty: &Ty) -> Vec<Json> {
    match ty {
        Ty::Str => STRINGS.iter().map(|text| Json::str(*text)).collect(),
        Ty::Int => INTS.iter().map(|value| Json::Int(*value)).collect(),
        Ty::Float => FLOATS.iter().map(|value| Json::Real(*value)).collect(),
        Ty::Bool => vec![Json::Bool(true), Json::Bool(false)],
        Ty::None => vec![Json::Null],
        Ty::List(inner) => match inner.as_ref() {
            Ty::Str => vec![
                Json::Array(Vec::new()),
                Json::Array(vec![Json::str("a")]),
                Json::Array(vec![Json::str("a"), Json::str("b"), Json::str("c")]),
                Json::Array(vec![Json::str(""), Json::str(" ")]),
                Json::Array(vec![Json::str("café"), Json::str("Mixed CASE")]),
            ],
            Ty::Int => vec![
                Json::Array(Vec::new()),
                Json::Array(vec![Json::Int(0)]),
                Json::Array(vec![Json::Int(1), Json::Int(2), Json::Int(3)]),
                Json::Array(vec![Json::Int(-1), Json::Int(0), Json::Int(1)]),
            ],
            _ => vec![Json::Array(Vec::new()), Json::Array(vec![Json::Null])],
        },
        Ty::Dict(value) => match value.as_ref() {
            Ty::Str => vec![
                Json::Object(Vec::new()),
                Json::obj([("a", Json::str("x"))]),
                Json::obj([("a", Json::str("1")), ("b", Json::str("2"))]),
            ],
            _ => vec![
                Json::Object(Vec::new()),
                Json::obj([("a", Json::Int(1))]),
                Json::obj([("n", Json::Int(-1)), ("m", Json::Int(2))]),
            ],
        },
        Ty::Optional(inner) => {
            let mut values = vec![Json::Null];
            values.extend(values_for(inner));
            values.truncate(8);
            values
        }
        _ => vec![
            Json::Null,
            Json::str("x"),
            Json::Int(0),
            Json::Real(1.5),
            Json::Array(vec![Json::Int(1)]),
            Json::obj([("a", Json::Int(1))]),
            Json::Bool(true),
        ],
    }
}

/// `{"id","fn","args"}` calls, one per generated argument combination.
pub fn calls(functions: &[Function], count: usize) -> Vec<Json> {
    let mut out = Vec::new();
    for function in functions {
        if function.params.is_empty() {
            out.push(Json::obj([
                ("id", Json::str(format!("{}:0", function.name))),
                ("fn", Json::str(function.name.clone())),
                ("args", Json::Array(Vec::new())),
            ]));
            continue;
        }
        let corpora: Vec<Vec<Json>> = function
            .params
            .iter()
            .map(|param| values_for(&param.ty))
            .collect();
        let total = corpora
            .iter()
            .map(Vec::len)
            .max()
            .unwrap_or(0)
            .min(count.max(1));
        for index in 0..total {
            let args = corpora
                .iter()
                .map(|values| values[index % values.len()].clone())
                .collect::<Vec<_>>();
            out.push(Json::obj([
                ("id", Json::str(format!("{}:{index}", function.name))),
                ("fn", Json::str(function.name.clone())),
                ("args", Json::Array(args)),
            ]));
        }
    }
    out
}
