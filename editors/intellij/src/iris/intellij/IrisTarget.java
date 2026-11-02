package iris.intellij;

import com.intellij.openapi.editor.Document;
import com.intellij.psi.PsiDocumentManager;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** What a place in an Iris file runs: the program, one test or one bench. */
record IrisTarget(String command, String filter, int line) {
    private static final Pattern BLOCK = Pattern.compile("^(test|bench)\\s+\"((?:[^\"\\\\]|\\\\.)*)\"");
    private static final Pattern MAIN = Pattern.compile("^!?[A-Za-z_][\\w\\[\\]]*\\s+main\\s*\\(");

    /** The test, bench or main declared on the line of element, or null. */
    static IrisTarget onLine(PsiElement element) {
        PsiFile file = element.getContainingFile();
        if (!(file instanceof IrisFile)) {
            return null;
        }
        Document doc = PsiDocumentManager.getInstance(file.getProject()).getDocument(file);
        if (doc == null) {
            return null;
        }
        int offset = element.getTextRange().getStartOffset();
        if (offset >= doc.getTextLength()) {
            return null;
        }
        int line = doc.getLineNumber(offset);
        String text = doc.getCharsSequence()
            .subSequence(doc.getLineStartOffset(line), doc.getLineEndOffset(line)).toString();
        Matcher m = BLOCK.matcher(text);
        if (m.find()) {
            return new IrisTarget(m.group(1), m.group(2), line);
        }
        if (MAIN.matcher(text).find()) {
            return new IrisTarget("run", "", line);
        }
        return null;
    }

    /** What running the whole file does: run when it has main, else its tests. */
    static IrisTarget ofFile(PsiFile file) {
        String text = file.getText();
        boolean hasMain = Pattern.compile("(?m)^!?[A-Za-z_][\\w\\[\\]]*\\s+main\\s*\\(").matcher(text).find();
        return new IrisTarget(hasMain ? "run" : "test", "", -1);
    }
}
