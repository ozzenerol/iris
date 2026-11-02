package iris.intellij;

import com.intellij.lang.Language;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.project.Project;
import com.intellij.psi.codeStyle.lineIndent.LineIndentProvider;

/** The indent of a new line: one level more after a line that opens {, ( or [, one less on a line that starts with }, ) or ]. */
public final class IrisLineIndentProvider implements LineIndentProvider {
    private static final String LEVEL = "    ";

    @Override
    public String getLineIndent(Project project, Editor editor, Language language, int offset) {
        Document doc = editor.getDocument();
        CharSequence text = doc.getCharsSequence();
        int line = doc.getLineNumber(offset);
        String rest = text.subSequence(offset, doc.getLineEndOffset(line)).toString().trim();
        int prev = line - 1;
        String above = null;
        for (; prev >= 0; prev--) {
            String t = text.subSequence(doc.getLineStartOffset(prev), doc.getLineEndOffset(prev)).toString();
            if (!t.isBlank()) {
                above = t;
                break;
            }
        }
        if (above == null) {
            return "";
        }
        String indent = above.substring(0, above.length() - above.stripLeading().length());
        String code = code(above);
        boolean opens = code.endsWith("{") || code.endsWith("(") || code.endsWith("[");
        boolean closes = !rest.isEmpty() && "})]".indexOf(rest.charAt(0)) >= 0;
        if (opens && !closes) {
            return indent + LEVEL;
        }
        if (!opens && closes) {
            return indent.endsWith(LEVEL) ? indent.substring(0, indent.length() - LEVEL.length())
                : indent.endsWith("\t") ? indent.substring(0, indent.length() - 1) : "";
        }
        return indent;
    }

    /** The line without its // comment and trailing blanks. */
    private static String code(String line) {
        char quote = 0;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (quote != 0) {
                if (c == '\\') {
                    i++;
                } else if (c == quote) {
                    quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '/' && i + 1 < line.length() && line.charAt(i + 1) == '/') {
                return line.substring(0, i).strip();
            }
        }
        return line.strip();
    }

    @Override
    public boolean isSuitableFor(Language language) {
        return language instanceof IrisLanguage;
    }
}
