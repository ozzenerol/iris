package iris.intellij;

import com.intellij.psi.TokenType;
import com.intellij.psi.tree.IElementType;
import com.intellij.psi.tree.IFileElementType;
import com.intellij.psi.tree.TokenSet;

public final class IrisTokenTypes {
    public static final IFileElementType FILE = new IFileElementType(IrisLanguage.INSTANCE);

    public static final IElementType LINE_COMMENT = new Token("LINE_COMMENT");
    public static final IElementType BLOCK_COMMENT = new Token("BLOCK_COMMENT");
    public static final IElementType STRING = new Token("STRING");
    public static final IElementType CHAR = new Token("CHAR");
    public static final IElementType INTERPOLATION = new Token("INTERPOLATION");
    public static final IElementType NUMBER = new Token("NUMBER");
    public static final IElementType KEYWORD = new Token("KEYWORD");
    public static final IElementType CONSTANT = new Token("CONSTANT");
    public static final IElementType PRIMITIVE = new Token("PRIMITIVE");
    public static final IElementType TYPE_NAME = new Token("TYPE_NAME");
    public static final IElementType FUNCTION = new Token("FUNCTION");
    public static final IElementType BUILTIN = new Token("BUILTIN");
    public static final IElementType ATTRIBUTE = new Token("ATTRIBUTE");
    public static final IElementType IDENTIFIER = new Token("IDENTIFIER");
    public static final IElementType LPAREN = new Token("(");
    public static final IElementType RPAREN = new Token(")");
    public static final IElementType LBRACE = new Token("{");
    public static final IElementType RBRACE = new Token("}");
    public static final IElementType LBRACKET = new Token("[");
    public static final IElementType RBRACKET = new Token("]");
    public static final IElementType COMMA = new Token(",");
    public static final IElementType DOT = new Token(".");
    public static final IElementType OPERATOR = new Token("OPERATOR");
    public static final IElementType WHITE_SPACE = TokenType.WHITE_SPACE;
    public static final IElementType BAD_CHARACTER = TokenType.BAD_CHARACTER;

    public static final TokenSet COMMENTS = TokenSet.create(LINE_COMMENT, BLOCK_COMMENT);
    public static final TokenSet STRINGS = TokenSet.create(STRING, CHAR);

    private IrisTokenTypes() {
    }

    static final class Token extends IElementType {
        Token(String name) {
            super(name, IrisLanguage.INSTANCE);
        }
    }
}
