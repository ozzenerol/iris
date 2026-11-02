package iris.intellij;

import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.HighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.tree.IElementType;

import java.util.HashMap;
import java.util.Map;

import static com.intellij.openapi.editor.colors.TextAttributesKey.createTextAttributesKey;

public final class IrisSyntaxHighlighter extends SyntaxHighlighterBase {
    public static final TextAttributesKey LINE_COMMENT = createTextAttributesKey("IRIS_LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
    public static final TextAttributesKey BLOCK_COMMENT = createTextAttributesKey("IRIS_BLOCK_COMMENT", DefaultLanguageHighlighterColors.BLOCK_COMMENT);
    public static final TextAttributesKey STRING = createTextAttributesKey("IRIS_STRING", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey INTERPOLATION = createTextAttributesKey("IRIS_INTERPOLATION", DefaultLanguageHighlighterColors.VALID_STRING_ESCAPE);
    public static final TextAttributesKey NUMBER = createTextAttributesKey("IRIS_NUMBER", DefaultLanguageHighlighterColors.NUMBER);
    public static final TextAttributesKey KEYWORD = createTextAttributesKey("IRIS_KEYWORD", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey CONSTANT = createTextAttributesKey("IRIS_CONSTANT", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey PRIMITIVE = createTextAttributesKey("IRIS_PRIMITIVE", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey TYPE_NAME = createTextAttributesKey("IRIS_TYPE_NAME", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey FUNCTION = createTextAttributesKey("IRIS_FUNCTION", DefaultLanguageHighlighterColors.FUNCTION_CALL);
    public static final TextAttributesKey BUILTIN = createTextAttributesKey("IRIS_BUILTIN", DefaultLanguageHighlighterColors.STATIC_METHOD);
    public static final TextAttributesKey ATTRIBUTE = createTextAttributesKey("IRIS_ATTRIBUTE", DefaultLanguageHighlighterColors.METADATA);
    public static final TextAttributesKey IDENTIFIER = createTextAttributesKey("IRIS_IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER);
    public static final TextAttributesKey PARENS = createTextAttributesKey("IRIS_PARENS", DefaultLanguageHighlighterColors.PARENTHESES);
    public static final TextAttributesKey BRACES = createTextAttributesKey("IRIS_BRACES", DefaultLanguageHighlighterColors.BRACES);
    public static final TextAttributesKey BRACKETS = createTextAttributesKey("IRIS_BRACKETS", DefaultLanguageHighlighterColors.BRACKETS);
    public static final TextAttributesKey COMMA = createTextAttributesKey("IRIS_COMMA", DefaultLanguageHighlighterColors.COMMA);
    public static final TextAttributesKey DOT = createTextAttributesKey("IRIS_DOT", DefaultLanguageHighlighterColors.DOT);
    public static final TextAttributesKey OPERATOR = createTextAttributesKey("IRIS_OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    public static final TextAttributesKey BAD_CHARACTER = createTextAttributesKey("IRIS_BAD_CHARACTER", HighlighterColors.BAD_CHARACTER);

    private static final Map<IElementType, TextAttributesKey> KEYS = new HashMap<>();

    static {
        KEYS.put(IrisTokenTypes.LINE_COMMENT, LINE_COMMENT);
        KEYS.put(IrisTokenTypes.BLOCK_COMMENT, BLOCK_COMMENT);
        KEYS.put(IrisTokenTypes.STRING, STRING);
        KEYS.put(IrisTokenTypes.CHAR, STRING);
        KEYS.put(IrisTokenTypes.INTERPOLATION, INTERPOLATION);
        KEYS.put(IrisTokenTypes.NUMBER, NUMBER);
        KEYS.put(IrisTokenTypes.KEYWORD, KEYWORD);
        KEYS.put(IrisTokenTypes.CONSTANT, CONSTANT);
        KEYS.put(IrisTokenTypes.PRIMITIVE, PRIMITIVE);
        KEYS.put(IrisTokenTypes.TYPE_NAME, TYPE_NAME);
        KEYS.put(IrisTokenTypes.FUNCTION, FUNCTION);
        KEYS.put(IrisTokenTypes.BUILTIN, BUILTIN);
        KEYS.put(IrisTokenTypes.ATTRIBUTE, ATTRIBUTE);
        KEYS.put(IrisTokenTypes.IDENTIFIER, IDENTIFIER);
        KEYS.put(IrisTokenTypes.LPAREN, PARENS);
        KEYS.put(IrisTokenTypes.RPAREN, PARENS);
        KEYS.put(IrisTokenTypes.LBRACE, BRACES);
        KEYS.put(IrisTokenTypes.RBRACE, BRACES);
        KEYS.put(IrisTokenTypes.LBRACKET, BRACKETS);
        KEYS.put(IrisTokenTypes.RBRACKET, BRACKETS);
        KEYS.put(IrisTokenTypes.COMMA, COMMA);
        KEYS.put(IrisTokenTypes.DOT, DOT);
        KEYS.put(IrisTokenTypes.OPERATOR, OPERATOR);
        KEYS.put(IrisTokenTypes.BAD_CHARACTER, BAD_CHARACTER);
    }

    @Override
    public Lexer getHighlightingLexer() {
        return new IrisLexer();
    }

    @Override
    public TextAttributesKey[] getTokenHighlights(IElementType tokenType) {
        return pack(KEYS.get(tokenType));
    }
}
