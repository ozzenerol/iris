package iris.intellij;

import com.intellij.lang.BracePair;
import com.intellij.lang.PairedBraceMatcher;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;

public final class IrisBraceMatcher implements PairedBraceMatcher {
    private static final BracePair[] PAIRS = {
        new BracePair(IrisTokenTypes.LBRACE, IrisTokenTypes.RBRACE, true),
        new BracePair(IrisTokenTypes.LPAREN, IrisTokenTypes.RPAREN, false),
        new BracePair(IrisTokenTypes.LBRACKET, IrisTokenTypes.RBRACKET, false),
    };

    @Override
    public BracePair[] getPairs() {
        return PAIRS;
    }

    @Override
    public boolean isPairedBracesAllowedBeforeType(IElementType lbraceType, IElementType contextType) {
        return true;
    }

    @Override
    public int getCodeConstructStart(PsiFile file, int openingBraceOffset) {
        return openingBraceOffset;
    }
}
