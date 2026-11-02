package iris.intellij;

import com.intellij.lexer.LexerBase;
import com.intellij.psi.tree.IElementType;

import java.util.Set;

/** Splits Iris source into the tokens the highlighter colours. Every token starts in state 0. */
public final class IrisLexer extends LexerBase {
    static final Set<String> KEYWORDS = Set.of(
        "if", "else", "for", "in", "match", "break", "continue", "return", "struct", "union",
        "enum", "interface", "type", "dyn", "pub", "const", "comptime", "when", "import", "try",
        "catch", "defer", "errdefer", "scoped", "spawn", "select", "chan", "thread",
        "threadlocal", "asm", "test", "not", "and", "or", "is", "bench");
    static final Set<String> CONSTANTS = Set.of("true", "false", "null", "self");
    static final Set<String> PRIMITIVES = Set.of(
        "void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "str",
        "u16be", "u32be", "u64be", "i16be", "i32be", "i64be", "String", "Error", "Duration",
        "Mutex", "Thread", "Atomic", "List", "Map", "Set", "Arena");
    static final Set<String> BUILTINS = Set.of(
        "print", "panic", "format", "alloc", "free", "sizeof", "error", "sleep", "after", "expect");
    private static final String UNITS = "ns us ms s m h KiB MiB GiB";

    private CharSequence buf;
    private int end;
    private int start;
    private int pos;
    private IElementType type;
    /** 0 in code; otherwise kind * 16 + depth: kind 1 is "...", 2 is """...""", depth 0 is the
     * string's text and 1 or more the braces open in a {value} inside it. */
    private int state;

    @Override
    public void start(CharSequence buffer, int startOffset, int endOffset, int initialState) {
        buf = buffer;
        end = endOffset;
        pos = startOffset;
        state = initialState;
        advance();
    }

    @Override
    public int getState() {
        return state;
    }

    @Override
    public IElementType getTokenType() {
        return type;
    }

    @Override
    public int getTokenStart() {
        return start;
    }

    @Override
    public int getTokenEnd() {
        return pos;
    }

    @Override
    public CharSequence getBufferSequence() {
        return buf;
    }

    @Override
    public int getBufferEnd() {
        return end;
    }

    private char at(int i) {
        return i < end ? buf.charAt(i) : '\0';
    }

    private boolean startsWith(String s) {
        for (int i = 0; i < s.length(); i++) {
            if (at(pos + i) != s.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    private static boolean identStart(char c) {
        return Character.isLetter(c) || c == '_';
    }

    private static boolean identChar(char c) {
        return Character.isLetterOrDigit(c) || c == '_';
    }

    @Override
    public void advance() {
        start = pos;
        if (pos >= end) {
            type = null;
            return;
        }
        int kind = state >> 4;
        int depth = state & 15;
        if (kind != 0 && depth == 0) {
            stringText(kind);
            return;
        }
        char c = at(pos);
        if (kind == 1 && c == '\n') {
            /* a line's string left open inside a {value}: it ends with the line */
            state = 0;
            kind = 0;
        }
        if (kind != 0 && c == '{') {
            state = kind << 4 | Math.min(depth + 1, 15);
        } else if (kind != 0 && c == '}') {
            pos++;
            if (depth == 1) {
                state = kind << 4;
                type = IrisTokenTypes.INTERPOLATION;
                return;
            }
            state = kind << 4 | depth - 1;
            type = IrisTokenTypes.RBRACE;
            return;
        }
        if (Character.isWhitespace(c)) {
            while (pos < end && Character.isWhitespace(at(pos))) {
                pos++;
            }
            type = IrisTokenTypes.WHITE_SPACE;
        } else if (startsWith("//")) {
            while (pos < end && at(pos) != '\n') {
                pos++;
            }
            type = IrisTokenTypes.LINE_COMMENT;
        } else if (startsWith("/*")) {
            pos += 2;
            while (pos < end && !startsWith("*/")) {
                pos++;
            }
            pos = Math.min(end, pos + 2);
            type = IrisTokenTypes.BLOCK_COMMENT;
        } else if (startsWith("r\"\"\"")) {
            pos += 4;
            tripleString(false);
        } else if (startsWith("\"\"\"") && kind == 0) {
            pos += 3;
            state = 2 << 4;
            stringText(2);
        } else if (startsWith("r\"")) {
            pos += 2;
            lineString(false);
        } else if (c == '"' && kind != 0) {
            /* a string inside a {value}: its own braces are not read */
            pos++;
            lineString(true);
        } else if (c == '"') {
            pos++;
            state = 1 << 4;
            stringText(1);
        } else if (c == '\'') {
            charLiteral();
        } else if (Character.isDigit(c)) {
            number();
        } else if (c == '@') {
            pos++;
            while (identChar(at(pos))) {
                pos++;
            }
            type = IrisTokenTypes.ATTRIBUTE;
        } else if (identStart(c)) {
            word();
        } else {
            pos++;
            type = switch (c) {
                case '(' -> IrisTokenTypes.LPAREN;
                case ')' -> IrisTokenTypes.RPAREN;
                case '{' -> IrisTokenTypes.LBRACE;
                case '}' -> IrisTokenTypes.RBRACE;
                case '[' -> IrisTokenTypes.LBRACKET;
                case ']' -> IrisTokenTypes.RBRACKET;
                case ',' -> IrisTokenTypes.COMMA;
                case '.' -> IrisTokenTypes.DOT;
                default -> "+-*/%=<>!&|^~?:;#$".indexOf(c) >= 0 ? IrisTokenTypes.OPERATOR : IrisTokenTypes.BAD_CHARACTER;
            };
            if (type == IrisTokenTypes.OPERATOR) {
                while ("+-*/%=<>!&|^~?:".indexOf(at(pos)) >= 0 && !startsWith("//") && !startsWith("/*")) {
                    pos++;
                }
            }
        }
    }

    /** The text of a string up to a {value}, its end or the end of the line. A { that starts a
     * value is a token of its own. */
    private void stringText(int kind) {
        if (at(pos) == '{' && at(pos + 1) != '{' && pos < end && pos > start) {
            type = IrisTokenTypes.STRING;   /* the opening quote, then the { on its own */
            return;
        }
        if (at(pos) == '{' && at(pos + 1) != '{' && pos < end) {
            pos++;
            state = kind << 4 | 1;
            type = IrisTokenTypes.INTERPOLATION;
            return;
        }
        while (pos < end) {
            char ch = at(pos);
            if (kind == 2 && startsWith("\"\"\"")) {
                pos += 3;
                state = 0;
                break;
            }
            if (kind == 1 && ch == '"') {
                pos++;
                state = 0;
                break;
            }
            if (kind == 1 && ch == '\n') {
                state = 0;
                break;
            }
            if (ch == '\\') {
                pos += 2;
            } else if ((ch == '{' && at(pos + 1) == '{') || (ch == '}' && at(pos + 1) == '}')) {
                pos += 2;
            } else if (ch == '{') {
                break;
            } else {
                pos++;
            }
        }
        pos = Math.min(pos, end);
        if (pos >= end && state != 0 && start == pos) {
            state = 0;
        }
        type = IrisTokenTypes.STRING;
    }

    private void tripleString(boolean escapes) {
        while (pos < end && !startsWith("\"\"\"")) {
            pos += escapes && at(pos) == '\\' ? 2 : 1;
        }
        pos = Math.min(end, pos + 3);
        type = IrisTokenTypes.STRING;
    }

    private void lineString(boolean escapes) {
        while (pos < end && at(pos) != '"' && at(pos) != '\n') {
            pos += escapes && at(pos) == '\\' && at(pos + 1) != '\n' ? 2 : 1;
        }
        if (at(pos) == '"') {
            pos++;
        }
        pos = Math.min(end, pos);
        type = IrisTokenTypes.STRING;
    }

    private void charLiteral() {
        int p = pos + 1;
        while (p < end && at(p) != '\'' && at(p) != '\n') {
            p += at(p) == '\\' ? 2 : 1;
        }
        if (at(p) == '\'' && p - pos <= 12) {
            pos = p + 1;
            type = IrisTokenTypes.CHAR;
        } else {
            pos++;
            type = IrisTokenTypes.BAD_CHARACTER;
        }
    }

    private void number() {
        if (at(pos) == '0' && (at(pos + 1) == 'x' || at(pos + 1) == 'b')) {
            pos += 2;
            while (Character.isLetterOrDigit(at(pos)) || at(pos) == '_') {
                pos++;
            }
        } else {
            while (Character.isDigit(at(pos)) || at(pos) == '_') {
                pos++;
            }
            if (at(pos) == '.' && Character.isDigit(at(pos + 1))) {
                pos++;
                while (Character.isDigit(at(pos)) || at(pos) == '_') {
                    pos++;
                }
            }
            if ((at(pos) == 'e' || at(pos) == 'E') && (Character.isDigit(at(pos + 1))
                    || ((at(pos + 1) == '+' || at(pos + 1) == '-') && Character.isDigit(at(pos + 2))))) {
                pos += 2;
                while (Character.isDigit(at(pos))) {
                    pos++;
                }
            }
            int u = pos;
            while (Character.isLetter(at(u))) {
                u++;
            }
            if (u > pos && (" " + UNITS + " ").contains(" " + buf.subSequence(pos, u) + " ")) {
                pos = u;
            }
        }
        type = IrisTokenTypes.NUMBER;
    }

    private void word() {
        while (identChar(at(pos))) {
            pos++;
        }
        String w = buf.subSequence(start, pos).toString();
        int next = pos;
        while (at(next) == ' ' || at(next) == '\t') {
            next++;
        }
        boolean call = at(next) == '(';
        if (KEYWORDS.contains(w) && !(w.equals("bench") && !startsWithQuote(next))) {
            type = IrisTokenTypes.KEYWORD;
        } else if (CONSTANTS.contains(w)) {
            type = IrisTokenTypes.CONSTANT;
        } else if (PRIMITIVES.contains(w)) {
            type = IrisTokenTypes.PRIMITIVE;
        } else if (call && BUILTINS.contains(w)) {
            type = IrisTokenTypes.BUILTIN;
        } else if (call) {
            type = IrisTokenTypes.FUNCTION;
        } else if (Character.isUpperCase(w.charAt(0)) && !w.equals(w.toUpperCase())) {
            type = IrisTokenTypes.TYPE_NAME;
        } else {
            type = IrisTokenTypes.IDENTIFIER;
        }
    }

    private boolean startsWithQuote(int p) {
        return at(p) == '"';
    }
}
