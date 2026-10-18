mod parser;
use crate::parser::Token;
use std::collections::HashMap;

pub trait Visitor {
    fn visit(&self, token: &Token) -> bool;
}

pub fn count(tokens: &[Token]) -> usize {
    let mut seen: HashMap<String, usize> = HashMap::new();
    for token in tokens {
        match token {
            Token::Word(w) if !w.is_empty() => *seen.entry(w.clone()).or_insert(0) += 1,
            _ => {}
        }
    }
    seen.len()
}
