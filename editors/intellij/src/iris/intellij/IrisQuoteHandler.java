package iris.intellij;

import com.intellij.codeInsight.editorActions.SimpleTokenSetQuoteHandler;

public final class IrisQuoteHandler extends SimpleTokenSetQuoteHandler {
    public IrisQuoteHandler() {
        super(IrisTokenTypes.STRING, IrisTokenTypes.CHAR);
    }
}
