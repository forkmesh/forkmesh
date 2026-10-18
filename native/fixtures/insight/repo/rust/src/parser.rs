use super::Visitor;

pub enum Token {
    Word(String),
    Space,
}

pub fn parse(text: &str) -> Vec<Token> {
    let mut out = Vec::new();
    for part in text.split(' ') {
        if part.is_empty() {
            out.push(Token::Space);
        } else {
            out.push(Token::Word(part.to_string()));
        }
    }
    out
}
